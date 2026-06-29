#include "config_fetch.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include <stdlib.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "cJSON.h"

#include "secrets.h"

static const char *TAG = "orb-cfg";

extern const char gts_root_r4_pem_start[] asm("_binary_gts_root_r4_pem_start");
extern const char gts_root_r4_pem_end[]   asm("_binary_gts_root_r4_pem_end");

#define BODY_MAX 1024

typedef struct {
    char  buf[BODY_MAX];
    int   len;
    bool  overflow;
} body_ctx_t;

void orb_device_id(char *out, size_t out_len)
{
#ifdef DEVICE_ID
    snprintf(out, out_len, "%s", DEVICE_ID);
#else
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_len, "orb-%02X%02X%02X", mac[3], mac[4], mac[5]);
#endif
}

bool orb_sntp_sync(int timeout_ms)
{
    static bool initialized = false;
    if (!initialized) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&cfg);
        initialized = true;
    }

    int waited = 0;
    while (waited < timeout_ms) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(500)) == ESP_OK) {
            break;
        }
        waited += 500;
    }

    time_t now = 0;
    time(&now);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    if (tm_info.tm_year + 1900 >= 2024) {
        ESP_LOGI(TAG, "SNTP synced: %ld", (long)now);
        return true;
    }
    ESP_LOGW(TAG, "SNTP sync incomplete (year=%d)", tm_info.tm_year + 1900);
    return false;
}

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    body_ctx_t *ctx = (body_ctx_t *)evt->user_data;
    if (!ctx || ctx->overflow) return ESP_OK;

    int room = (int)sizeof(ctx->buf) - 1 - ctx->len;
    if (evt->data_len > room) {
        ctx->overflow = true;
        ESP_LOGW(TAG, "config body > %d bytes; truncating", BODY_MAX);
        return ESP_OK;
    }
    memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
    ctx->len += evt->data_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

static bool parse_config(const char *body, OrbConfig *out)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return false;
    }

    bool ok = false;
    const cJSON *id   = cJSON_GetObjectItemCaseSensitive(root, "agent_id");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "agent_name");
    const cJSON *vol  = cJSON_GetObjectItemCaseSensitive(root, "volume");

    if (cJSON_IsString(id) && id->valuestring && id->valuestring[0]) {
        strncpy(out->agent_id, id->valuestring, ORB_AGENT_ID_MAX - 1);
        out->agent_id[ORB_AGENT_ID_MAX - 1] = '\0';

        if (cJSON_IsString(name) && name->valuestring) {
            strncpy(out->agent_name, name->valuestring, ORB_AGENT_NAME_MAX - 1);
            out->agent_name[ORB_AGENT_NAME_MAX - 1] = '\0';
        } else {
            strncpy(out->agent_name, "Unknown", ORB_AGENT_NAME_MAX - 1);
            out->agent_name[ORB_AGENT_NAME_MAX - 1] = '\0';
        }

        out->volume = cJSON_IsNumber(vol) ? vol->valueint : 0;
        ok = true;
    } else {
        ESP_LOGE(TAG, "config missing agent_id");
    }

    cJSON_Delete(root);
    return ok;
}

bool orb_config_fetch(OrbConfig *out)
{
    char device_id[ORB_DEVICE_ID_MAX];
    orb_device_id(device_id, sizeof(device_id));

    char url[256];
    snprintf(url, sizeof(url), "%s?device_id=%s", SUPABASE_CONFIG_URL, device_id);
    ESP_LOGI(TAG, "GET %s", url);

    body_ctx_t ctx = {0};

    esp_http_client_config_t cfg = {
        .url = url,
        .cert_pem = gts_root_r4_pem_start,
        .event_handler = http_event,
        .user_data = &ctx,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http_client_init failed");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        return false;
    }
    if (ctx.overflow || ctx.len == 0) {
        ESP_LOGE(TAG, "bad body (len=%d, overflow=%d)", ctx.len, ctx.overflow);
        return false;
    }

    memset(out, 0, sizeof(*out));
    return parse_config(ctx.buf, out);
}

// ─── NFC tag table download ─────────────────────────────────────────────────
//
// The tag map (UID -> phrase) is hosted as a raw file on the public GitHub repo
// and pulled fresh each session. It's small today (~5 KB) but give it generous
// headroom; the buffer lives in PSRAM so it never pressures internal SRAM.

#define NFC_TAGS_BODY_MAX (32 * 1024)

typedef struct {
    char *buf;
    int   cap;
    int   len;
    bool  overflow;
} tags_ctx_t;

static esp_err_t tags_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    tags_ctx_t *ctx = (tags_ctx_t *)evt->user_data;
    if (!ctx || ctx->overflow) return ESP_OK;

    int room = ctx->cap - 1 - ctx->len;
    if (evt->data_len > room) {
        ctx->overflow = true;
        ESP_LOGW(TAG, "nfc tags body > %d bytes; truncating", ctx->cap);
        return ESP_OK;
    }
    memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
    ctx->len += evt->data_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

char *orb_nfc_tags_fetch(void)
{
    // PSRAM if available, else internal — either way freed by the caller with free().
    char *buf = heap_caps_malloc(NFC_TAGS_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(NFC_TAGS_BODY_MAX);
    if (!buf) {
        ESP_LOGE(TAG, "nfc tags buf alloc (%d B) failed", NFC_TAGS_BODY_MAX);
        return NULL;
    }

    tags_ctx_t ctx = { .buf = buf, .cap = NFC_TAGS_BODY_MAX, .len = 0, .overflow = false };

    ESP_LOGI(TAG, "GET %s", NFC_TAGS_URL);
    esp_http_client_config_t cfg = {
        .url               = NFC_TAGS_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = tags_http_event,
        .user_data         = &ctx,
        .timeout_ms        = 10000,
        .user_agent        = "orb-esp32/nfc-tags",
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "nfc tags http_client_init failed");
        free(buf);
        return NULL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nfc tags HTTP perform: %s", esp_err_to_name(err));
        free(buf);
        return NULL;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "nfc tags HTTP status %d", status);
        free(buf);
        return NULL;
    }
    if (ctx.overflow || ctx.len == 0) {
        ESP_LOGE(TAG, "nfc tags bad body (len=%d, overflow=%d)", ctx.len, ctx.overflow);
        free(buf);
        return NULL;
    }

    ESP_LOGI(TAG, "nfc tags fetched (%d bytes)", ctx.len);
    return buf;
}

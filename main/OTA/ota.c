#include "ota.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "secrets.h"
#include "orb_ui.h"

#define TAG "ota"

// /releases/latest is plenty of metadata for our needs. ~2 KB body; cap is
// generous to absorb release-note bodies without truncating tag_name/asset
// fields. Sits on the http-event task stack — no PSRAM needed.
#define RELEASES_BODY_MAX 8192
#define GITHUB_API_HOST   "api.github.com"

typedef struct {
    char  buf[RELEASES_BODY_MAX];
    int   len;
    bool  overflow;
} body_ctx_t;

static esp_err_t releases_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    body_ctx_t *ctx = (body_ctx_t *)evt->user_data;
    if (!ctx || ctx->overflow) return ESP_OK;

    int room = (int)sizeof(ctx->buf) - 1 - ctx->len;
    if (evt->data_len > room) {
        ctx->overflow = true;
        ESP_LOGW(TAG, "releases body > %d bytes; truncating", RELEASES_BODY_MAX);
        return ESP_OK;
    }
    memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
    ctx->len += evt->data_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

// Walks the GitHub /releases/latest JSON and copies tag_name into out_tag and
// the first asset whose name ends in ".bin" into out_url. Returns true only
// if both were found.
static bool parse_latest_release(const char *body, char *out_tag, size_t tag_sz,
                                                  char *out_url, size_t url_sz)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "release JSON parse failed");
        return false;
    }

    bool ok = false;
    const cJSON *tag    = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");

    if (!cJSON_IsString(tag) || !tag->valuestring) {
        ESP_LOGE(TAG, "release missing tag_name");
        goto done;
    }
    if (!cJSON_IsArray(assets)) {
        ESP_LOGE(TAG, "release missing assets array");
        goto done;
    }

    const cJSON *a = NULL;
    cJSON_ArrayForEach(a, assets) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
        const cJSON *url  = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
        if (!cJSON_IsString(name) || !cJSON_IsString(url)) continue;
        size_t nlen = strlen(name->valuestring);
        if (nlen < 4) continue;
        if (strcmp(name->valuestring + nlen - 4, ".bin") != 0) continue;

        strncpy(out_tag, tag->valuestring, tag_sz - 1);
        out_tag[tag_sz - 1] = '\0';
        strncpy(out_url, url->valuestring, url_sz - 1);
        out_url[url_sz - 1] = '\0';
        ok = true;
        break;
    }
    if (!ok) ESP_LOGE(TAG, "no .bin asset found in latest release");

done:
    cJSON_Delete(root);
    return ok;
}

// Fetches https://api.github.com/repos/<owner>/<repo>/releases/latest using
// the IDF cert bundle. Anonymous request — public repo, no token.
//
// IMPORTANT: body_ctx_t holds an 8 KB buffer; allocate it on the heap, NOT
// the stack. The HTTPS GET nests through mbedTLS whose handshake adds ~5-7
// KB of its own stack pressure on top of whatever we put in this frame.
// Together with 8 KB of ctx we'd blow even an 8 KB task stack and crash
// silently inside esp_http_client_perform.
static bool fetch_latest_release(char *out_tag, size_t tag_sz,
                                 char *out_url, size_t url_sz)
{
    char url[256];
    snprintf(url, sizeof(url), "https://%s/repos/%s/%s/releases/latest",
             GITHUB_API_HOST, OTA_GITHUB_OWNER, OTA_GITHUB_REPO);
    ESP_LOGI(TAG, "GET %s", url);

    body_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        ESP_LOGE(TAG, "ctx calloc failed");
        return false;
    }

    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = releases_http_event,
        .user_data         = ctx,
        .timeout_ms        = 10000,
        // GitHub requires a User-Agent on every API request, else 403.
        .user_agent        = "orb-esp32/ota",
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http_client_init failed");
        free(ctx);
        return false;
    }
    // GitHub recommends an explicit Accept header, lets them serve the
    // stable v3 schema even if defaults shift.
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    bool ok = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform: %s", esp_err_to_name(err));
    } else if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
    } else if (ctx->overflow || ctx->len == 0) {
        ESP_LOGE(TAG, "bad body (len=%d, overflow=%d)", ctx->len, ctx->overflow);
    } else {
        ok = parse_latest_release(ctx->buf, out_tag, tag_sz, out_url, url_sz);
    }
    free(ctx);
    return ok;
}

void orb_ota_check_and_update_on_boot(void)
{
    orb_ui_set_state(ORB_CHECK_UPD);

    const esp_app_desc_t *app  = esp_app_get_description();
    const char *running_ver    = app ? app->version : "?";
    ESP_LOGI(TAG, "running version: %s", running_ver);

    // Dev-build guard. A clean release is built at an exact tag, so its
    // git-describe version is just the tag (e.g. "v0.0.1"). A local/dev build
    // carries extra suffixes — "-<N>-g<hash>" (commits ahead of the tag) and/or
    // "-dirty" (uncommitted changes), or "NOTAG" when no tag exists. Because the
    // update test below is a plain strcmp (any difference triggers a download),
    // auto-updating a dev build would pull the device back DOWN to whatever the
    // latest published release is — clobbering the local work you just flashed.
    // So skip the OTA check entirely for non-release builds; only clean tagged
    // images participate in auto-update.
#if !CONFIG_ORB_OTA_UPDATE_DEV_BUILDS
    if (strstr(running_ver, "-dirty") || strstr(running_ver, "-g") ||
        strstr(running_ver, "NOTAG")) {
        ESP_LOGW(TAG, "dev build (%s) — skipping OTA auto-update", running_ver);
        return;
    }
#else
    ESP_LOGW(TAG, "dev-guard DISABLED (CONFIG_ORB_OTA_UPDATE_DEV_BUILDS) — "
                  "dev build %s will take OTA updates", running_ver);
#endif

    char latest_tag[64]    = {0};
    char asset_url[256]    = {0};
    if (!fetch_latest_release(latest_tag, sizeof(latest_tag),
                              asset_url,  sizeof(asset_url))) {
        ESP_LOGW(TAG, "release check failed; continuing with current image");
        return;
    }
    ESP_LOGI(TAG, "latest release: tag=%s asset=%s", latest_tag, asset_url);

    if (strcmp(latest_tag, running_ver) == 0) {
        ESP_LOGI(TAG, "up to date");
        return;
    }

    ESP_LOGI(TAG, "update available (%s -> %s); downloading",
             running_ver, latest_tag);
    orb_ui_set_state(ORB_UPDATING);

    esp_http_client_config_t http_cfg = {
        .url               = asset_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
        .keep_alive_enable = true,
        .user_agent        = "orb-esp32/ota",
        // GitHub release downloads 302-redirect from github.com to a signed
        // objects.githubusercontent.com URL whose query string is 600-900+
        // chars. The default 512 B TX buffer can't hold that request line and
        // esp_http_client aborts with "Out of buffer" before the GET is sent.
        // Bump both buffers so the redirected request + its response headers
        // fit. (RX bumped too — the signed-URL host returns large headers.)
        .buffer_size       = 4096,
        .buffer_size_tx    = 4096,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA write OK; rebooting into new image");
        vTaskDelay(pdMS_TO_TICKS(500));  // let the log line flush over UDP
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA failed: %s; keeping current image", esp_err_to_name(err));
    // Boot continues from here — convai will start in post_connect_task.
}

void orb_ota_mark_running_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "running image marked valid (rollback cancelled)");
    } else {
        ESP_LOGW(TAG, "mark_app_valid failed: %s", esp_err_to_name(err));
    }
}

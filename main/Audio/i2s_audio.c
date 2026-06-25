#include "i2s_audio.h"

#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_gpio.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "soc/gpio_sig_map.h"

#define I2S_SAMPLE_RATE 16000
#define I2S_BCLK_GPIO   GPIO_NUM_43
#define I2S_WS_GPIO     GPIO_NUM_44
#define I2S_DOUT_GPIO   GPIO_NUM_19   // → MAX98357A DIN
#define I2S_DIN_GPIO    GPIO_NUM_20   // ← INMP441 SD (moved off GPIO 0 — strap pin contention)

static const char *TAG = "i2s";

static i2s_chan_handle_t s_tx = NULL;
static i2s_chan_handle_t s_rx = NULL;
static bool s_tx_enabled = false;
static bool s_rx_enabled = false;
static bool s_initialized = false;

static void dout_mute(void);
static void dout_unmute(void);

void i2s_audio_init(void)
{
    if (s_initialized) return;

    // Two separate I2S peripherals, like every working INMP441 example does:
    //   I2S_NUM_0: TX master  → drives BCLK/WS/DOUT to MAX98357A
    //   I2S_NUM_1: RX slave   → reads BCLK/WS as inputs from the same pins,
    //                           reads DIN from INMP441 SD
    // The pins are shared physically (J9 wiring); the GPIO matrix lets each
    // pin be both an output (from I2S0) and an input (to I2S1) at once.

    // ── TX (speaker) ────────────────────────────────────────────────────
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // Larger DMA: 8 descriptors × 480 frames = 3840 samples = 240 ms of TX
    // buffering at 16 kHz. Default (~6×240) makes chunk boundaries every
    // ~22 ms — every late top-up under load shows up as a click. With
    // 240 ms we have huge slack.
    tx_chan_cfg.dma_desc_num  = 8;
    tx_chan_cfg.dma_frame_num = 480;
    // Auto-clear TX DMA so when no one writes, hardware clocks zeros — the
    // speaker can't replay the boot beep tail.
    tx_chan_cfg.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &s_tx, NULL));

    // 32-bit data width matches RX so both peripherals run at the same
    // BCLK rate (16 kHz × 64 = 1.024 MHz). MAX98357A accepts 16-bit data
    // MSB-aligned in a 32-bit slot — the play path widens with << 16.
    i2s_std_config_t tx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &tx_cfg));

    // ── RX (mic) — separate peripheral, slave to the TX clocks ──────────
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &s_rx));

    i2s_std_config_t rx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,           // input — read I2S0's clock
            .ws   = I2S_WS_GPIO,             // input — read I2S0's WS
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &rx_cfg));

    // Disable pulls on the DIN pin so nothing fights the INMP441 driver.
    // (Keep this even though we're now on GPIO 20, which has no strap
    // circuitry — it's a no-op there but documents intent.)
    gpio_set_pull_mode(I2S_DIN_GPIO, GPIO_FLOATING);

    // Bring TX up immediately and leave it on. The mic depends on TX for
    // its clocks (RX is slave), so steady clocks = stable mic. Speaker
    // silence is achieved by detaching DOUT (dout_mute) plus auto_clear.
    i2s_channel_enable(s_tx);
    s_tx_enabled = true;
    dout_mute();

    s_initialized = true;
    ESP_LOGI(TAG, "split I2S ready: TX=I2S0 master, RX=I2S1 slave — 16 kHz mono — BCLK=%d WS=%d DOUT=%d DIN=%d",
             I2S_BCLK_GPIO, I2S_WS_GPIO, I2S_DOUT_GPIO, I2S_DIN_GPIO);
}

// ─── DOUT routing (amp mute) ─────────────────────────────────────────────
// We can't disable the I2S TX channel during record (in duplex mode RX needs
// TX to be running for BCLK/WS to clock), but we still want the speaker
// silent. Solution: detach DOUT from the I2S signal and drive the pin LOW
// from GPIO. MAX98357A auto-mutes after ~25 ms of steady DIN. On unmute we
// re-route DOUT back to the I2S TX serial-data signal.

static void dout_mute(void)
{
    gpio_set_direction(I2S_DOUT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(I2S_DOUT_GPIO, 0);
    esp_rom_gpio_connect_out_signal(I2S_DOUT_GPIO, SIG_GPIO_OUT_IDX, false, false);
}

static void dout_unmute(void)
{
    // I2S0O_SD_OUT_IDX is the I2S0 TX serial-data output signal on ESP32-S3.
    esp_rom_gpio_connect_out_signal(I2S_DOUT_GPIO, I2S0O_SD_OUT_IDX, false, false);
}

// ─── TX ──────────────────────────────────────────────────────────────────

static void tx_ensure_enabled(void)
{
    if (s_tx && !s_tx_enabled) {
        i2s_channel_enable(s_tx);
        s_tx_enabled = true;
    }
}

// Wraps tx_ensure_enabled and also restores DOUT routing to the I2S signal.
// Call this from the playback path so a previous record() that muted DOUT
// gets undone before audio is written.
static void tx_audible(void)
{
    tx_ensure_enabled();
    dout_unmute();
}

// Output volume as percent (0..100). Mutable at runtime — set via
// i2s_audio_set_volume_pct() after the Supabase config fetch.
static int s_tx_volume_pct = 100;

void i2s_audio_set_volume_pct(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    s_tx_volume_pct = pct;
    ESP_LOGI(TAG, "TX volume set to %d%%", pct);
}

// I2S now uses 32-bit data width; the speaker amp wants 16-bit data MSB-
// aligned in the 32-bit slot. Helper widens an int16_t sample to that form
// and applies the global volume scale.
static inline int32_t widen_s16_to_s32(int16_t s)
{
    int32_t scaled = ((int32_t)s * s_tx_volume_pct) / 100;
    return scaled << 16;
}

// Write `count` int16 samples, widened to int32 in fixed chunks. Used by
// the play/write/beep paths so they don't have to allocate big buffers.
static void tx_write_s16(const int16_t *samples, size_t count)
{
    int32_t chunk[256];
    size_t produced = 0;
    while (produced < count) {
        size_t take = count - produced;
        if (take > sizeof(chunk) / sizeof(chunk[0])) take = sizeof(chunk) / sizeof(chunk[0]);
        for (size_t i = 0; i < take; i++) chunk[i] = widen_s16_to_s32(samples[produced + i]);
        size_t written = 0;
        i2s_channel_write(s_tx, chunk, take * sizeof(int32_t), &written, portMAX_DELAY);
        produced += take;
    }
}

static void tx_drain_and_disable(void)
{
    if (!s_tx || !s_tx_enabled) return;
    // i2s_channel_write only queues into DMA; it returns long before the
    // audio has actually clocked out the speaker. With 240 ms of DMA
    // buffer (see i2s_audio_init), if we mute DOUT immediately the beep
    // is still sitting in DMA and never gets heard. Push enough trailing
    // silence to fully flush whatever audio was queued before us, THEN
    // mute the amp. 8 × 480 frames = 3840 samples — match the DMA size.
    int32_t silence[480] = {0};
    size_t written = 0;
    for (int i = 0; i < 8; i++) {
        i2s_channel_write(s_tx, silence, sizeof(silence), &written, portMAX_DELAY);
    }
    dout_mute();
}

void i2s_audio_write(const int16_t *samples, size_t count)
{
    if (!s_tx || !samples || count == 0) return;
    tx_audible();
    tx_write_s16(samples, count);
}

void i2s_audio_play(const int16_t *samples, size_t count)
{
    if (!s_tx || !samples || count == 0) return;
    tx_audible();
    tx_write_s16(samples, count);
    tx_drain_and_disable();
}

void i2s_audio_beep(int freq_hz, int duration_ms)
{
    if (!s_tx || freq_hz <= 0 || duration_ms <= 0) return;

    tx_audible();

    int total_samples = (I2S_SAMPLE_RATE * duration_ms) / 1000;
    const int amp = 8000;
    // 5 ms linear ramp at the start and end so the speaker doesn't pop on
    // the 0→sin step. Capped at half the total length for very short beeps.
    int ramp = (5 * I2S_SAMPLE_RATE) / 1000;
    if (ramp > total_samples / 2) ramp = total_samples / 2;

    int16_t buf[256];
    float phase  = 0.0f;
    float dphase = 2.0f * (float)M_PI * (float)freq_hz / (float)I2S_SAMPLE_RATE;

    int sent = 0;
    while (sent < total_samples) {
        int chunk = (total_samples - sent) > (int)(sizeof(buf) / sizeof(buf[0]))
                  ? (int)(sizeof(buf) / sizeof(buf[0]))
                  : (total_samples - sent);
        for (int i = 0; i < chunk; i++) {
            int idx = sent + i;
            // Envelope: ramp up over the first `ramp` samples, ramp down
            // over the last `ramp` samples, full volume in between.
            int env_num = amp;
            if (idx < ramp) {
                env_num = (amp * idx) / ramp;
            } else if (idx >= total_samples - ramp) {
                env_num = (amp * (total_samples - 1 - idx)) / ramp;
            }
            buf[i] = (int16_t)(env_num * sinf(phase));
            phase += dphase;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        tx_write_s16(buf, chunk);
        sent += chunk;
    }

    tx_drain_and_disable();
}

void i2s_audio_tone_sequence(const i2s_audio_tone_t *tones, size_t count)
{
    if (!s_tx || !tones || count == 0) return;

    tx_audible();

    const int amp = 8000;
    int ramp = (3 * I2S_SAMPLE_RATE) / 1000;   // 3 ms transition between tones
    int16_t buf[256];

    for (size_t t = 0; t < count; t++) {
        int freq_hz = tones[t].freq_hz;
        int duration_ms = tones[t].duration_ms;
        if (freq_hz <= 0 || duration_ms <= 0) continue;

        int total_samples = (I2S_SAMPLE_RATE * duration_ms) / 1000;
        int this_ramp = ramp;
        if (this_ramp > total_samples / 2) this_ramp = total_samples / 2;

        // Phase resets per tone — the brief 3ms ramp at boundaries hides
        // the discontinuity. Phase-continuous tone sequencing would need
        // tracking dphase across tones; the ramp is simpler and inaudible.
        float phase  = 0.0f;
        float dphase = 2.0f * (float)M_PI * (float)freq_hz / (float)I2S_SAMPLE_RATE;

        int sent = 0;
        while (sent < total_samples) {
            int chunk = (total_samples - sent) > (int)(sizeof(buf) / sizeof(buf[0]))
                      ? (int)(sizeof(buf) / sizeof(buf[0]))
                      : (total_samples - sent);
            for (int i = 0; i < chunk; i++) {
                int idx = sent + i;
                int env_num = amp;
                if (idx < this_ramp) {
                    env_num = (amp * idx) / this_ramp;
                } else if (idx >= total_samples - this_ramp) {
                    env_num = (amp * (total_samples - 1 - idx)) / this_ramp;
                }
                buf[i] = (int16_t)(env_num * sinf(phase));
                phase += dphase;
                if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            }
            tx_write_s16(buf, chunk);
            sent += chunk;
        }
    }

    tx_drain_and_disable();
}

// ─── RX ──────────────────────────────────────────────────────────────────

bool i2s_audio_record(int16_t *out, size_t count)
{
    if (!s_rx || !out || count == 0) return false;

    // Keep TX channel running (it owns BCLK/WS in duplex mode) but detach
    // the DOUT pin from I2S so the speaker amp sees a steady 0 V on its DIN
    // line and auto-mutes. Reattach in tx_drain_and_disable / next play.
    tx_ensure_enabled();
    dout_mute();

    if (!s_rx_enabled) {
        esp_err_t err = i2s_channel_enable(s_rx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_enable(RX) failed: %s", esp_err_to_name(err));
            return false;
        }
        s_rx_enabled = true;
    }

    // Deadline: 2× expected duration, minimum 500 ms. If the mic is dead or
    // miswired, DMA underruns and the read would otherwise block forever.
    int64_t expected_us = ((int64_t)count * 1000000) / I2S_SAMPLE_RATE;
    int64_t deadline_us = esp_timer_get_time() + (expected_us * 2) + 500000;

    // I2S now reads 32-bit samples (full INMP441 slot). Pull samples in
    // chunks via a stack-side int32_t buffer, shift down to 16-bit, store.
    int32_t chunk[256];
    size_t produced = 0;
    bool ok = true;
    bool logged_raw = false;

    while (produced < count) {
        if (esp_timer_get_time() >= deadline_us) {
            ESP_LOGW(TAG, "record timed out: got %u/%u samples",
                     (unsigned)produced, (unsigned)count);
            ok = false;
            break;
        }
        // We need `want_samples` REAL mic samples, but ESP-IDF i2s_std on
        // ESP32-S3 captures BOTH stereo slots into the DMA even in MONO
        // mode (the right slot just reads back as zero since INMP441 only
        // drives the left). Read 2× and keep the even-indexed entries.
        size_t want_real = count - produced;
        if (want_real > sizeof(chunk) / sizeof(chunk[0]) / 2) {
            want_real = sizeof(chunk) / sizeof(chunk[0]) / 2;
        }
        size_t want_raw = want_real * 2;
        size_t got_bytes = 0;
        i2s_channel_read(s_rx, chunk, want_raw * sizeof(int32_t),
                         &got_bytes, pdMS_TO_TICKS(250));
        size_t got_raw = got_bytes / sizeof(int32_t);
        if (!logged_raw && got_raw >= 8) {
            ESP_LOGI(TAG, "raw int32 [0..7] = %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                     (unsigned long)chunk[0], (unsigned long)chunk[1],
                     (unsigned long)chunk[2], (unsigned long)chunk[3],
                     (unsigned long)chunk[4], (unsigned long)chunk[5],
                     (unsigned long)chunk[6], (unsigned long)chunk[7]);
            logged_raw = true;
        }
        // Take only even-indexed (LEFT slot) samples. INMP441 outputs 24-bit
        // signed data MSB-aligned in the 32-bit slot. Shift right by 15 =
        // 2× software gain over the lossless >>16. 4× was too aggressive
        // and clipped audible mains hum / loud audio.
        size_t got_real = got_raw / 2;
        for (size_t i = 0; i < got_real; i++) {
            int32_t s = chunk[i * 2] >> 15;
            if (s > 32767) s = 32767;
            else if (s < -32768) s = -32768;
            out[produced + i] = (int16_t)s;
        }
        produced += got_real;
    }

    // INMP441 outputs invalid data for the first ~50 ms after BCLK starts
    // (datasheet). Zero those samples and fade-in the next 10 ms so the
    // zero→signal transition doesn't pop.
    size_t settle_samples = (50 * I2S_SAMPLE_RATE) / 1000;
    size_t fade_samples   = (10 * I2S_SAMPLE_RATE) / 1000;
    if (settle_samples > produced) settle_samples = produced;
    for (size_t i = 0; i < settle_samples; i++) out[i] = 0;
    if (settle_samples + fade_samples > produced) {
        fade_samples = produced - settle_samples;
    }
    for (size_t i = 0; i < fade_samples; i++) {
        int32_t v = out[settle_samples + i];
        out[settle_samples + i] = (int16_t)((v * (int32_t)i) / (int32_t)fade_samples);
    }

    // Remove DC offset: compute mean over the post-settle window and
    // subtract. Captures any bias from the mic (typical INMP441 has small
    // nonzero DC). Skip the settle/fade region.
    size_t dc_start = settle_samples + fade_samples;
    if (dc_start < produced) {
        int64_t sum = 0;
        for (size_t i = dc_start; i < produced; i++) sum += out[i];
        int32_t mean = (int32_t)(sum / (int64_t)(produced - dc_start));
        for (size_t i = dc_start; i < produced; i++) {
            int32_t v = (int32_t)out[i] - mean;
            if (v > 32767) v = 32767;
            else if (v < -32768) v = -32768;
            out[i] = (int16_t)v;
        }
    }

    // Despike: a "click" is a single sample that jumps far from BOTH
    // neighbors. Real audio (even at high frequencies) doesn't do that
    // because adjacent samples are correlated. We replace such samples
    // with the average of their neighbors. Threshold 6000 (~9 dBFS step)
    // is well above any natural delta in 4 kHz-band speech but well below
    // the 24,000+ deltas seen in the diagnostic.
    if (produced >= 3) {
        const int spike_thresh = 6000;
        int despiked = 0;
        for (size_t i = 1; i + 1 < produced; i++) {
            int prev = out[i - 1];
            int cur  = out[i];
            int next = out[i + 1];
            int dprev = cur - prev; if (dprev < 0) dprev = -dprev;
            int dnext = next - cur; if (dnext < 0) dnext = -dnext;
            int dneighbors = next - prev; if (dneighbors < 0) dneighbors = -dneighbors;
            // Both jumps large AND neighbors are close to each other →
            // the middle sample is the outlier.
            if (dprev > spike_thresh && dnext > spike_thresh && dneighbors < spike_thresh) {
                out[i] = (int16_t)((prev + next) / 2);
                despiked++;
            }
        }
        ESP_LOGI(TAG, "despiker removed %d samples", despiked);
    }

    i2s_channel_disable(s_rx);
    s_rx_enabled = false;
    return ok;
}

// ─── Streaming RX (push-to-talk) ─────────────────────────────────────────
// Unlike i2s_audio_record (one-shot fixed length with post-processing), this
// keeps the RX channel open across many small chunks. No settle/fade/DC
// removal — INMP441's first ~50 ms after start are zeroed by the caller if
// needed. Designed for streaming the mic to the network with minimal latency.

void i2s_audio_record_start(void)
{
    if (!s_rx) return;
    tx_ensure_enabled();   // RX shares BCLK/WS with TX in duplex
    dout_mute();           // amp silent while mic is open
    if (!s_rx_enabled) {
        esp_err_t err = i2s_channel_enable(s_rx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "record_start: enable RX failed: %s", esp_err_to_name(err));
            return;
        }
        s_rx_enabled = true;
    }
}

size_t i2s_audio_record_chunk(int16_t *out, size_t count)
{
    if (!s_rx || !s_rx_enabled || !out || count == 0) return 0;

    int32_t chunk[256];
    size_t produced = 0;
    while (produced < count) {
        size_t want_real = count - produced;
        if (want_real > sizeof(chunk) / sizeof(chunk[0]) / 2) {
            want_real = sizeof(chunk) / sizeof(chunk[0]) / 2;
        }
        size_t want_raw = want_real * 2;
        size_t got_bytes = 0;
        i2s_channel_read(s_rx, chunk, want_raw * sizeof(int32_t),
                         &got_bytes, pdMS_TO_TICKS(250));
        size_t got_raw = got_bytes / sizeof(int32_t);
        if (got_raw == 0) break;       // timeout — caller decides what to do
        size_t got_real = got_raw / 2;
        for (size_t i = 0; i < got_real; i++) {
            int32_t s = chunk[i * 2] >> 15;        // INMP441: top 24 bits, +1 bit software gain
            if (s > 32767) s = 32767;
            else if (s < -32768) s = -32768;
            out[produced + i] = (int16_t)s;
        }
        produced += got_real;
    }
    return produced;
}

void i2s_audio_record_stop(void)
{
    if (s_rx_enabled) {
        i2s_channel_disable(s_rx);
        s_rx_enabled = false;
    }
    dout_unmute();         // amp can sound again (agent response, etc.)
}

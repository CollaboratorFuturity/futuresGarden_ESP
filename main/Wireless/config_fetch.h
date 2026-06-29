#pragma once

#include <stdbool.h>
#include <stddef.h>

#define ORB_AGENT_ID_MAX   64
#define ORB_AGENT_NAME_MAX 32
#define ORB_DEVICE_ID_MAX  16

typedef struct {
    char agent_id[ORB_AGENT_ID_MAX];
    char agent_name[ORB_AGENT_NAME_MAX];
    int  volume;   // 1..10, or 0 = use ElevenLabs default
} OrbConfig;

// Build "orb-XXXXXX" from STA MAC last-3 bytes (uppercase hex), or use the
// DEVICE_ID override from secrets.h if defined. Writes into `out` (>= 16 chars).
void orb_device_id(char *out, size_t out_len);

// Blocking SNTP sync. Returns true once system time is past 2024 or timeout
// elapses. Safe to call multiple times.
bool orb_sntp_sync(int timeout_ms);

// GET SUPABASE_CONFIG_URL?device_id=<id> over HTTPS, parse JSON into `out`.
// Returns true on success. Streams body via event handler; bounded to 1 KB.
bool orb_config_fetch(OrbConfig *out);

// Download the NFC tag table (NFC_TAGS_URL, raw file on GitHub) over HTTPS into
// a freshly-allocated, NUL-terminated buffer. Caller owns it — free() when done.
// Uses the IDF cert bundle (not the pinned Supabase cert). Returns NULL on any
// failure (alloc / network / TLS / non-200 / empty / oversized).
char *orb_nfc_tags_fetch(void);

#include "log_sink.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/sockets.h"

// Laptop IP that runs `nc -ul 6666`. Swap the active line when you change
// networks (one subnet per Wi-Fi). Empty string "" = limited broadcast
// (255.255.255.255) if your AP allows it.
//#define LOG_SINK_TARGET_IP  "192.168.0.13"
#define LOG_SINK_TARGET_IP  "192.168.1.27"

#define LOG_LINE_MAX 512

static int s_sock = -1;
static struct sockaddr_in s_dest;

static int udp_vprintf(const char *fmt, va_list args)
{
    // Format first, then send. We deliberately do NOT fall back to UART
    // vprintf — once audio claims GPIO 43/44 the UART write blocks forever
    // waiting on a dead pin, which would also block every log call.
    char buf[LOG_LINE_MAX];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len <= 0) return len;
    if (len > (int)sizeof(buf)) len = sizeof(buf);

    if (s_sock >= 0) {
        // UDP sendto on lwip is thread-safe per call — no mutex needed.
        sendto(s_sock, buf, len, 0, (struct sockaddr *)&s_dest, sizeof(s_dest));
    }
    return len;
}

void log_sink_start(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    // Bind to a fixed source port. macOS BSD nc -u -l locks to the first
    // sender's (ip:port) pair, so without this every packet leaves from a
    // different ephemeral port and nc drops everything after the first one.
    struct sockaddr_in src = {0};
    src.sin_family      = AF_INET;
    src.sin_addr.s_addr = htonl(INADDR_ANY);
    src.sin_port        = htons(54321);
    bind(sock, (struct sockaddr *)&src, sizeof(src));

    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sin_family = AF_INET;
    s_dest.sin_port   = htons(port);

    const char *target = LOG_SINK_TARGET_IP;
    if (target[0] != '\0') {
        inet_aton(target, &s_dest.sin_addr);
    } else {
        s_dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        target = "255.255.255.255";
    }

    s_sock = sock;
    esp_log_set_vprintf(udp_vprintf);

    ESP_LOGI("log_sink", "UDP log sink up on port %u (target=%s)", port, target);
}

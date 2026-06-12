#include "ntp.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/udp.h"

#define NTP_PORT          123
#define NTP_MSG_LEN       48
// Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01).
#define NTP_UNIX_DELTA    2208988800UL

typedef struct {
    ip_addr_t       server_addr;
    struct udp_pcb *pcb;
    volatile bool   done;
    volatile bool   ok;
    time_t          epoch;
} ntp_ctx_t;

// Called by lwIP when an NTP reply arrives.
static void ntp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                     const ip_addr_t *addr, u16_t port) {
    ntp_ctx_t *ctx = (ntp_ctx_t *)arg;
    uint8_t mode    = pbuf_get_at(p, 0) & 0x7;
    uint8_t stratum = pbuf_get_at(p, 1);

    if (p->tot_len == NTP_MSG_LEN && mode == 0x4 && stratum != 0 &&
        ip_addr_cmp(addr, &ctx->server_addr) && port == NTP_PORT) {
        // Transmit timestamp: seconds since 1900 in bytes 40..43 (big-endian).
        uint8_t b[4];
        pbuf_copy_partial(p, b, 4, 40);
        uint32_t secs1900 = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                            ((uint32_t)b[2] << 8) | b[3];
        ctx->epoch = (time_t)(secs1900 - NTP_UNIX_DELTA);
        ctx->ok = true;
    }
    pbuf_free(p);
    ctx->done = true;
}

static void ntp_send_request(ntp_ctx_t *ctx) {
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
    uint8_t *req = (uint8_t *)p->payload;
    memset(req, 0, NTP_MSG_LEN);
    req[0] = 0x1b;  // LI=0, VN=3, Mode=3 (client)
    udp_sendto(ctx->pcb, p, &ctx->server_addr, NTP_PORT);
    pbuf_free(p);
    cyw43_arch_lwip_end();
}

// Called by lwIP when DNS resolution completes.
static void ntp_dns_found(const char *name, const ip_addr_t *addr, void *arg) {
    ntp_ctx_t *ctx = (ntp_ctx_t *)arg;
    if (addr) {
        ctx->server_addr = *addr;
        printf("NTP: DNS resolved %s -> %s\n", name, ipaddr_ntoa(addr));
        ntp_send_request(ctx);
    } else {
        printf("NTP: DNS resolution failed for %s\n", name);
        ctx->ok = false;
        ctx->done = true;
    }
}

bool ntp_sync(time_t *out_epoch, uint32_t timeout_ms) {
    ntp_ctx_t ctx = {0};

    cyw43_arch_lwip_begin();
    ctx.pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (!ctx.pcb) {
        cyw43_arch_lwip_end();
        return false;
    }
    udp_recv(ctx.pcb, ntp_recv, &ctx);

    err_t err = dns_gethostbyname(NTP_SERVER, &ctx.server_addr, ntp_dns_found, &ctx);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) {
        // Address was cached -- send immediately.
        ntp_send_request(&ctx);
    } else if (err != ERR_INPROGRESS) {
        cyw43_arch_lwip_begin();
        udp_remove(ctx.pcb);
        cyw43_arch_lwip_end();
        return false;
    }

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!ctx.done && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        cyw43_arch_poll();
        sleep_ms(5);
    }

    if (!ctx.done)
        printf("NTP: timed out waiting for reply (no packet in %lu ms)\n",
               (unsigned long)timeout_ms);
    else if (!ctx.ok)
        printf("NTP: reply received but rejected (bad mode/stratum)\n");

    cyw43_arch_lwip_begin();
    udp_remove(ctx.pcb);
    cyw43_arch_lwip_end();

    if (ctx.ok) *out_epoch = ctx.epoch;
    return ctx.ok;
}

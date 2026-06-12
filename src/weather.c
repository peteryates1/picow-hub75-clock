#include "weather.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"

// wttr.in returns a plain-text temperature (e.g. "+18°C") for the `?format=%t`
// query *only* when the User-Agent looks like curl; browsers get HTML. The
// reply is tiny, so a small buffer is plenty. The body may be chunked, so we
// don't parse HTTP framing — we just scan the body for the first signed integer
// (the %t output always carries a +/- sign), which skips any chunk-size digits.
#define WEATHER_HOST "wttr.in"
#define HTTP_PORT    80
#define RESP_MAX     512

typedef struct {
    ip_addr_t       addr;
    struct tcp_pcb *pcb;
    char            resp[RESP_MAX];
    int             resp_len;
    volatile bool   done;
} weather_ctx_t;

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    weather_ctx_t *c = (weather_ctx_t *)arg;
    if (!p) {            // remote closed -> response complete
        c->done = true;
        return ERR_OK;
    }
    if (err == ERR_OK) {
        for (struct pbuf *q = p; q; q = q->next) {
            int n = q->len;
            if (c->resp_len + n > RESP_MAX - 1) n = RESP_MAX - 1 - c->resp_len;
            if (n > 0) { memcpy(c->resp + c->resp_len, q->payload, n); c->resp_len += n; }
        }
        tcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    weather_ctx_t *c = (weather_ctx_t *)arg;
    if (err != ERR_OK) { c->done = true; return err; }
    char req[160];
    int n = snprintf(req, sizeof req,
                     "GET /%s?format=%%t HTTP/1.1\r\n"
                     "Host: " WEATHER_HOST "\r\n"
                     "User-Agent: curl/8.0\r\n"
                     "Connection: close\r\n\r\n",
                     WEATHER_LOCATION);
    tcp_write(pcb, req, n, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    (void)err;
    weather_ctx_t *c = (weather_ctx_t *)arg;
    c->pcb = NULL;       // lwIP has freed the pcb
    c->done = true;
}

// Open the connection (must be called with the lwIP lock held, or from a
// callback that already holds it).
static void start_connect(weather_ctx_t *c) {
    c->pcb = tcp_new_ip_type(IP_GET_TYPE(&c->addr));
    if (!c->pcb) { c->done = true; return; }
    tcp_arg(c->pcb, c);
    tcp_recv(c->pcb, on_recv);
    tcp_err(c->pcb, on_err);
    if (tcp_connect(c->pcb, &c->addr, HTTP_PORT, on_connected) != ERR_OK)
        c->done = true;
}

static void dns_cb(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name;
    weather_ctx_t *c = (weather_ctx_t *)arg;
    if (!addr) { c->done = true; return; }
    c->addr = *addr;
    start_connect(c);    // in lwIP callback context
}

// Scan the response body for the first signed integer and return it.
static bool parse_temp(const char *resp, int *out_c) {
    const char *body = strstr(resp, "\r\n\r\n");
    const char *p = body ? body + 4 : resp;
    for (; *p; p++) {
        if ((*p == '+' || *p == '-') && p[1] >= '0' && p[1] <= '9') {
            *out_c = atoi(p);
            return true;
        }
    }
    return false;
}

bool weather_fetch_temp(int *out_celsius, uint32_t timeout_ms) {
    static weather_ctx_t c;   // keep the 512-byte buffer off the stack
    memset(&c, 0, sizeof c);

    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(WEATHER_HOST, &c.addr, dns_cb, &c);
    if (err == ERR_OK) start_connect(&c);   // address cached; still hold lock
    cyw43_arch_lwip_end();

    if (err != ERR_OK && err != ERR_INPROGRESS) return false;

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!c.done && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        cyw43_arch_poll();
        sleep_ms(10);
    }

    cyw43_arch_lwip_begin();
    if (c.pcb) {
        tcp_arg(c.pcb, NULL);
        tcp_recv(c.pcb, NULL);
        tcp_err(c.pcb, NULL);
        tcp_close(c.pcb);
        c.pcb = NULL;
    }
    cyw43_arch_lwip_end();

    c.resp[c.resp_len] = '\0';
    return parse_temp(c.resp, out_celsius);
}

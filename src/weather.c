#include "weather.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"

// We fetch a small JSON forecast from Open-Meteo over plain HTTP and pull out
// three numbers: current temperature, and today's max/min. The response is
// ~500 bytes, so a 1KB buffer is plenty. We scan for tokens rather than parsing
// JSON properly; the keys are anchored under "current"/"daily" to avoid the
// matching "*_units" string fields.
#define WEATHER_HOST "api.open-meteo.com"
#define HTTP_PORT    80
#define RESP_MAX     1024

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
    char req[256];
    int n = snprintf(req, sizeof req,
                     "GET /v1/forecast?latitude=" WEATHER_LAT
                     "&longitude=" WEATHER_LON
                     "&current=temperature_2m"
                     "&daily=temperature_2m_max,temperature_2m_min"
                     "&timezone=auto&forecast_days=2 HTTP/1.1\r\n"
                     "Host: " WEATHER_HOST "\r\n"
                     "User-Agent: curl/8.0\r\n"
                     "Connection: close\r\n\r\n");
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

static int round_i(double v) { return (int)(v < 0 ? v - 0.5 : v + 0.5); }
static bool is_num(char c) { return c == '-' || (c >= '0' && c <= '9'); }

// Parse the (rounded) number right after `token` (skipping spaces and a leading
// '[' for arrays). Must start with a digit/'-' so it won't read a "_units":"°C"
// string. Returns false if not found.
static bool num_after(const char *s, const char *token, int *out) {
    const char *p = strstr(s, token);
    if (!p) return false;
    p += strlen(token);
    while (*p == ' ' || *p == '[') p++;
    if (!is_num(*p)) return false;
    *out = round_i(atof(p));
    return true;
}

// Like num_after but for a 2-element array "[a,b]": fills *a and *b (*b = *a if
// only one element is present).
static bool nums2_after(const char *s, const char *token, int *a, int *b) {
    const char *p = strstr(s, token);
    if (!p) return false;
    p += strlen(token);
    while (*p == ' ' || *p == '[') p++;
    if (!is_num(*p)) return false;
    *a = round_i(atof(p));
    *b = *a;
    const char *comma = strchr(p, ',');
    const char *end = strchr(p, ']');
    if (comma && (!end || comma < end)) {
        const char *q = comma + 1;
        while (*q == ' ') q++;
        if (is_num(*q)) *b = round_i(atof(q));
    }
    return true;
}

bool weather_fetch(weather_t *out, uint32_t timeout_ms) {
    static weather_ctx_t c;   // keep the response buffer off the stack
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
    // Anchor on the "current"/"daily" data objects so we read the numeric
    // values, not the matching "current_units"/"daily_units" strings. The daily
    // arrays hold [today, tomorrow].
    const char *daily = strstr(c.resp, "\"daily\":");
    if (daily) {
        nums2_after(daily, "\"temperature_2m_min\":", &out->today_min, &out->tomorrow_min);
        nums2_after(daily, "\"temperature_2m_max\":", &out->today_max, &out->tomorrow_max);
    }
    const char *cur = strstr(c.resp, "\"current\":");
    return cur && num_after(cur, "\"temperature_2m\":", &out->current);
}

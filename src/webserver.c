#include "webserver.h"
#include "control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"

// Control page. Sliders/buttons call /set?... and refresh from the JSON state.
static const char PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Pico Clock</title><style>"
    "body{font-family:sans-serif;max-width:420px;margin:18px auto;padding:0 14px}"
    "h2{font-size:1.2em}input[type=range]{width:100%}"
    ".r{margin:14px 0}button{padding:8px 16px;margin:0 6px 0 0;font-size:1em}"
    "label{display:block;margin-bottom:4px;color:#444}"
    "input[type=number]{width:3.2em;font-size:1em}#st{color:#888}"
    "</style></head><body><h2>Pico Clock</h2>"
    "<div class=r><label>Brightness <b id=bv></b></label>"
    "<input type=range id=bri min=0 max=255></div>"
    "<div class=r><button onclick=\"g('/set?auto=1')\">Auto</button>"
    "<button onclick=\"g('/set?power=ON')\">On</button>"
    "<button onclick=\"g('/set?power=OFF')\">Off</button></div><hr>"
    "<div class=r><label>Day level <b id=dv></b></label>"
    "<input type=range id=day min=0 max=255></div>"
    "<div class=r><label>Night level <b id=nv></b></label>"
    "<input type=range id=night min=0 max=255></div>"
    "<div class=r><label>Bright hours</label>from "
    "<input type=number id=ds min=0 max=23> to "
    "<input type=number id=de min=0 max=23> "
    "<button onclick=sched()>Set</button></div>"
    "<p id=st></p><script>"
    "function g(u){fetch(u).then(r=>r.json()).then(set)}"
    "function set(s){bri.value=s.override<0?s.day:s.override;bv.textContent=bri.value;"
    "day.value=s.day;dv.textContent=s.day;night.value=s.night;nv.textContent=s.night;"
    "ds.value=s.start;de.value=s.end;"
    "st.textContent='Outside '+s.temp.replace('~','\\u00b0')+' (min/max '+s.minmax+')'}"
    "bri.oninput=()=>bv.textContent=bri.value;bri.onchange=()=>g('/set?bri='+bri.value);"
    "day.oninput=()=>dv.textContent=day.value;day.onchange=()=>g('/set?day='+day.value);"
    "night.oninput=()=>nv.textContent=night.value;night.onchange=()=>g('/set?night='+night.value);"
    "function sched(){g('/set?start='+ds.value+'&end='+de.value)}"
    "g('/state');</script></body></html>";

static int state_json(char *buf, size_t n) {
    return snprintf(buf, n,
        "{\"power\":%d,\"override\":%d,\"day\":%d,\"night\":%d,"
        "\"start\":%d,\"end\":%d,\"temp\":\"%s\",\"minmax\":\"%s\"}",
        control_power(), control_override(), control_day(), control_night(),
        control_day_start(), control_day_end(), control_temp(), control_minmax());
}

// Parse an integer value for "key=" in the request (atoi stops at non-digits).
static bool qint(const char *req, const char *key, int *out) {
    const char *p = strstr(req, key);
    if (!p) return false;
    *out = atoi(p + strlen(key));
    return true;
}

static void apply_query(const char *req) {
    int v;
    if (strstr(req, "auto=1"))      control_set_brightness(-1);
    if (qint(req, "bri=", &v))      control_set_brightness(v);
    if (qint(req, "day=", &v))      control_set_day(v);
    if (qint(req, "night=", &v))    control_set_night(v);
    if (qint(req, "start=", &v))    control_set_day_start(v);
    if (qint(req, "end=", &v))      control_set_day_end(v);
    const char *pw = strstr(req, "power=");
    if (pw) {
        pw += 6;
        control_set_power(strncasecmp(pw, "ON", 2) == 0 || pw[0] == '1');
    }
}

// Send a small response and close the connection (HTTP/1.0 style).
static void respond(struct tcp_pcb *pcb, const char *ctype,
                    const char *body, int blen) {
    char hdr[96];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
        "Connection: close\r\n\r\n", ctype, blen);
    tcp_write(pcb, hdr, hn, TCP_WRITE_FLAG_COPY);
    tcp_write(pcb, body, blen, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    tcp_close(pcb);
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) { tcp_close(pcb); return ERR_OK; }  // client closed

    char req[256];
    u16_t n = p->tot_len < sizeof req - 1 ? p->tot_len : sizeof req - 1;
    pbuf_copy_partial(p, req, n, 0);
    req[n] = '\0';
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (strncmp(req, "GET ", 4) != 0) { tcp_close(pcb); return ERR_OK; }

    if (strncmp(req + 4, "/set", 4) == 0 || strncmp(req + 4, "/state", 6) == 0) {
        if (strncmp(req + 4, "/set", 4) == 0) apply_query(req);
        char json[160];
        int jn = state_json(json, sizeof json);
        respond(pcb, "application/json", json, jn);
    } else {
        respond(pcb, "text/html", PAGE, (int)(sizeof PAGE - 1));
    }
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !newpcb) return ERR_VAL;
    tcp_recv(newpcb, on_recv);
    return ERR_OK;
}

void webserver_init(void) {
    cyw43_arch_lwip_begin();
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb && tcp_bind(pcb, IP_ANY_TYPE, 80) == ERR_OK) {
        pcb = tcp_listen_with_backlog(pcb, 2);
        tcp_accept(pcb, on_accept);
        printf("Web server listening on :80\n");
    } else {
        printf("Web server: bind failed\n");
    }
    cyw43_arch_lwip_end();
}

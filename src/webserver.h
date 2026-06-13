#ifndef WEBSERVER_H
#define WEBSERVER_H

// Start a tiny HTTP server on port 80 serving a control page. Call once after
// the network is up. Endpoints:
//   GET /            -> the HTML control page
//   GET /state       -> current settings as JSON
//   GET /set?...     -> apply settings (bri, auto, power, day, night, start, end)
void webserver_init(void);

#endif // WEBSERVER_H

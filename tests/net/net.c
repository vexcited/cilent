#include "net.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

struct net_client {
  char *base_url;
  ciphon_client http;
};

static thread_local char last_error[256];

const char *net_last_error(void) { return last_error; }

[[gnu::format(printf, 2, 3)]] static net_error fail(net_error code, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(last_error, sizeof last_error, fmt, ap);
  va_end(ap);
  return code;
}

void net_reply_dispose(net_reply *reply) {
  free(reply->body);
  free(reply->cookie);
  free(reply->location);
  free(reply->url);
  *reply = (net_reply){};
}

net_client *net_client_new(const char *base_url, ciphon_client http) {
  net_client *client = calloc(1, sizeof *client);
  client->base_url = strdup(base_url);
  client->http = http;
  return client;
}

void net_client_free(net_client *client) {
  if (!client) return;
  ciphon_client_release(&client->http);
  free(client->base_url);
  free(client);
}

typedef struct options {
  const char *host;
  const char *address;
  uint8_t protocols;
} options;

static net_error request(net_client *client, ciphon_method method, const char *path, const char *body, bool follow,
                         uint64_t max, options opts, net_reply *reply) {
  char url[512];
  // the base url is `http://127.0.0.1:<port>`, a pinned host replaces its address.
  const char *at = opts.host ? strrchr(client->base_url, ':') : nullptr;
  if (at) snprintf(url, sizeof url, "http://%s%s%s", opts.host, at, path);
  else snprintf(url, sizeof url, "%s%s", client->base_url, path);
  ciphon_pin pin = {.host = opts.host, .address = opts.address};
  ciphon_header headers[] = {{.name = "Accept", .value = "text/plain"}, {.name = "Content-Type", .value = "text/plain"}};
  ciphon_request req = {
    .method = method,
    .url = url,
    .headers = headers,
    .headers_count = body ? 2 : 1,
    .body = (const uint8_t *)body,
    .body_len = body ? strlen(body) : 0,
    .follow_redirects = follow,
    .timeout_ms = 5000,
    .max_response_len = max,
    .protocols = opts.protocols,
    .pins = &pin,
    .pins_count = opts.host ? 1 : 0,
  };
  ciphon_response res = {};
  char *message = nullptr;
  ciphon_error code = ciphon_send(&client->http, &req, &res, &message);
  if (code != CIPHON_OK) {
    net_error error = fail(code == CIPHON_ERROR_TOO_LARGE ? NET_ERROR_TOO_LARGE : NET_ERROR_TRANSPORT, "%s: %s", url,
                           message ? message : "no details");
    free(message);
    return error;
  }
  if (res.status >= 400) {
    uint16_t status = res.status;
    ciphon_response_dispose(&res);
    return fail(NET_ERROR_STATUS, "%s answered %u", url, status);
  }

  net_reply out = {.status = res.status, .body = calloc(res.body_len + 1, 1)};
  if (res.body_len) memcpy(out.body, res.body, res.body_len);
  for (size_t i = 0; i < res.headers_count; i++) {
    if (strcasecmp(res.headers[i].name, "set-cookie") != 0) continue;
    if (!out.cookie) out.cookie = strdup(res.headers[i].value);
    out.cookies++;
  }
  const char *location = ciphon_response_header(&res, "location");
  if (location) out.location = strdup(location);
  if (res.url) out.url = strdup(res.url);
  ciphon_response_dispose(&res);
  *reply = out;
  return NET_OK;
}

net_error net_client_get(net_client *client, const char *path, bool follow, net_reply *reply) {
  return request(client, CIPHON_METHOD_GET, path, nullptr, follow, 0, (options){}, reply);
}

net_error net_client_get_capped(net_client *client, const char *path, uint64_t max, net_reply *reply) {
  return request(client, CIPHON_METHOD_GET, path, nullptr, true, max, (options){}, reply);
}

net_error net_client_get_pinned(net_client *client, const char *host, const char *address, const char *path,
                                net_reply *reply) {
  options opts = {.host = host, .address = address};
  return request(client, CIPHON_METHOD_GET, path, nullptr, true, 0, opts, reply);
}

net_error net_client_get_secure(net_client *client, const char *path, net_reply *reply) {
  options opts = {.protocols = CIPHON_PROTOCOL_HTTPS};
  return request(client, CIPHON_METHOD_GET, path, nullptr, true, 0, opts, reply);
}

net_error net_client_post(net_client *client, const char *path, const char *body, net_reply *reply) {
  return request(client, CIPHON_METHOD_POST, path, body, true, 0, (options){}, reply);
}


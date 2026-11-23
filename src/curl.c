#include <ciphon/curl.h>

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct client {
  CURL *curl;
  pthread_mutex_t lock;
  // the pins of the last request, removed from the dns cache of the handle before the next one.
  struct curl_slist *unpin;
} client;

typedef struct buffer {
  const ciphon_request *request;
  const ciphon_sink *sink;
  const ciphon_source *source;
  ciphon_response *response;
  size_t capacity;
  uint64_t received;
  uint64_t sent;
  bool too_large;
  bool stopped;
  bool source_failed;
} buffer;

static pthread_once_t global_once = PTHREAD_ONCE_INIT;

static void global_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

static size_t on_body(char *data, size_t size, size_t count, void *user) {
  buffer *b = user;
  ciphon_response *res = b->response;
  size_t n = size * count;
  uint64_t max = b->request->max_response_len;
  b->received += n;
  if (max && b->received > max) {
    b->too_large = true;
    return 0;
  }
  if (b->sink) {
    b->stopped = !b->sink->write(b->sink->userdata, res, (const uint8_t *)data, n);
    return b->stopped ? 0 : n;
  }
  if (res->body_len + n > b->capacity) {
    size_t capacity = b->capacity ? b->capacity * 2 : 4096;
    while (capacity < res->body_len + n) capacity *= 2;
    uint8_t *grown = realloc(res->body, capacity);
    if (!grown) return 0;
    res->body = grown;
    b->capacity = capacity;
  }
  memcpy(res->body + res->body_len, data, n);
  res->body_len += n;
  return n;
}

static size_t on_read(char *data, size_t size, size_t count, void *user) {
  buffer *b = user;
  size_t got = 0;
  if (!b->source->read(b->source->userdata, b->sent, (uint8_t *)data, size * count, &got)) {
    b->source_failed = true;
    return CURL_READFUNC_ABORT;
  }
  b->sent += got;
  return got;
}

static int on_seek(void *user, curl_off_t offset, int origin) {
  buffer *b = user;
  if (origin != SEEK_SET || offset < 0) return CURL_SEEKFUNC_CANTSEEK;
  b->sent = (uint64_t)offset;
  return CURL_SEEKFUNC_OK;
}

// whether the headers of the final response announce a body over the cap. a compressed length says nothing of
// the decoded size, the body is then checked as it arrives.
static bool announces_too_large(const buffer *b) {
  const ciphon_response *res = b->response;
  uint64_t max = b->request->max_response_len;
  if (!max || b->request->method == CIPHON_METHOD_HEAD || res->status < 200) return false;
  if (b->request->follow_redirects && res->status >= 300 && res->status < 400) return false;
  const char *length = ciphon_response_header(res, "content-length");
  if (!length || ciphon_response_header(res, "content-encoding")) return false;
  return strtoull(length, nullptr, 10) > max;
}

// keeps every header line of the last response, a redirect followed by curl starts the list over.
static size_t on_header(char *line, size_t size, size_t count, void *user) {
  buffer *b = user;
  ciphon_response *res = b->response;
  size_t n = size * count;
  if (n > 5 && strncmp(line, "HTTP/", 5) == 0) {
    for (size_t i = 0; i < res->headers_count; i++) {
      free((void *)res->headers[i].name);
      free((void *)res->headers[i].value);
    }
    res->headers_count = 0;
    const char *code = memchr(line, ' ', n);
    res->status = code ? (uint16_t)strtoul(code + 1, nullptr, 10) : 0;
    return n;
  }
  if (n <= 2 && (line[0] == '\r' || line[0] == '\n')) {
    b->too_large = announces_too_large(b);
    return b->too_large ? 0 : n;
  }
  const char *colon = memchr(line, ':', n);
  if (!colon) return n;
  const char *value = colon + 1;
  const char *end = line + n;
  while (value < end && (*value == ' ' || *value == '\t')) value++;
  while (end > value && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) end--;
  ciphon_header *grown = realloc(res->headers, (res->headers_count + 1) * sizeof *grown);
  if (!grown) return 0;
  res->headers = grown;
  res->headers[res->headers_count++] = (ciphon_header){
    .name = strndup(line, (size_t)(colon - line)),
    .value = strndup(value, (size_t)(end - value)),
  };
  return n;
}

static const char *method_name(ciphon_method method) {
  switch (method) {
    case CIPHON_METHOD_HEAD: return "HEAD";
    case CIPHON_METHOD_POST: return "POST";
    case CIPHON_METHOD_PUT: return "PUT";
    case CIPHON_METHOD_PATCH: return "PATCH";
    case CIPHON_METHOD_DELETE: return "DELETE";
    default: return "GET";
  }
}

static ciphon_error error_code(CURLcode rc) {
  switch (rc) {
    case CURLE_OPERATION_TIMEDOUT: return CIPHON_ERROR_TIMEOUT;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CERTPROBLEM: return CIPHON_ERROR_TLS;
    case CURLE_ABORTED_BY_CALLBACK: return CIPHON_ERROR_CANCELLED;
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL: return CIPHON_ERROR_INVALID_REQUEST;
    default: return CIPHON_ERROR_NETWORK;
  }
}

static const char *protocol_names(uint8_t protocols) {
  switch (protocols & (CIPHON_PROTOCOL_HTTP | CIPHON_PROTOCOL_HTTPS)) {
    case CIPHON_PROTOCOL_HTTP: return "http";
    case CIPHON_PROTOCOL_HTTPS: return "https";
    default: return "http,https";
  }
}

// the port of `url`, its scheme's when it names none.
static long url_port(const char *url) {
  CURLU *u = curl_url();
  char *port = nullptr;
  long n = 0;
  if (u && !curl_url_set(u, CURLUPART_URL, url, 0) && !curl_url_get(u, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT))
    n = strtol(port, nullptr, 10);
  curl_free(port);
  curl_url_cleanup(u);
  return n;
}

static long pin_port(const ciphon_pin *pin, long url_port) { return pin->port ? pin->port : url_port; }

// the pins as curl resolve entries, `host:port:address,address`, after the removal of the previous ones, which
// curl would keep in the dns cache of the handle. false when a pin can't be read.
static bool resolve_entries(client *self, const ciphon_request *request, struct curl_slist **out) {
  struct curl_slist *list = nullptr, *unpin = nullptr;
  for (struct curl_slist *e = self->unpin; e; e = e->next) list = curl_slist_append(list, e->data);
  long port = request->pins_count ? url_port(request->url) : 0;
  bool ok = !request->pins_count || port;
  for (size_t i = 0; i < request->pins_count && ok; i++) {
    const ciphon_pin *pin = &request->pins[i];
    ok = pin->host && *pin->host && !strchr(pin->host, ':') && pin->address && *pin->address;
  }
  for (size_t i = 0; i < request->pins_count && ok; i++) {
    const ciphon_pin *pin = &request->pins[i];
    bool seen = false;
    for (size_t j = 0; j < i && !seen; j++)
      seen = pin_port(&request->pins[j], port) == pin_port(pin, port) && !strcasecmp(request->pins[j].host, pin->host);
    if (seen) continue;
    char *line = nullptr;
    size_t len = 0;
    FILE *f = open_memstream(&line, &len);
    if (!f) {
      ok = false;
      break;
    }
    fprintf(f, "%s:%ld:", pin->host, pin_port(pin, port));
    const char *sep = "";
    for (size_t j = i; j < request->pins_count; j++) {
      const ciphon_pin *same = &request->pins[j];
      if (pin_port(same, port) != pin_port(pin, port) || strcasecmp(same->host, pin->host)) continue;
      // curl reads v6 addresses in brackets.
      fprintf(f, strchr(same->address, ':') ? "%s[%s]" : "%s%s", sep, same->address);
      sep = ",";
    }
    fclose(f);
    list = curl_slist_append(list, line);
    free(line);
    char removal[320];
    snprintf(removal, sizeof removal, "-%s:%ld", pin->host, pin_port(pin, port));
    unpin = curl_slist_append(unpin, removal);
  }
  if (!ok) {
    curl_slist_free_all(list);
    curl_slist_free_all(unpin);
    return false;
  }
  curl_slist_free_all(self->unpin);
  self->unpin = unpin;
  *out = list;
  return true;
}

static ciphon_error client_send(void *userdata, const ciphon_request *request, ciphon_response *response,
                                char **message) {
  client *self = userdata;
  *response = (ciphon_response){};
  buffer b = {.request = request,
              .sink = ciphon_request_sink(request),
              .source = ciphon_request_source(request),
              .response = response};
  struct curl_slist *headers = nullptr;
  for (size_t i = 0; i < request->headers_count; i++) {
    // a line of its own length, a token in a header may take kilobytes.
    const ciphon_header *h = &request->headers[i];
    size_t len = strlen(h->name) + strlen(h->value) + 3;
    char *line = malloc(len);
    if (!line) continue;
    snprintf(line, len, "%s: %s", h->name, h->value);
    headers = curl_slist_append(headers, line);
    free(line);
  }
  char error[CURL_ERROR_SIZE] = {};

  pthread_mutex_lock(&self->lock);
  struct curl_slist *resolve = nullptr;
  if ((request->pins_count || self->unpin) && !resolve_entries(self, request, &resolve)) {
    pthread_mutex_unlock(&self->lock);
    curl_slist_free_all(headers);
    *message = strdup("a pin names no host, no address or no port");
    return CIPHON_ERROR_INVALID_REQUEST;
  }
  CURL *curl = self->curl;
  // the handle is reused so connections stay alive between requests, only the options are reset.
  curl_easy_reset(curl);
  curl_easy_setopt(curl, CURLOPT_URL, request->url);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method_name(request->method));
  if (request->method == CIPHON_METHOD_HEAD) curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  if (b.source) {
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, on_read);
    curl_easy_setopt(curl, CURLOPT_READDATA, &b);
    curl_easy_setopt(curl, CURLOPT_SEEKFUNCTION, on_seek);
    curl_easy_setopt(curl, CURLOPT_SEEKDATA, &b);
    curl_off_t len = b.source->len == CIPHON_LENGTH_UNKNOWN ? -1 : (curl_off_t)b.source->len;
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, len);
  } else if (request->body_len || request->method == CIPHON_METHOD_POST) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body ? (const char *)request->body : "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request->body_len);
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, request->follow_redirects ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, protocol_names(request->protocols));
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, protocol_names(request->protocols));
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (request->timeout_ms) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)request->timeout_ms);
  if (request->connect_timeout_ms)
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)request->connect_timeout_ms);
  // an empty proxy overrides the proxy variables of the environment.
  if (request->no_proxy) curl_easy_setopt(curl, CURLOPT_PROXY, "");
  if (resolve) curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
  if (request->pins_count) {
    // connections are matched by host name, one opened before the pins could lead elsewhere.
    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
  }
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &b);
  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  char *effective = nullptr;
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
  if (rc == CURLE_OK && effective) response->url = strdup(effective);
  pthread_mutex_unlock(&self->lock);
  curl_slist_free_all(headers);
  curl_slist_free_all(resolve);

  if (rc != CURLE_OK) {
    ciphon_response_dispose(response);
    if (b.too_large) {
      *message = malloc(64);
      if (*message)
        snprintf(*message, 64, "the response body exceeds %llu bytes", (unsigned long long)request->max_response_len);
      return CIPHON_ERROR_TOO_LARGE;
    }
    if (b.stopped) {
      *message = strdup("the sink stopped the transfer");
      return CIPHON_ERROR_CANCELLED;
    }
    if (b.source_failed) {
      *message = strdup("the source stopped the transfer");
      return CIPHON_ERROR_CANCELLED;
    }
    *message = strdup(*error ? error : curl_easy_strerror(rc));
    return error_code(rc);
  }
  response->status = (uint16_t)status;
  return CIPHON_OK;
}

static void client_release(void *userdata) {
  client *self = userdata;
  curl_easy_cleanup(self->curl);
  curl_slist_free_all(self->unpin);
  pthread_mutex_destroy(&self->lock);
  free(self);
}

ciphon_client ciphon_curl(void) {
  pthread_once(&global_once, global_init);
  client *self = calloc(1, sizeof *self);
  self->curl = curl_easy_init();
  pthread_mutex_init(&self->lock, nullptr);
  return (ciphon_client){.userdata = self, .send = client_send, .release = client_release};
}

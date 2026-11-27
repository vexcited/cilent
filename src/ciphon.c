#include <ciphon/ciphon.h>

#include <errno.h>
#include <stdarg.h>
#include <stddefer.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#elif __has_include(<unistd.h>)
#include <unistd.h>
#endif

// a request sent with ciphon_stream or ciphon_upload. clients write to `sink`, which counts the bytes against the
// cap before handing them to `target`, and read from `source`, which keeps `origin` within its length.
typedef struct stream {
  const ciphon_request *request;
  ciphon_sink sink;
  ciphon_sink target;
  ciphon_source source;
  ciphon_source origin;
  uint64_t received;
  bool streams;
  bool uploads;
  bool taken;
  bool source_taken;
  bool too_large;
} stream;

// the request being streamed on this thread, clients find its sink from the request they are given.
static thread_local stream *current;

void ciphon_response_dispose(ciphon_response *response) {
  for (size_t i = 0; i < response->headers_count; i++) {
    free((void *)response->headers[i].name);
    free((void *)response->headers[i].value);
  }
  free(response->headers);
  free(response->body);
  free(response->url);
  *response = (ciphon_response){};
}

// turns the outcome of a send into `code`, the message of a failed send is replaced.
[[nodiscard, gnu::cold, gnu::format(printf, 5, 6)]]
static ciphon_error fail(ciphon_error sent, ciphon_error code, ciphon_response *response, char **message,
                         const char *fmt, ...) {
  ciphon_response_dispose(response);
  if (sent != CIPHON_OK) free(*message);
  va_list ap;
  va_start(ap, fmt);
  *message = malloc(64);
  if (*message) vsnprintf(*message, 64, fmt, ap);
  va_end(ap);
  return code;
}

static bool stream_write(void *userdata, const ciphon_response *response, const uint8_t *data, size_t len) {
  stream *s = userdata;
  s->received += len;
  uint64_t max = s->request->max_response_len;
  if (max && s->received > max) {
    s->too_large = true;
    return false;
  }
  return s->target.write(s->target.userdata, response, data, len);
}

static bool source_read(void *userdata, uint64_t offset, uint8_t *data, size_t cap, size_t *got) {
  stream *s = userdata;
  uint64_t len = s->origin.len;
  *got = 0;
  if (len != CIPHON_LENGTH_UNKNOWN) {
    if (offset >= len) return offset == len;
    if (cap > len - offset) cap = (size_t)(len - offset);
  }
  if (!s->origin.read(s->origin.userdata, offset, data, cap, got)) return false;
  if (*got > cap) return false;
  // a body ending before its announced length would leave the server waiting.
  return *got || len == CIPHON_LENGTH_UNKNOWN;
}

[[nodiscard]] static ciphon_error send_with(const ciphon_client *client, const ciphon_request *request, stream *s,
                                            ciphon_response *response, char **message) {
  // a plain send keeps the stream of an outer one, a client wrapping another one passes its sink along.
  stream *outer = current;
  if (s) current = s;
  ciphon_error code = client->send(client->userdata, request, response, message);
  current = outer;
  // clients may not know the cap, the body they buffered is checked again.
  unsigned long long max = request->max_response_len;
  if ((s && s->too_large) || (code == CIPHON_OK && max && response->body_len > max))
    return fail(code, CIPHON_ERROR_TOO_LARGE, response, message, "the response body exceeds %llu bytes", max);
  if (code == CIPHON_OK && s && s->uploads && !s->source_taken)
    return fail(code, CIPHON_ERROR_INVALID_REQUEST, response, message, "the client does not stream request bodies");
  if (code != CIPHON_OK || !s || !s->streams || s->taken) return code;

  bool more = !response->body_len || stream_write(s, response, response->body, response->body_len);
  free(response->body);
  response->body = nullptr;
  response->body_len = 0;
  if (!more) return fail(code, CIPHON_ERROR_CANCELLED, response, message, "the sink stopped the transfer");
  return code;
}

ciphon_error ciphon_send(const ciphon_client *client, const ciphon_request *request, ciphon_response *response,
                         char **message) {
  char *ignored = nullptr;
  defer free(ignored);
  return send_with(client, request, nullptr, response, message ? message : &ignored);
}

ciphon_error ciphon_stream(const ciphon_client *client, const ciphon_request *request, ciphon_sink sink,
                           ciphon_response *response, char **message) {
  stream s = {.request = request, .target = sink, .streams = true};
  s.sink = (ciphon_sink){.userdata = &s, .write = stream_write};
  char *ignored = nullptr;
  defer free(ignored);
  return send_with(client, request, &s, response, message ? message : &ignored);
}

ciphon_error ciphon_upload(const ciphon_client *client, const ciphon_request *request, ciphon_source source,
                           ciphon_response *response, char **message) {
  stream s = {.request = request, .origin = source, .uploads = true};
  s.source = (ciphon_source){.userdata = &s, .len = source.len, .read = source_read};
  char *ignored = nullptr;
  defer free(ignored);
  return send_with(client, request, &s, response, message ? message : &ignored);
}

const ciphon_sink *ciphon_request_sink(const ciphon_request *request) {
  if (!current || current->request != request || !current->streams) return nullptr;
  current->taken = true;
  return &current->sink;
}

const ciphon_source *ciphon_request_source(const ciphon_request *request) {
  if (!current || current->request != request || !current->uploads) return nullptr;
  current->source_taken = true;
  return &current->source;
}

#if defined(_WIN32)
// windows reads at an offset through the handle under the descriptor, which pread is elsewhere.
static bool fd_read(void *userdata, uint64_t offset, uint8_t *data, size_t cap, size_t *got) {
  HANDLE h = (HANDLE)_get_osfhandle((int)(intptr_t)userdata);
  OVERLAPPED at = {.Offset = (DWORD)offset, .OffsetHigh = (DWORD)(offset >> 32)};
  DWORD n = 0;
  if (h == INVALID_HANDLE_VALUE) return false;
  if (!ReadFile(h, data, cap > 0x7fffffff ? 0x7fffffff : (DWORD)cap, &n, &at) && GetLastError() != ERROR_HANDLE_EOF)
    return false;
  *got = n;
  return true;
}

ciphon_source ciphon_source_fd(int fd, uint64_t len) {
  return (ciphon_source){.userdata = (void *)(intptr_t)fd, .len = len, .read = fd_read};
}
#elif __has_include(<unistd.h>)
static bool fd_read(void *userdata, uint64_t offset, uint8_t *data, size_t cap, size_t *got) {
  for (;;) {
    ssize_t n = pread((int)(intptr_t)userdata, data, cap, (off_t)offset);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) return false;
    *got = (size_t)n;
    return true;
  }
}

ciphon_source ciphon_source_fd(int fd, uint64_t len) {
  return (ciphon_source){.userdata = (void *)(intptr_t)fd, .len = len, .read = fd_read};
}
#endif

const char *ciphon_response_header(const ciphon_response *response, const char *name) {
  for (size_t i = 0; i < response->headers_count; i++) {
    if (strcasecmp(response->headers[i].name, name) == 0) return response->headers[i].value;
  }
  return nullptr;
}

void ciphon_client_release(ciphon_client *client) {
  if (client->release) client->release(client->userdata);
  *client = (ciphon_client){};
}

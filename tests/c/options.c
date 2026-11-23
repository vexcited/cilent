#include <ciphon/curl.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EXPECT(condition, what)                     \
  do {                                              \
    if (!(condition)) {                             \
      printf("FAIL line %d: %s\n", __LINE__, what); \
      return 1;                                     \
    }                                               \
  } while (0)

enum { UPLOAD_LEN = 100000 };

static char base[128];
static ciphon_client curl;
static ciphon_response response;
static char *message;

static const char *url(const char *path) {
  static char out[256];
  snprintf(out, sizeof out, "%s%s", base, path);
  return out;
}

static ciphon_error fetch(ciphon_request request) {
  ciphon_response_dispose(&response);
  free(message);
  message = nullptr;
  if (!request.timeout_ms) request.timeout_ms = 5000;
  return ciphon_send(&curl, &request, &response, &message);
}

static bool body_is(const char *text) {
  return response.body_len == strlen(text) && !memcmp(response.body, text, response.body_len);
}

// the bytes `i % 251`, which the test server checks, cut short at `end` and stopped at `stop`.
typedef struct pattern {
  uint64_t end;
  uint64_t stop;
} pattern;

static bool pattern_read(void *userdata, uint64_t offset, uint8_t *data, size_t cap, size_t *got) {
  pattern *p = userdata;
  if (p->stop && offset >= p->stop) return false;
  size_t n = 0;
  while (n < cap && offset + n < p->end && n < 3000) {
    data[n] = (uint8_t)((offset + n) % 251);
    n++;
  }
  *got = n;
  return true;
}

static ciphon_error upload(const char *path, ciphon_source source, const ciphon_header *headers, size_t count) {
  ciphon_response_dispose(&response);
  free(message);
  message = nullptr;
  ciphon_request request = {.method = CIPHON_METHOD_PUT,
                            .url = url(path),
                            .headers = headers,
                            .headers_count = count,
                            .follow_redirects = true,
                            .timeout_ms = 5000};
  return ciphon_upload(&curl, &request, source, &response, &message);
}

static ciphon_error buffered_send(void *, const ciphon_request *, ciphon_response *response, char **) {
  *response = (ciphon_response){.status = 200};
  return CIPHON_OK;
}

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void) {
  const char *env = getenv("CIPHON_TEST_URL");
  EXPECT(env, "CIPHON_TEST_URL is set");
  snprintf(base, sizeof base, "%s", env);
  const char *port = strrchr(base, ':') + 1;
  curl = ciphon_curl();

  EXPECT(fetch((ciphon_request){.url = url("/redirect"), .follow_redirects = true}) == CIPHON_OK, "redirect");
  EXPECT(response.url && !strcmp(response.url, url("/hello")) && body_is("hello"), "the final url");
  EXPECT(fetch((ciphon_request){.url = url("/redirect")}) == CIPHON_OK, "a redirect not followed");
  EXPECT(response.status == 302 && !strcmp(response.url, url("/redirect")), "its own url");

  EXPECT(fetch((ciphon_request){.url = url("/redirect-ftp"), .follow_redirects = true}) ==
             CIPHON_ERROR_INVALID_REQUEST,
         "a redirect to another scheme");
  EXPECT(fetch((ciphon_request){.url = url("/hello"), .protocols = CIPHON_PROTOCOL_HTTPS}) ==
             CIPHON_ERROR_INVALID_REQUEST,
         "http refused when only https is allowed");
  EXPECT(fetch((ciphon_request){.url = url("/redirect"),
                               .follow_redirects = true,
                               .protocols = CIPHON_PROTOCOL_HTTP | CIPHON_PROTOCOL_HTTPS}) == CIPHON_OK &&
             body_is("hello"),
         "both schemes");

  char pinned[128];
  snprintf(pinned, sizeof pinned, "http://pinned.invalid:%s/hello", port);
  ciphon_pin pins[] = {{.host = "pinned.invalid", .address = "::1"}, {.host = "PINNED.invalid", .address = "127.0.0.1"}};
  EXPECT(fetch((ciphon_request){.url = pinned, .pins = pins, .pins_count = 2}) == CIPHON_OK && body_is("hello"),
         "a pinned host");
  EXPECT(strstr(response.url, "pinned.invalid"), "the url keeps its host");
  ciphon_pin by_port[] = {{.host = "pinned.invalid", .port = (uint16_t)atoi(port), .address = "127.0.0.1"}};
  EXPECT(fetch((ciphon_request){.url = pinned, .pins = by_port, .pins_count = 1}) == CIPHON_OK, "a pin by port");
  EXPECT(fetch((ciphon_request){.url = pinned}) == CIPHON_ERROR_NETWORK, "pins do not outlive their request");
  ciphon_pin other_port[] = {{.host = "pinned.invalid", .port = 1, .address = "127.0.0.1"}};
  EXPECT(fetch((ciphon_request){.url = pinned, .pins = other_port, .pins_count = 1}) == CIPHON_ERROR_NETWORK,
         "a pin on another port");
  ciphon_pin broken[] = {{.host = "pinned.invalid"}};
  EXPECT(fetch((ciphon_request){.url = pinned, .pins = broken, .pins_count = 1}) == CIPHON_ERROR_INVALID_REQUEST,
         "a pin without an address");

  setenv("http_proxy", "http://127.0.0.1:1", 1);
  EXPECT(fetch((ciphon_request){.url = url("/hello")}) == CIPHON_ERROR_NETWORK, "the proxy of the environment");
  EXPECT(fetch((ciphon_request){.url = url("/hello"), .no_proxy = true}) == CIPHON_OK && body_is("hello"),
         "no proxy");
  unsetenv("http_proxy");

  // test-net-1 is never routed, its connections hang or fail at once without a route.
  int64_t started = now_ms();
  ciphon_error stalled = fetch((ciphon_request){.url = "http://192.0.2.1/", .connect_timeout_ms = 200});
  EXPECT(stalled == CIPHON_ERROR_TIMEOUT || stalled == CIPHON_ERROR_NETWORK, "an unreachable address");
  EXPECT(now_ms() - started < 3000, "the connect timeout is shorter than the total one");

  char path[] = "/tmp/ciphon-upload-XXXXXX";
  int fd = mkstemp(path);
  EXPECT(fd >= 0, "a temporary file");
  unlink(path);
  uint8_t chunk[UPLOAD_LEN];
  for (size_t i = 0; i < UPLOAD_LEN; i++) chunk[i] = (uint8_t)(i % 251);
  EXPECT(write(fd, chunk, UPLOAD_LEN) == UPLOAD_LEN, "write the file");
  // more than the source length, which never goes out.
  EXPECT(write(fd, "tail", 4) == 4, "write past the length");
  lseek(fd, 7, SEEK_SET);
  EXPECT(upload("/upload", ciphon_source_fd(fd, UPLOAD_LEN), nullptr, 0) == CIPHON_OK && body_is("sized 100000"),
         "a file");
  EXPECT(lseek(fd, 0, SEEK_CUR) == 7, "the file keeps its position");
  EXPECT(upload("/redirect-upload", ciphon_source_fd(fd, UPLOAD_LEN), nullptr, 0) == CIPHON_OK &&
             body_is("sized 100000"),
         "a body sent again after a redirect");
  close(fd);

  const ciphon_header no_expect[] = {{.name = "Expect", .value = ""}};
  pattern p = {.end = 54321};
  ciphon_source source = {.userdata = &p, .len = CIPHON_LENGTH_UNKNOWN, .read = pattern_read};
  EXPECT(upload("/upload", source, no_expect, 1) == CIPHON_OK && body_is("chunked 54321"), "a body of unknown length");

  p = (pattern){.end = 500};
  source.len = 1000;
  EXPECT(upload("/upload", source, nullptr, 0) == CIPHON_ERROR_CANCELLED && strstr(message, "source"),
         "a body shorter than its length");
  p = (pattern){.end = 50000, .stop = 9000};
  source.len = 50000;
  EXPECT(upload("/upload", source, nullptr, 0) == CIPHON_ERROR_CANCELLED, "a source stops the transfer");
  p = (pattern){.end = 50000};
  EXPECT(upload("/upload", source, nullptr, 0) == CIPHON_OK && body_is("sized 50000"), "the client works after");

  EXPECT(fetch((ciphon_request){.method = CIPHON_METHOD_POST,
                               .url = url("/echo"),
                               .body = (const uint8_t *)"plain",
                               .body_len = 5}) == CIPHON_OK &&
             body_is("PLAIN"),
         "plain bodies after a source");

  char token[5001];
  memset(token, 'k', sizeof token - 1);
  token[sizeof token - 1] = '\0';
  const ciphon_header long_header[] = {{.name = "X-Token", .value = token}};
  EXPECT(fetch((ciphon_request){.url = url("/token-length"), .headers = long_header, .headers_count = 1}) ==
                 CIPHON_OK &&
             body_is("5000"),
         "a header of several kilobytes is sent whole");

  ciphon_client buffered = {.send = buffered_send};
  ciphon_request request = {.method = CIPHON_METHOD_PUT, .url = "fake"};
  ciphon_response_dispose(&response);
  free(message);
  message = nullptr;
  EXPECT(ciphon_upload(&buffered, &request, source, &response, &message) == CIPHON_ERROR_INVALID_REQUEST,
         "a client that does not stream request bodies");
  EXPECT(strstr(message, "stream") && !response.status, "its message");

  ciphon_response_dispose(&response);
  free(message);
  ciphon_client_release(&curl);
  puts("c options: all checks passed");
  return 0;
}

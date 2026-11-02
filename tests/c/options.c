#include <ciphon/curl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define EXPECT(condition, what)                     \
  do {                                              \
    if (!(condition)) {                             \
      printf("FAIL line %d: %s\n", __LINE__, what); \
      return 1;                                     \
    }                                               \
  } while (0)

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

  ciphon_response_dispose(&response);
  free(message);
  ciphon_client_release(&curl);
  puts("c options: all checks passed");
  return 0;
}

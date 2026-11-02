// response bodies streamed to a sink, through the libcurl client and through a client that buffers them.
#include <ciphon/curl.h>
#include <stdio.h>
#include <string.h>

#define EXPECT(condition, what)                     \
  do {                                              \
    if (!(condition)) {                             \
      printf("FAIL line %d: %s\n", __LINE__, what); \
      return 1;                                     \
    }                                               \
  } while (0)

typedef struct collected {
  size_t len;
  size_t chunks;
  size_t stop_after;
  uint16_t status;
  bool sized;
  char first[8];
} collected;

static bool collect(void *userdata, const ciphon_response *response, const uint8_t *data, size_t len) {
  collected *c = userdata;
  if (!c->chunks) memcpy(c->first, data, len < sizeof c->first - 1 ? len : sizeof c->first - 1);
  c->status = response->status;
  c->sized = ciphon_response_header(response, "content-length") != nullptr;
  c->len += len;
  c->chunks++;
  return !c->stop_after || c->chunks < c->stop_after;
}

static ciphon_error buffered_send(void *, const ciphon_request *, ciphon_response *response, char **) {
  *response = (ciphon_response){.status = 200, .body = (uint8_t *)strdup("fake"), .body_len = 4};
  return CIPHON_OK;
}

static ciphon_error stream(const ciphon_client *client, const char *url, uint64_t max, collected *c,
                           ciphon_response *response, char **message) {
  ciphon_request request = {.method = CIPHON_METHOD_GET, .url = url, .timeout_ms = 5000, .max_response_len = max};
  return ciphon_stream(client, &request, (ciphon_sink){.userdata = c, .write = collect}, response, message);
}

int main(void) {
  const char *base = getenv("CIPHON_TEST_URL");
  EXPECT(base, "CIPHON_TEST_URL is set");
  char url[256];
  ciphon_client curl = ciphon_curl();
  ciphon_response response = {};
  char *message = nullptr;

  collected c = {};
  snprintf(url, sizeof url, "%s/bytes/100000", base);
  EXPECT(stream(&curl, url, 0, &c, &response, &message) == CIPHON_OK, "stream");
  EXPECT(c.len == 100000 && c.chunks > 1 && c.status == 200 && c.sized, "chunks with the status and headers");
  EXPECT(response.status == 200 && !response.body && !response.body_len, "the body is not buffered");
  EXPECT(ciphon_response_header(&response, "content-length"), "the headers stay in the response");
  ciphon_response_dispose(&response);

  c = (collected){.stop_after = 1};
  EXPECT(stream(&curl, url, 0, &c, &response, &message) == CIPHON_ERROR_CANCELLED, "a sink stops the transfer");
  EXPECT(c.chunks == 1 && strstr(message, "sink"), "cancel message");
  free(message);
  message = nullptr;

  c = (collected){};
  snprintf(url, sizeof url, "%s/stream/100000", base);
  EXPECT(stream(&curl, url, 10000, &c, &response, &message) == CIPHON_ERROR_TOO_LARGE, "streamed bodies are capped");
  EXPECT(c.len <= 10000 && strstr(message, "10000"), "the sink never sees more than the cap");
  free(message);
  message = nullptr;

  c = (collected){};
  snprintf(url, sizeof url, "%s/hello", base);
  EXPECT(ciphon_send(&curl, &(ciphon_request){.url = url}, &response, nullptr) == CIPHON_OK, "plain send after");
  EXPECT(response.body_len == 5 && !memcmp(response.body, "hello", 5), "plain sends still buffer");
  ciphon_response_dispose(&response);
  ciphon_client_release(&curl);

  ciphon_client buffered = {.send = buffered_send};
  c = (collected){};
  EXPECT(stream(&buffered, "fake", 0, &c, &response, &message) == CIPHON_OK, "a buffering client");
  EXPECT(c.chunks == 1 && c.len == 4 && !strcmp(c.first, "fake") && !response.body, "one chunk");
  ciphon_response_dispose(&response);

  c = (collected){.stop_after = 1};
  EXPECT(stream(&buffered, "fake", 0, &c, &response, &message) == CIPHON_ERROR_CANCELLED, "its sink can stop");
  free(message);
  message = nullptr;

  c = (collected){};
  EXPECT(stream(&buffered, "fake", 3, &c, &response, &message) == CIPHON_ERROR_TOO_LARGE, "its body is capped");
  EXPECT(!c.chunks && !response.body, "nothing reaches the sink");
  free(message);

  puts("c stream: all checks passed");
  return 0;
}

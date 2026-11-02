// the net library used from c with the libcurl client.
#include <ciphon/curl.h>
#include <stdio.h>
#include <string.h>

#include "net.h"

#define EXPECT(condition, what)                 \
  do {                                          \
    if (!(condition)) {                         \
      printf("FAIL line %d: %s\n", __LINE__, what); \
      return 1;                                 \
    }                                           \
  } while (0)

int main(void) {
  const char *base = getenv("CIPHON_TEST_URL");
  EXPECT(base, "CIPHON_TEST_URL is set");
  net_client *client = net_client_new(base, ciphon_curl());

  net_reply reply;
  EXPECT(net_client_get(client, "/hello", true, &reply) == NET_OK, "get");
  EXPECT(reply.status == 200 && strcmp(reply.body, "hello") == 0, "status and body");
  EXPECT(reply.cookies == 2 && strncmp(reply.cookie, "session=abc", 11) == 0, "repeated headers stay separate");
  net_reply_dispose(&reply);

  EXPECT(net_client_get(client, "/redirect", false, &reply) == NET_OK, "redirect");
  EXPECT(reply.status == 302 && strcmp(reply.location, "/hello") == 0, "redirects can be read");
  net_reply_dispose(&reply);

  EXPECT(net_client_post(client, "/echo", "shout", &reply) == NET_OK && strcmp(reply.body, "SHOUT") == 0, "post");
  net_reply_dispose(&reply);

  EXPECT(net_client_get_capped(client, "/bytes/1000", 1000, &reply) == NET_OK && strlen(reply.body) == 1000,
         "a body at the cap");
  net_reply_dispose(&reply);
  EXPECT(net_client_get_capped(client, "/bytes/100000", 1000, &reply) == NET_ERROR_TOO_LARGE, "content-length cap");
  EXPECT(strstr(net_last_error(), "exceeds 1000 bytes"), "cap message");
  EXPECT(net_client_get_capped(client, "/huge", 1000, &reply) == NET_ERROR_TOO_LARGE, "refused from its headers");
  EXPECT(net_client_get_capped(client, "/stream/100000", 1000, &reply) == NET_ERROR_TOO_LARGE, "streamed cap");
  EXPECT(net_client_get_capped(client, "/hello", 1000, &reply) == NET_OK, "the client works after a cap");
  net_reply_dispose(&reply);

  EXPECT(net_client_get(client, "/missing", true, &reply) == NET_ERROR_STATUS, "error status");
  EXPECT(strstr(net_last_error(), "404"), "error message");
  net_client_free(client);

  net_client *offline = net_client_new("http://127.0.0.1:1", ciphon_curl());
  EXPECT(net_client_get(offline, "/", true, &reply) == NET_ERROR_TRANSPORT, "transport errors");
  net_client_free(offline);

  puts("c: all checks passed");
  return 0;
}

// a library sending its requests through the ciphon client it is given.
#pragma once

#include <ciphon/ciphon.h>

typedef enum CIFFI_ERROR net_error : int32_t {
  NET_OK = 0,
  /// the request failed before a response arrived.
  NET_ERROR_TRANSPORT = 1,
  /// the server answered with an error status.
  NET_ERROR_STATUS = 2,
} net_error;

CIFFI_ERROR_MESSAGE const char *net_last_error(void);

/// what a request returned.
typedef struct CIFFI_RECORD net_reply {
  uint16_t status;
  char *body;
  /// the first `set-cookie` header, if any.
  CIFFI_OPTIONAL char *cookie;
  /// the `location` of a redirect that was not followed.
  CIFFI_OPTIONAL char *location;
  uint32_t cookies;
} net_reply;

CIFFI_DESTRUCTOR void net_reply_dispose(net_reply *reply);

/// a client sending its requests through the given http client.
typedef struct CIFFI_OBJECT net_client net_client;

CIFFI_CONSTRUCTOR net_client *net_client_new(const char *base_url, ciphon_client http);
CIFFI_DESTRUCTOR void net_client_free(net_client *client);

/// fetches `path`, following redirects when `follow` is set.
CIFFI_EXPORT CIFFI_ASYNC net_error net_client_get(net_client *client, const char *path, bool follow, net_reply *reply);
/// posts `body` to `path`.
CIFFI_EXPORT CIFFI_ASYNC net_error net_client_post(net_client *client, const char *path, const char *body,
                                                   net_reply *reply);


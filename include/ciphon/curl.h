// the libcurl implementation of ciphon_client, for c programs.
#ifndef CIPHON_CURL_H
#define CIPHON_CURL_H

#include "ciphon.h"

#ifdef __cplusplus
extern "C" {
#endif

// a client backed by its own curl handle, released with ciphon_client_release or by the library given it.
ciphon_client ciphon_curl(void);

#ifdef __cplusplus
}
#endif

#endif

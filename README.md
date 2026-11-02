# ciphon

*Suck data from the internet.* A small HTTP client interface for C libraries, part of CiKo.

A library takes a `ciphon_client`, a struct of function pointers, and sends its requests through it. It never
links an HTTP stack itself, so each platform does the networking with its own TLS, proxies and certificates:

| Platform | Client | Target |
| --- | --- | --- |
| C | `ciphon_curl()` from `<ciphon/curl.h>`, or your own | `ciphon::curl` |
| Swift | `CiphonURLSession()` | module `Ciphon` |
| Kotlin | `CiphonOkHttp()` | package `dev.ciko.ciphon` |

TLS certificates are always verified.

## In a library

```c
#include <ciphon/ciphon.h>

CIFFI_CONSTRUCTOR app_client *app_client_new(const char *base_url, ciphon_client http);
```

```c
ciphon_header headers[] = {{.name = "Accept", .value = "application/json"}};
ciphon_request request = {
  .method = CIPHON_METHOD_GET,
  .url = "https://api.example.com/items",
  .headers = headers,
  .headers_count = 1,
  .follow_redirects = true,
};
ciphon_response response = {};
char *message = nullptr;
if (ciphon_send(&client->http, &request, &response, &message) == CIPHON_OK) {
  // response.status, response.headers, response.body and response.body_len.
  ciphon_response_dispose(&response);
}
free(message);
```

Release the client with `ciphon_client_release` once the library no longer needs it.

Set `max_response_len` to refuse large bodies: the send fails with `CIPHON_ERROR_TOO_LARGE` as soon as an uncompressed `Content-Length` or the bytes received go over it, so the transfer stops early instead of buffering the whole body. The curl, URLSession and OkHttp clients all stop early, and `ciphon_send` checks the body again for clients of your own that ignore the cap.

```cmake
add_subdirectory(path/to/ciphon ciphon)
target_link_libraries(app PUBLIC ciphon::ciphon)
```

## With ciffi

ciphon comes with [ciffi](../ciffi) bindings. Generate yours with `DEPENDS ciphon` and the `http` param of
`app_client_new` defaults to the platform client:

```cmake
ciffi_generate(app HEADERS include/app.h NAMESPACE app DEPENDS ciphon SWIFT KOTLIN_PACKAGE dev.example.app)
```

```swift
let client = AppClient(baseUrl: "https://api.example.com")   // CiphonURLSession
```

```kotlin
val client = AppClient("https://api.example.com")            // CiphonOkHttp
```

ciphon fetches ciffi unless the parent project already provides `ciffi_generate`, set
`FETCHCONTENT_SOURCE_DIR_CIFFI` to use a local checkout. The Kotlin client needs OkHttp: Android apps get it from
Gradle, JVM builds set `CIPHON_OKHTTP_CLASSPATH` or turn on `CIPHON_FETCH_OKHTTP`. Turn `CIPHON_BINDINGS` off to
build only the C libraries.

## In C

C programs pass `ciphon_curl()`, or any client with a `send` and a `release`: WinHTTP, libsoup, an existing curl
handle or a test double. `send` blocks, so call the library from a worker thread in an event loop.

```c
static ciphon_error my_send(void *userdata, const ciphon_request *request, ciphon_response *response,
                            char **message) {
  // fill response with malloc'd headers and body, or return an error with an owned message.
}

app_client *client = app_client_new(url, (ciphon_client){.userdata = ctx, .send = my_send, .release = my_release});
```

Installed, ciphon is a pair of static libraries and its headers. `<ciphon/ciffi.h>` is a copy of the ciffi
markers, so C users never need ciffi.

## Build and test

```sh
cmake -S . -B build -G Ninja -DFETCHCONTENT_SOURCE_DIR_CIFFI=../ciffi
ninja -C build
ctest --test-dir build
```

The tests run a local server with `python3` and check the same library from C, Swift and Kotlin.

## License

[MIT](LICENSE)

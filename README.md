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

```cmake
add_subdirectory(path/to/ciphon ciphon)
target_link_libraries(app PUBLIC ciphon::ciphon)
```

Set `max_response_len` to refuse large bodies: the send fails with `CIPHON_ERROR_TOO_LARGE` as soon as an uncompressed `Content-Length` or the bytes received go over it, so the transfer stops early instead of buffering the whole body. The curl, URLSession and OkHttp clients all stop early, and `ciphon_send` checks the body again for clients of your own that ignore the cap.

### Safer requests

A library fetching URLs its users give can lock the request down:

```c
ciphon_pin pins[] = {{.host = "example.com", .address = "93.184.215.14"}};
ciphon_request request = {
  .method = CIPHON_METHOD_GET,
  .url = "https://example.com/feed",
  .timeout_ms = 8000,
  .connect_timeout_ms = 2000,
  .no_proxy = true,
  .protocols = CIPHON_PROTOCOL_HTTPS,
  .pins = pins,
  .pins_count = 1,
};
```

- `pins` connect to the addresses given instead of resolving their host, so the addresses a library checked against private ranges are the ones it reaches, without a DNS rebinding window. A pin's port defaults to the URL's, and repeating a host gives it several addresses. A redirect to another host resolves it as usual, so a library checking addresses follows redirects itself.
- `protocols` limits the schemes of the URL and of every redirect, a refused one fails with `CIPHON_ERROR_INVALID_REQUEST`. Clients only ever use `http` and `https`.
- `no_proxy` ignores the proxies of the environment, such as `http_proxy` for curl, and of the system.
- `connect_timeout_ms` bounds the connection, `timeout_ms` still bounds the whole request.

`response.url` is the URL that answered, after the redirects followed.

The curl and OkHttp clients support every option. URLSession has no connect timeout and cannot pin addresses, so `CiphonURLSession` ignores `connectTimeoutMs` and refuses a request with pins.

### Streaming

From C, `ciphon_stream` hands the body to a sink as it arrives instead of buffering it, to write a download to a file or hash it on the way. The response still gets the status and headers, its body stays empty. Return false from `write` to stop the transfer with `CIPHON_ERROR_CANCELLED`, and `max_response_len` caps the bytes the sink sees.

```c
static bool to_file(void *userdata, const ciphon_response *response, const uint8_t *data, size_t len) {
  return response->status == 200 && fwrite(data, 1, len, userdata) == len;
}

ciphon_error error = ciphon_stream(&client->http, &request, (ciphon_sink){.userdata = file, .write = to_file},
                                   &response, &message);
```

`ciphon_curl()` streams, other clients buffer the body and the sink then gets it in one chunk.

`ciphon_upload` sends a body a source reads as it goes, so a large file never sits in memory. `ciphon_source_fd` reads a file with `pread`, any other source fills a buffer from an offset, and a length of `CIPHON_LENGTH_UNKNOWN` sends the body chunked.

```c
ciphon_error error = ciphon_upload(&client->http, &request, ciphon_source_fd(fd, size), &response, &message);
```

Only clients streaming request bodies can upload, `ciphon_curl()` does. Another client fails with `CIPHON_ERROR_INVALID_REQUEST`, after sending the request without its body. The bindings do not expose streaming.

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

ciphon fetches ciffi unless the parent project already provides `ciffi_generate`, from a checkout at
`../ciko-ciffi` when there is one, or the one `FETCHCONTENT_SOURCE_DIR_CIFFI` points at. The Kotlin client needs OkHttp: Android apps get it from
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

To stream, `send` asks `ciphon_request_sink(request)` for the sink of the request and writes the body to it instead of the response. It gets null for a plain `ciphon_send`, and for a request it did not receive itself. Likewise `ciphon_request_source(request)` gives the source of an upload, whose reads stay within its length.

Installed, ciphon is a pair of static libraries and its headers. `<ciphon/ciffi.h>` is a copy of the ciffi
markers, so C users never need ciffi.

## Build and test

```sh
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build
```

The tests run a local server with `python3` and check the same library from C, Swift and Kotlin.

## License

[MIT](LICENSE)

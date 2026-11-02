#include <ciphon/ciphon.h>

#include <stdio.h>
#include <strings.h>

void ciphon_response_dispose(ciphon_response *response) {
  for (size_t i = 0; i < response->headers_count; i++) {
    free((void *)response->headers[i].name);
    free((void *)response->headers[i].value);
  }
  free(response->headers);
  free(response->body);
  *response = (ciphon_response){};
}

ciphon_error ciphon_send(const ciphon_client *client, const ciphon_request *request, ciphon_response *response,
                         char **message) {
  char *ignored = nullptr;
  ciphon_error code = client->send(client->userdata, request, response, message ? message : &ignored);
  free(ignored);
  // clients may not know the cap, the body they buffered is checked again.
  if (code == CIPHON_OK && request->max_response_len && response->body_len > request->max_response_len) {
    ciphon_response_dispose(response);
    code = CIPHON_ERROR_TOO_LARGE;
    if (message && (*message = malloc(64)))
      snprintf(*message, 64, "the response body exceeds %llu bytes", (unsigned long long)request->max_response_len);
  }
  return code;
}

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

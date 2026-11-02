#include <ciphon/ciphon.h>

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

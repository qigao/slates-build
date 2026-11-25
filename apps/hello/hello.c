#include <http_client/http.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
  if (argc != 4) {
    fprintf(stderr, "Usage: hello tcp://HOST:PORT HOST:PORT /path\n");
    return EXIT_FAILURE;
  }

  enum { timeout_ms = 5000 };
  const chttp_client_config config = {
      .network = {
#if defined(_WIN32)
          .backend = NATIVE_IO_BACKEND_IOCP,
#elif defined(__APPLE__)
          .backend = NATIVE_IO_BACKEND_KQUEUE,
#else
          .backend = NATIVE_IO_BACKEND_EPOLL,
#endif
          .connection_capacity = 1,
          .command_capacity = 8,
          .command_buffer_bytes = 256 * 1024,
          .request_capacity = 4,
          .completion_batch_capacity = 4,
          .event_capacity = 8,
          .max_send_bytes = 64 * 1024,
          .receive_buffer_bytes = 16 * 1024,
          .connect_timeout_ms = timeout_ms,
          .read_timeout_ms = timeout_ms,
          .write_timeout_ms = timeout_ms,
      },
      .request_capacity = 1,
      .max_start_line_bytes = 4096,
      .max_header_count = 64,
      .max_header_bytes = 32 * 1024,
      .max_request_body_bytes = 32 * 1024,
      .max_response_body_bytes = 1024 * 1024,
      .max_informational_responses = 4,
  };
  chttp_client client = {0};
  int status = chttp_client_init(&client, &config);
  if (status != SALTS_OK) {
    fprintf(stderr, "HTTP client initialization failed (%d)\n", status);
    return EXIT_FAILURE;
  }

  const chttp_options options = {
      .connection_uri = argv[1],
      .authority = argv[2],
      .target = argv[3],
      .timeout_ms = timeout_ms,
  };
  chttp_response response = {0};
  chttp_error error = {0};
  status = chttp_get(&client, &options, &response, &error);
  int result = EXIT_FAILURE;
  if (status == SALTS_OK) {
    printf("HTTP %u\n", response.status_code);
    if (response.body_size &&
        fwrite(response.body, 1, response.body_size, stdout) !=
            response.body_size) {
      fputs("Could not write the response body\n", stderr);
    } else if (response.status_code >= 200 && response.status_code < 300) {
      result = EXIT_SUCCESS;
    }
    chttp_response_destroy(&response);
  } else {
    fprintf(stderr, "HTTP GET failed (%d, native %d, stage %s)\n", status,
            error.native_status, error.stage ? error.stage : "unknown");
  }

  status = chttp_client_destroy(&client, timeout_ms);
  if (status != SALTS_OK) {
    fprintf(stderr, "HTTP client shutdown failed (%d)\n", status);
    return EXIT_FAILURE;
  }
  return result;
}

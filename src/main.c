#include "lab.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TEST
#define main main_exclude
#endif

static const char *const USAGE =
    "Usage: myapp -f <from> -t <to> [-s subject] [-b body] [-p port] [-H helo-host] <server>\n"
    "  -f <from> - envelope sender, for example you@example.com\n"
    "  -t <to> - envelope recipient\n"
    "  -s <subject> - subject line (default: empty)\n"
    "  -b <body> - message body (default: read from stdin)\n"
    "  -p <port> - port or service name (default: 25)\n"
    "  -H <helo-host> - host name sent with HELO (default: localhost)\n"
    "  <server> - host name or address of the mail server\n";

static char *read_stdin_body(void) {
  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc(cap);
  if (buf == NULL) {
    return NULL;
  }
  size_t n;
  
  while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
    len += n;
    if (len == cap) {
      cap *= 2;
      char *grown = realloc(buf, cap);
      if (grown == NULL) {
        free(buf);
        return NULL;
      }
      buf = grown;
    }
  }

  buf[len] = '\0';
  return buf;
}

int main(int argc, char **argv) {
  if (argc == 1) {
    printf("%s", USAGE);
    return 0;
  }

  const char *from = NULL;
  const char *to = NULL;
  const char *subject = "";
  const char *body_arg = NULL;
  const char *port = "25";
  const char *helo_host = "localhost";

  int opt;
  opterr = 0;
  while ((opt = getopt(argc, argv, "f:t:s:b:p:H:")) != -1) {
    switch (opt) {
      case 'f':
        from = optarg;
        break;
      case 't':
        to = optarg;
        break;
      case 's':
        subject = optarg;
        break;
      case 'b':
        body_arg = optarg;
        break;
      case 'p':
        port = optarg;
        break;
      case 'H':
        helo_host = optarg;
        break;
      default:
        fprintf(stderr, "myapp: unknown or malformed option\n%s", USAGE);
        return 1;
    }
  }

  if (optind != argc - 1) {
    fprintf(stderr, "myapp: exactly one server argument is required\n%s", USAGE);
    return 1;
  }
  const char *server = argv[optind];

  if (from == NULL || to == NULL) {
    fprintf(stderr, "myapp: -f and -t are required\n%s", USAGE);
    return 1;
  }

  if (smtp_has_crlf(from) || smtp_has_crlf(to) || smtp_has_crlf(subject) || smtp_has_crlf(helo_host) ||
      smtp_has_crlf(server)) {
    fprintf(stderr, "myapp: arguments may not contain a bare CR or LF\n");
    return 1;
  }

  char *body_owned = NULL;
  const char *body = body_arg;
  if (body == NULL) {
    body_owned = read_stdin_body();
    if(body_owned == NULL) {
      fprintf(stderr, "myapp: could not read message body from stdin\n");
      return 2;
    }
    body = body_owned;
  }

  char err_msg[256];
  int fd = smtp_connect(server, port, err_msg, sizeof(err_msg));
  if (fd < 0) {
    fprintf(stderr, "myapp: %s\n", err_msg);
    free(body_owned);
    return 2;
  }

  smtp_transport_t transport = {
      .read_fn = smtp_socket_read,
      .write_fn = smtp_socket_write,
      .ctx = &fd,
  };

  smtp_session_params_t params = {
      .helo_host = helo_host,
      .mail_from = from,
      .rcpt_to = to,
      .subject = subject,
      .body = body,
  };

  int rc = smtp_run_session(&transport, &params, err_msg, sizeof(err_msg));
  smtp_disconnect(fd);
  free(body_owned);

  if (rc != SMTP_OK) {
    fprintf(stderr, "myapp: %s\n", err_msg);
    return 2;
  }

  return 0;
}

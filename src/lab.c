#include "lab.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// Protocol Helpers

int smtp_parse_reply_code(const char *line, size_t len) {
  if (line == NULL || len < 3) {
    return -1;
  }

  int code = 0;
  for (size_t i = 0; i < 3; i++) {
    char c = line[i];
    if (c < '0' || c > '9') {
      return -1;
    }
    code = code * 10 + (c - '0');
  }
  return code;
}

bool smtp_reply_is_final(const char *line, size_t len) {
  if (line == NULL || len < 4) {
    return true;
  }
  return line[3] != '-';
}

bool smtp_has_crlf(const char *s) {
  if (s == NULL) {
    return false;
  }
  for (size_t i = 0; s[i] != '\0'; i++) {
    if (s[i] == '\r' || s[i] == '\n') {
      return true;
    }
  }
  return false;
}

char *smtp_build_command_line(const char *verb, const char *arg) {
  if (verb == NULL) {
    return NULL;
  }

  int needed = (arg != NULL) ? snprintf(NULL, 0, "%s %s\r\n", verb, arg)
                              : snprintf(NULL, 0, "%s\r\n", verb);
  if (needed < 0) {
    return NULL;
  }

  size_t size = (size_t)needed + 1;
  char *line = malloc(size);

  if (line == NULL) {
    return NULL;
  }

  if (arg != NULL) {
    snprintf(line, size, "%s %s\r\n", verb, arg);
  } else {
    snprintf(line, size, "%s\r\n", verb);
  }
  return line;
}

char *smtp_dot_stuff(const char *body) {
  if (body == NULL) {
    return NULL;
  }

  size_t body_len = strlen(body);
  size_t cap = body_len * 3 + 4;
  char *out = malloc(cap);
  if (out == NULL) {
    return NULL;
  } 

  size_t out_len = 0;
  size_t i = 0;
  bool at_line_start = true;

  while (i < body_len) {
    char c = body[i];

    if (c == '\r') {
      i++;
      continue;
    }

    if (at_line_start && c == '.') {
      out[out_len++] = '.';
    }

    if (c == '\n') {
      out[out_len++] = '\r';
      out[out_len++] = '\n';
      at_line_start = true;
    } else {
      out[out_len++] = c;
      at_line_start = false;
    }
    i++;
  }

  if (!at_line_start) {
    out[out_len++] = '\r';
    out[out_len++] = '\n';
  }

  out[out_len] = '\0';
  return out;
}

char *smtp_build_message(const char *from, const char *to, const char *subject, const char *body) {
  if (from == NULL || to == NULL || subject == NULL || body == NULL) {
    return NULL;
  }

  char *stuffed = smtp_dot_stuff(body);
  if (stuffed == NULL) {
    return NULL;
  }

  static const char *const fmt = "From: %s\r\nTo: %s\r\nSubject: %s\r\n\r\n%s.\r\n";
  int needed = snprintf(NULL, 0, fmt, from, to, subject, stuffed);
  if (needed < 0) {
    free(stuffed);
    return NULL;
  }

  size_t size = (size_t)needed + 1;
  char *message = malloc(size);
  if (message == NULL) {
    free(stuffed);
    return NULL;
  }

  snprintf(message, size, fmt, from, to, subject, stuffed);
  free(stuffed);
  return message;
}

// ssession, over a swappable transport

void smtp_reader_init(smtp_reader_t *reader, smtp_transport_t transport) {
  reader->transport = transport;
  reader->start = 0;
  reader->len = 0;
}

int smtp_reader_read_line(smtp_reader_t *reader, char *line_out, size_t line_out_size, size_t *out_len) {
  if (reader == NULL || line_out == NULL || line_out_size == 0) {
    return SMTP_ERR_INVALID_ARG;
  }

  for (;;) {
    for (size_t i = 0; i + 1 < reader->len; i++) {
      size_t idx = reader->start + i;
      if (reader->buf[idx] == '\r' && reader->buf[idx + 1] == '\n') {
        size_t line_len = i;
        if (line_len + 1 > line_out_size) {
          return SMTP_ERR_LINE_TOO_LONG;
        }
        memcpy(line_out, reader->buf + reader->start, line_len);
        line_out[line_len] = '\0';
        if (out_len != NULL) {
          *out_len = line_len;
        }
        reader->start += line_len + 2;
        reader->len -= line_len + 2;
        return SMTP_OK;
      }
    }

    if (reader->start > 0) {
      memmove(reader->buf, reader->buf + reader->start, reader->len);
      reader->start = 0;
    }

    if (reader->len == sizeof(reader->buf)) {
      return SMTP_ERR_LINE_TOO_LONG;
    }

    ssize_t n = reader->transport.read_fn(reader->transport.ctx, reader->buf + reader->len, sizeof(reader->buf) - reader->len);
    if (n <= 0) {
      return SMTP_ERR_IO;
    }
    reader->len += (size_t)n;
  }
}

int smtp_read_reply(smtp_reader_t *reader, smtp_reply_t *reply) {
  if (reader == NULL || reply == NULL) {
    return SMTP_ERR_INVALID_ARG;
  }

  reply->code = -1;
  reply->text[0] = '\0';
  size_t text_used = 0;
  bool first = true;

  for (;;) {
    char line[SMTP_LINE_MAX];
    size_t line_len = 0;
    int rc = smtp_reader_read_line(reader, line, sizeof(line), &line_len);
    if (rc != SMTP_OK) {
      return rc;
    }

    int code = smtp_parse_reply_code(line, line_len);
    if (code < 0) {
      return SMTP_ERR_PROTOCOL;
    }

    if (first) {
      reply->code = code;
      first = false;
    } else if (code != reply->code) {
      return SMTP_ERR_PROTOCOL;
    }

    size_t text_start = (line_len >= 4) ? 4 : line_len;
    size_t text_part_len = line_len - text_start;
    size_t space = (text_used < sizeof(reply->text) - 1) ? (sizeof(reply->text) - 1 - text_used) : 0;
    size_t copy_len = (text_part_len < space) ? text_part_len : space;
    memcpy(reply->text + text_used, line + text_start, copy_len);
    text_used += copy_len;
    reply->text[text_used] = '\0';

    if (smtp_reply_is_final(line, line_len)) {
      return SMTP_OK;
    }
  }
}

int smtp_write_all(smtp_transport_t *transport, const char *data, size_t len) {
  if (transport == NULL || data == NULL) {
    return SMTP_ERR_INVALID_ARG;
  }

  size_t sent = 0;
  while (sent < len) {
    ssize_t n = transport->write_fn(transport->ctx, data + sent, len - sent);
    if (n <= 0) {
      return SMTP_ERR_IO;
    }
    sent += (size_t)n;
  }
  return SMTP_OK;
}

int smtp_send_command(smtp_transport_t *transport, smtp_reader_t *reader, const char *verb,
                       const char *arg, int expected_code, smtp_reply_t *reply) {
  if (transport == NULL || reader == NULL || verb == NULL || reply == NULL) {
    return SMTP_ERR_INVALID_ARG;
  }

  char *line = smtp_build_command_line(verb, arg);
  if (line == NULL) {
    return SMTP_ERR_INVALID_ARG;
  }

  int rc = smtp_write_all(transport, line, strlen(line));
  free(line);
  if (rc != SMTP_OK) {
    return rc;
  }

  rc = smtp_read_reply(reader, reply);
  if (rc != SMTP_OK) {
    return rc;
  }

  if (reply->code != expected_code) {
    return SMTP_ERR_PROTOCOL;
  }
  return SMTP_OK;
}

static void smtp_set_err(char *err_msg, size_t err_msg_size, const char *msg) {
  if (err_msg == NULL || err_msg_size == 0) {
    return;
  }
  snprintf(err_msg, err_msg_size, "%s", msg);
}

static void smtp_format_error(char *err_msg, size_t err_msg_size, const char *step, int rc,
                               const smtp_reply_t *reply) {
  if (err_msg == NULL || err_msg_size == 0) {
    return;
  }

  switch (rc) {
    case SMTP_ERR_IO:
      snprintf(err_msg, err_msg_size, "connection lost while waiting for the reply to %s", step);
      break;
    case SMTP_ERR_LINE_TOO_LONG:
      snprintf(err_msg, err_msg_size, "server reply to %s exceeded the maximum line length", step);
      break;
    default:
      if (reply->code < 0) {
        snprintf(err_msg, err_msg_size, "malformed reply from server for %s", step);
      } else {
        snprintf(err_msg, err_msg_size, "unexpected reply to %s: %d %s", step, reply->code, reply->text);
      }
      break;
  }
}

int smtp_run_session(smtp_transport_t *transport, const smtp_session_params_t *params, char *err_msg,
                      size_t err_msg_size) {
  if (transport == NULL || params == NULL) {
    return SMTP_ERR_INVALID_ARG;
  }

  if (smtp_has_crlf(params->helo_host) || smtp_has_crlf(params->mail_from) ||
      smtp_has_crlf(params->rcpt_to) || smtp_has_crlf(params->subject)) {
    smtp_set_err(err_msg, err_msg_size, "an argument contains a bare CR or LF");
    return SMTP_ERR_INVALID_ARG;
  }

  smtp_reader_t reader;
  smtp_reader_init(&reader, *transport);
  smtp_reply_t reply;

  int rc = smtp_read_reply(&reader, &reply);
  if (rc != SMTP_OK || reply.code != 220) {
    smtp_format_error(err_msg, err_msg_size, "the greeting", (rc != SMTP_OK) ? rc : SMTP_ERR_PROTOCOL, &reply);
    return (rc != SMTP_OK) ? rc : SMTP_ERR_PROTOCOL;
  }

  char mail_from_cmd[SMTP_LINE_MAX];
  char rcpt_to_cmd[SMTP_LINE_MAX];
  snprintf(mail_from_cmd, sizeof(mail_from_cmd), "MAIL FROM:<%s>", params->mail_from);
  snprintf(rcpt_to_cmd, sizeof(rcpt_to_cmd), "RCPT TO:<%s>", params->rcpt_to);

  struct {
    const char *verb;
    const char *arg;
    int expected;
    const char *label;
  } steps[] = {
      {"HELO", params->helo_host, 250, "HELO"},
      {mail_from_cmd, NULL, 250, "MAIL FROM"},
      {rcpt_to_cmd, NULL, 250, "RCPT TO"},
      {"DATA", NULL, 354, "DATA"},
  };

  for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
    rc = smtp_send_command(transport, &reader, steps[i].verb, steps[i].arg, steps[i].expected, &reply);
    if (rc != SMTP_OK) {
      smtp_format_error(err_msg, err_msg_size, steps[i].label, rc, &reply);
      return rc;
    }
  }

  char *message = smtp_build_message(params->mail_from, params->rcpt_to, params->subject, params->body);
  if (message == NULL) { // GCOVR_EXCL_START
    smtp_set_err(err_msg, err_msg_size, "failed to build the message body");
    return SMTP_ERR_INVALID_ARG;
  } // GCOVR_EXCL_STOP

  rc = smtp_write_all(transport, message, strlen(message));
  free(message);
  if (rc != SMTP_OK) {
    smtp_format_error(err_msg, err_msg_size, "sending the message body", rc, &reply);
    return rc;
  }

  rc = smtp_read_reply(&reader, &reply);
  if (rc != SMTP_OK || reply.code != 250) {
    smtp_format_error(err_msg, err_msg_size, "end of DATA", (rc != SMTP_OK) ? rc : SMTP_ERR_PROTOCOL, &reply);
    return (rc != SMTP_OK) ? rc : SMTP_ERR_PROTOCOL;
  }

  rc = smtp_send_command(transport, &reader, "QUIT", NULL, 221, &reply);
  if (rc != SMTP_OK) {
    smtp_format_error(err_msg, err_msg_size, "QUIT", rc, &reply);
    return rc;
  }

  return SMTP_OK;
}

// socket transport stuff

int smtp_connect(const char *host, const char *port, char *err_msg, size_t err_msg_size) {
  if (host == NULL || port == NULL) {
    smtp_set_err(err_msg, err_msg_size, "no server specified");
    return -1;
  }

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *results = NULL;
  int gai_rc = getaddrinfo(host, port, &hints, &results);
  if (gai_rc != 0) {
    snprintf(err_msg, err_msg_size, "could not resolve %s:%s: %s", host, port, gai_strerror(gai_rc));
    return -1;
  }

  int fd = -1;
  for (struct addrinfo *rp = results; rp != NULL; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(results);

  if (fd < 0) {
    snprintf(err_msg, err_msg_size, "could not connect to %s:%s: %s", host, port, strerror(errno));
    return -1;
  }

  return fd;
}

void smtp_disconnect(int fd) {
  if (fd >= 0) {
    close(fd);
  }
}

ssize_t smtp_socket_read(void *ctx, char *buf, size_t len) {
  int fd = *(int *)ctx;
  return recv(fd, buf, len, 0);
}

ssize_t smtp_socket_write(void *ctx, const char *buf, size_t len) {
  int fd = *(int *)ctx;
  return send(fd, buf, len, 0);
}
#ifndef LAB_H
#define LAB_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

// Three layers: (1) pure protocol helpers, no I/O; (2) the session, over a
// transport of function pointers; (3) the socket transport implementing it.

/** Maximum length of a single SMTP protocol line (RFC 5321 4.5.3.1.4). */
#define SMTP_LINE_MAX 512

/** Maximum bytes of reply text kept for error reporting. */
#define SMTP_REPLY_TEXT_MAX 256

typedef enum {
  SMTP_OK = 0,
  SMTP_ERR_IO = -1,             /**< Read/write failed or the peer closed the connection. */
  SMTP_ERR_PROTOCOL = -2,       /**< Server sent something invalid or unexpected. */
  SMTP_ERR_LINE_TOO_LONG = -3,  /**< A reply line didn't fit in SMTP_LINE_MAX bytes. */
  SMTP_ERR_INVALID_ARG = -4,    /**< Bad arguments (NULL, CR/LF injection, ...). */
} smtp_status_t;

/* --- Layer 1: pure protocol helpers (no I/O) --- */

/** Parses the 3-digit code at the start of line, or -1 if it's not one. */
int smtp_parse_reply_code(const char *line, size_t len);

/** True if line is the last line of a (possibly multi-line) reply. */
bool smtp_reply_is_final(const char *line, size_t len);

/** True if s contains a bare CR or LF (header/command injection check). */
bool smtp_has_crlf(const char *s);

/** Builds "verb arg\r\n" (or "verb\r\n" if arg is NULL). Caller frees. */
char *smtp_build_command_line(const char *verb, const char *arg);

/** Dot-stuffs body per RFC 5321 4.5.2: CRLF line endings, leading dots doubled. */
char *smtp_dot_stuff(const char *body);

/** Builds the full DATA payload: headers, blank line, stuffed body, ".\r\n". */
char *smtp_build_message(const char *from, const char *to, const char *subject, const char *body);

/* --- Layer 2: the session, over a swappable transport --- */

/** Reads up to len bytes into buf; returns bytes read, 0 on EOF, <0 on error. */
typedef ssize_t (*smtp_read_fn)(void *ctx, char *buf, size_t len);

/** Writes up to len bytes from buf; returns bytes written, <=0 on error. */
typedef ssize_t (*smtp_write_fn)(void *ctx, const char *buf, size_t len);

/** A pair of I/O callbacks plus the context they operate on. */
typedef struct {
  smtp_read_fn read_fn;
  smtp_write_fn write_fn;
  void *ctx;
} smtp_transport_t;

/** Buffered line reader built on top of a transport. */
typedef struct {
  smtp_transport_t transport;
  char buf[SMTP_LINE_MAX];
  size_t start; /**< Offset of the first unconsumed byte. */
  size_t len;   /**< Number of unconsumed bytes starting at start. */
} smtp_reader_t;

/** A parsed (possibly multi-line) SMTP reply. */
typedef struct {
  int code;
  char text[SMTP_REPLY_TEXT_MAX];
} smtp_reply_t;

/** Parameters for a full SMTP session (see smtp_run_session). */
typedef struct {
  const char *helo_host;
  const char *mail_from;
  const char *rcpt_to;
  const char *subject;
  const char *body;
} smtp_session_params_t;

/** Initializes a reader that pulls bytes from transport. */
void smtp_reader_init(smtp_reader_t *reader, smtp_transport_t transport);

/** Reads one CRLF-terminated line, refilling from the transport as needed. */
int smtp_reader_read_line(smtp_reader_t *reader, char *line_out, size_t line_out_size, size_t *out_len);

/** Reads a full reply, following continuation lines until the final one. */
int smtp_read_reply(smtp_reader_t *reader, smtp_reply_t *reply);

/** Writes all of data to the transport, looping over partial writes. */
int smtp_write_all(smtp_transport_t *transport, const char *data, size_t len);

/** Sends one command and checks the reply's code against expected_code. */
int smtp_send_command(smtp_transport_t *transport, smtp_reader_t *reader, const char *verb,
                       const char *arg, int expected_code, smtp_reply_t *reply);

/** Runs the full session: greeting, HELO, MAIL FROM, RCPT TO, DATA, body, QUIT. */
int smtp_run_session(smtp_transport_t *transport, const smtp_session_params_t *params, char *err_msg,
                      size_t err_msg_size);

/* --- Layer 3: the socket transport --- */

/** Resolves host/port and connects a TCP socket; returns fd or -1. */
int smtp_connect(const char *host, const char *port, char *err_msg, size_t err_msg_size);

/** Closes a socket returned by smtp_connect. */
void smtp_disconnect(int fd);

/** smtp_read_fn over a socket; ctx is a pointer to the fd. */
ssize_t smtp_socket_read(void *ctx, char *buf, size_t len);

/** smtp_write_fn over a socket; ctx is a pointer to the fd. */
ssize_t smtp_socket_write(void *ctx, const char *buf, size_t len);

#endif // LAB_H

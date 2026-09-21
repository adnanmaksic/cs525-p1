#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/lab.h"
#include "harness/unity.h"

void setUp(void) {}
void tearDown(void) {}

// Scripted in-memory server: read_fn hands out bytes from `script`, write_fn
// captures whatever the client sends into `sent`.
typedef struct {
  const char *script;
  size_t script_len;
  size_t script_pos;
  size_t read_chunk; /* 0 means "as much as fits" */
  int force_write_fail;
  char sent[4096];
  size_t sent_len;
} mock_server_t;

static void mock_server_init(mock_server_t *m, const char *script) {
  memset(m, 0, sizeof(*m));
  m->script = script;
  m->script_len = script != NULL ? strlen(script) : 0;
}

static ssize_t mock_read(void *ctx, char *buf, size_t len) {
  mock_server_t *m = (mock_server_t *)ctx;
  if (m->script_pos >= m->script_len) {
    return 0; /* server hung up */
  }
  size_t remaining = m->script_len - m->script_pos;
  size_t n = remaining < len ? remaining : len;
  if (m->read_chunk > 0 && n > m->read_chunk) {
    n = m->read_chunk;
  }
  memcpy(buf, m->script + m->script_pos, n);
  m->script_pos += n;
  return (ssize_t)n;
}

static ssize_t mock_write(void *ctx, const char *buf, size_t len) {
  mock_server_t *m = (mock_server_t *)ctx;
  if (m->force_write_fail) {
    return -1;
  }
  size_t space = sizeof(m->sent) - m->sent_len - 1;
  size_t n = len < space ? len : space;
  memcpy(m->sent + m->sent_len, buf, n);
  m->sent_len += n;
  m->sent[m->sent_len] = '\0';
  return (ssize_t)n;
}

static smtp_transport_t mock_transport(mock_server_t *m) {
  smtp_transport_t t = {.read_fn = mock_read, .write_fn = mock_write, .ctx = m};
  return t;
}

void test_reply_code_and_final(void) {
  TEST_ASSERT_EQUAL_INT(250, smtp_parse_reply_code("250 OK", 6));
  TEST_ASSERT_EQUAL_INT(-1, smtp_parse_reply_code("2X0 OK", 6));

  TEST_ASSERT_FALSE(smtp_reply_is_final("250-more", 8));
  TEST_ASSERT_TRUE(smtp_reply_is_final("250 OK", 6));
}

void test_has_crlf(void) {
  TEST_ASSERT_FALSE(smtp_has_crlf("plain@example.com"));
  TEST_ASSERT_TRUE(smtp_has_crlf("evil\r\ninjected"));
}

void test_build_command_line(void) {
  char *line = smtp_build_command_line("MAIL FROM:", "<a@b.com>");
  TEST_ASSERT_EQUAL_STRING("MAIL FROM: <a@b.com>\r\n", line);
  free(line);

  TEST_ASSERT_NULL(smtp_build_command_line(NULL, "x"));
}

void test_dot_stuff(void) {
  char *out = smtp_dot_stuff("Hello\n.leading dot\n");
  TEST_ASSERT_EQUAL_STRING("Hello\r\n..leading dot\r\n", out);
  free(out);
}

void test_build_message(void) {
  char *msg = smtp_build_message("a@b.com", "c@d.com", "Hi", "Body\n");
  TEST_ASSERT_EQUAL_STRING("From: a@b.com\r\nTo: c@d.com\r\nSubject: Hi\r\n\r\nBody\r\n.\r\n", msg);
  free(msg);

  TEST_ASSERT_NULL(smtp_build_message(NULL, "c@d.com", "Hi", "Body"));
}

void test_reader_read_line(void) {
  mock_server_t m;
  mock_server_init(&m, "220 hi\r\n");
  m.read_chunk = 1; /* forces the line to arrive split across several reads */
  smtp_reader_t reader;
  smtp_reader_init(&reader, mock_transport(&m));

  char line[64];
  TEST_ASSERT_EQUAL_INT(SMTP_OK, smtp_reader_read_line(&reader, line, sizeof(line), NULL));
  TEST_ASSERT_EQUAL_STRING("220 hi", line);
}

void test_read_reply(void) {
  mock_server_t m;
  mock_server_init(&m, "250-smtp.example.com\r\n250 SIZE 10240000\r\n");
  smtp_reader_t reader;
  smtp_reader_init(&reader, mock_transport(&m));

  smtp_reply_t reply;
  TEST_ASSERT_EQUAL_INT(SMTP_OK, smtp_read_reply(&reader, &reply));
  TEST_ASSERT_EQUAL_INT(250, reply.code);

  mock_server_init(&m, "XYZ broken\r\n");
  smtp_reader_init(&reader, mock_transport(&m));
  TEST_ASSERT_EQUAL_INT(SMTP_ERR_PROTOCOL, smtp_read_reply(&reader, &reply));
}

void test_write_all_and_send_command(void) {
  mock_server_t m;
  mock_server_init(&m, "250 Ok\r\n");
  smtp_transport_t t = mock_transport(&m);
  TEST_ASSERT_EQUAL_INT(SMTP_OK, smtp_write_all(&t, "hello", 5));
  TEST_ASSERT_EQUAL_STRING("hello", m.sent);

  mock_server_init(&m, "550 nope\r\n");
  t = mock_transport(&m);
  smtp_reader_t reader;
  smtp_reader_init(&reader, t);
  smtp_reply_t reply;
  TEST_ASSERT_EQUAL_INT(SMTP_ERR_PROTOCOL, smtp_send_command(&t, &reader, "HELO", "host", 250, &reply));
}

#define FULL_SESSION_SCRIPT \
  "220 smtp.example.com ESMTP ready\r\n" \
  "250 smtp.example.com\r\n" \
  "250 2.1.0 Ok\r\n" \
  "250 2.1.5 Ok\r\n" \
  "354 End data with .\r\n" \
  "250 2.0.0 Ok: queued\r\n" \
  "221 Bye\r\n"

void test_run_session(void) {
  mock_server_t m;
  mock_server_init(&m, FULL_SESSION_SCRIPT);
  smtp_transport_t t = mock_transport(&m);
  smtp_session_params_t params = {
      .helo_host = "onyx.example.edu",
      .mail_from = "me@example.com",
      .rcpt_to = "you@example.com",
      .subject = "hello",
      .body = "Hi there.\n.leading dot\nBye\n",
  };
  char err[256];

  TEST_ASSERT_EQUAL_INT(SMTP_OK, smtp_run_session(&t, &params, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(m.sent, "MAIL FROM:<me@example.com>\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(m.sent, "\r\n.\r\nQUIT\r\n"));

  mock_server_init(&m, "421 too busy\r\n");
  t = mock_transport(&m);
  TEST_ASSERT_EQUAL_INT(SMTP_ERR_PROTOCOL, smtp_run_session(&t, &params, err, sizeof(err)));

  params.mail_from = "evil\r\nRCPT TO:<x>";
  TEST_ASSERT_EQUAL_INT(SMTP_ERR_INVALID_ARG, smtp_run_session(&t, &params, err, sizeof(err)));
}

void test_socket_transport(void) {
  char err[128];
  TEST_ASSERT_EQUAL_INT(-1, smtp_connect(NULL, "25", err, sizeof(err)));

  int fds[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  TEST_ASSERT_EQUAL_INT(4, (int)smtp_socket_write(&fds[0], "ping", 4));
  char buf[16] = {0};
  TEST_ASSERT_EQUAL_INT(4, (int)smtp_socket_read(&fds[1], buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("ping", buf);

  smtp_disconnect(fds[0]);
  smtp_disconnect(fds[1]);
  smtp_disconnect(-1); /* must not crash */
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_reply_code_and_final);
  RUN_TEST(test_has_crlf);
  RUN_TEST(test_build_command_line);
  RUN_TEST(test_dot_stuff);
  RUN_TEST(test_build_message);
  RUN_TEST(test_reader_read_line);
  RUN_TEST(test_read_reply);
  RUN_TEST(test_write_all_and_send_command);
  RUN_TEST(test_run_session);
  RUN_TEST(test_socket_transport);

  return UNITY_END();
}

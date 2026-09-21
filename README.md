# Project 1 - SMTP Client

Name: Adnan Maksic
Email: adnanmaksic@u.boisestate.edu
Class: CS525-001

A command line mail client that speaks RFC 5321 SMTP directly over a TCP socket, with no mail library. It sends HELLO, MAIL FROM, RCPT TO, DATA, the message, and QUIT, checking every reply along the way.

```
Usage: myapp -f <from> -t <to> [-s subject] [-b body] [-p port] [-H helo-host] <server>
```

Exit codes: "0" success, "1" bad command line, "2" connection/session failure.

## Design

"src/lab.h"/"src/lab.c" are split into three layers:
pure protocol helpers with no I/O
the session (which reads and writes through a "smtp_transport_t" pair of function pointers instead of calling "recv"/"send" directly)
the socket transport that implements those callbacks with real sockets.
The point of the split is testability, "tests/lab-test.c" plugs a scripted in-memory
server into layer 2 so the full session and its error paths can be tested
without a network, and the real client in "main.c" runs through that same
"smtp_run_session".

## Known Bugs or Issues

None known. "make leak" / "make leak-test" are clean

## Experience

The main design decision was making the session layer swappable via
function pointers so it could be driven by a fake server in tests instead
of a real socket. Dot-stuffing was the fiddliest part to get right: doubling
a leading dot only at the start of a line, and adding a trailing CRLF before
the terminator even when the input does not end in a newline.

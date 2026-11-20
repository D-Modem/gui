#!/usr/bin/env python3
# A modem for the smoke test: it listens on 127.0.0.1:PORT like
# dmodemd's TCP port, writes each line it gets to LOG, answers AT
# commands with OK and ATD with CONNECT, and after CONNECT sends back
# the data it gets.
#
#   fake_modem.py PORT LOG
import socket
import sys
import threading

port, log_path = int(sys.argv[1]), sys.argv[2]
log = open(log_path, "a", buffering=1)


def serve(c):
    buf, online = b"", False
    while True:
        d = c.recv(1024)
        if not d:
            return
        if online:
            c.sendall(d)
            continue
        buf += d
        while b"\r" in buf:
            line, buf = buf.split(b"\r", 1)
            line = line.strip()
            if not line:
                continue
            log.write(line.decode("ascii", "replace") + "\n")
            if line.upper().startswith(b"ATD"):
                c.sendall(b"\r\nCONNECT 33600\r\n")
                online = True
            else:
                c.sendall(b"\r\nOK\r\n")


s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port))
s.listen(5)
while True:
    c, _ = s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()

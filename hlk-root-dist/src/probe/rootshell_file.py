#!/usr/bin/env python3
"""Interactive root shell over the rootcmd FILE channel (no TCP listener).

iAware kills any uid-0 TCP listener (C-30/C-34), so rootsrv's TCP shell is
short-lived. The rootd file channel (rootcmd -> rootout) has survived
continuously since 23:15, so this client drives root through it instead:

    echo <b64(cmd)> | base64 -d > /data/local/tmp/rootcmd
    ... wait for /data/local/tmp/rootcmd.done ...
    print the new tail of /data/local/tmp/rootout

Usage:
    python rootshell_file.py            # interactive, prompt "root# "
    echo "id" | python rootshell_file.py   # one-shot from stdin

No PTY: `cd` state does not persist between commands - use `cd x && ...`.
"""
import base64
import os
import subprocess
import sys
import time

SERIAL = os.environ.get("ANDROID_SERIAL", "").strip() or None
TMP = "/data/local/tmp"


def sh(cmd, timeout=90):
    argv = ["adb"]
    if SERIAL:
        argv += ["-s", SERIAL]
    argv += ["shell", cmd]
    # stdin=DEVNULL: adb must not swallow our stdin (piped commands!)
    r = subprocess.run(argv, capture_output=True, timeout=timeout,
                       stdin=subprocess.DEVNULL)
    txt = r.stdout.decode("utf-8", "replace")
    return txt.replace("\r\n", "\n").replace("\r", "\n")


def main():
    out = sh(f"wc -c < {TMP}/rootout 2>/dev/null || echo 0")
    try:
        offset = int(out.strip().splitlines()[-1])
    except ValueError:
        offset = 0
    sh(f"rm -f {TMP}/rootcmd {TMP}/rootcmd.done")

    interactive = sys.stdin.isatty()
    if interactive:
        print("HLK root shell (file channel, no TCP). 'exit' to quit.")
        print("note: no PTY - cd does not persist, use 'cd x && ...'")
    while True:
        try:
            line = input("root# " if interactive else "")
        except EOFError:
            break
        if line.strip() in ("exit", "quit"):
            break
        if not line.strip():
            continue
        b64 = base64.b64encode(line.encode()).decode()
        sh(f"rm -f {TMP}/rootcmd.done")
        sh(f"echo {b64} | base64 -d > {TMP}/rootcmd")
        done = False
        for _ in range(240):          # up to 120 s
            time.sleep(0.5)
            if sh(f"ls {TMP}/rootcmd.done 2>/dev/null").strip():
                done = True
                break
        if not done:
            print("(timeout: rootd did not acknowledge)")
            continue
        end = sh(f"wc -c < {TMP}/rootout")
        try:
            end = int(end.strip().splitlines()[-1])
        except ValueError:
            end = offset
        if end > offset:
            chunk = sh(f"tail -c +{offset + 1} {TMP}/rootout")
            sys.stdout.write(chunk)
            if interactive and not chunk.endswith("\n"):
                print()
        offset = end


if __name__ == "__main__":
    main()

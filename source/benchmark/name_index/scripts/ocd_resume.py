#!/usr/bin/env python3
"""Resume the target when OpenOCD leaves it halted outside a semihosting call.

OpenOCD occasionally stops the core at an interrupt handler entry right after
servicing a semihosting request and does not resume it. This polls the core
state over the Tcl port and resumes it after 15 s halted. It starts only once
the firmware has opened target_meta.txt in the working directory, so the halt
during flashing is left alone. Exits when OpenOCD closes. Standard library only.
"""
import datetime
import os
import socket
import time


def main():
    while not os.path.exists("target_meta.txt"):
        time.sleep(2)
    for _ in range(30):
        try:
            s = socket.create_connection(("localhost", 6666))
            break
        except OSError:
            time.sleep(1)
    else:
        return

    def cmd(c):
        s.sendall(c.encode() + b"\x1a")
        b = b""
        while not b.endswith(b"\x1a"):
            d = s.recv(4096)
            if not d:
                raise ConnectionError
            b += d
        return b[:-1].decode().strip()

    since = None
    while True:
        try:
            state = cmd("stm32h7x.cpu0 curstate")
            if state != "halted":
                since = None
            elif since is None:
                since = time.time()
            elif time.time() - since > 15:
                print(f"{datetime.datetime.now():%T} resumed after a spurious halt, {cmd('reg pc')}", flush=True)
                cmd("resume")
                since = None
        except (ConnectionError, OSError):
            return
        time.sleep(5)


if __name__ == "__main__":
    main()

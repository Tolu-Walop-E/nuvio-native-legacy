#!/usr/bin/env python3
"""Passive Android TV socket monitor (port 80/443 rows of /proc/net/tcp*).

Usage: ANDROID_SERIAL=<device> monitor_shield.py <uid> <out.csv> [seconds]
"""
import os
import socket
import struct
import subprocess
import sys
import time

if len(sys.argv) < 3:
    raise SystemExit("usage: monitor_shield.py <uid> <out.csv> [seconds]")
uid, out = sys.argv[1], sys.argv[2]
duration = int(sys.argv[3]) if len(sys.argv) > 3 else 3600
serial = os.environ.get("ANDROID_SERIAL")
if not serial:
    raise SystemExit("set ANDROID_SERIAL to the adb device (see `adb devices`)")
loop = ("while true; do t=$(date +%H:%M:%S.%N); cat /proc/net/tcp /proc/net/tcp6 | "
        "awk -v t=$t '$3 ~ /:(01BB|0050)$/ {print t, $2, $3, $4, $8}'; sleep 0.5; done")

def ipport(value):
    address, port = value.split(":")
    if len(address) == 8:
        ip = socket.inet_ntoa(struct.pack("<I", int(address, 16)))
    else:
        raw = bytes.fromhex(address)
        raw = b"".join(raw[i:i + 4][::-1] for i in range(0, 16, 4))
        ip = socket.inet_ntop(socket.AF_INET6, raw).replace("::ffff:", "")
    return ip, int(port, 16)

known, held = set(), {}
end = time.time() + duration
proc = subprocess.Popen(["adb", "-s", serial, "shell", loop], stdout=subprocess.PIPE,
                        text=True)
try:
    with open(out, "w", buffering=1, encoding="utf-8") as handle:
        handle.write("time,state,local_port,remote_ip,remote_port,uid\n")
        for line in proc.stdout:
            if time.time() > end:
                break
            parts = line.split()
            if len(parts) != 5:
                continue
            stamp, local, remote, state, row_uid = parts
            (_, local_port), (remote_ip, remote_port) = ipport(local), ipport(remote)
            row = f"{stamp[:12]},{state},{local_port},{remote_ip},{remote_port},{row_uid}\n"
            if row_uid == uid:
                if remote_ip not in known:
                    known.add(remote_ip)
                    for saved in held.pop(remote_ip, []):
                        handle.write(saved)
                handle.write(row)
            elif state == "06" and row_uid == "0":
                if remote_ip in known:
                    handle.write(row)
                else:
                    held.setdefault(remote_ip, []).append(row)
finally:
    proc.terminate()

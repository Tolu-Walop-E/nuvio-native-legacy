#!/usr/bin/env python3
import http.client
import json
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

PORT = 18765
proc = subprocess.Popen([sys.executable, "tests/rede_faltas_server.py", "--port", str(PORT)],
                        stdout=subprocess.PIPE, text=True)
try:
    ready = json.loads(proc.stdout.readline())
    assert ready["ready"]
    base = f"http://127.0.0.1:{PORT}"
    request = urllib.request.Request(base + "/file/small", headers={"Range": "bytes=10-19"})
    with urllib.request.urlopen(request) as response:
        assert response.status == 206 and response.headers["Content-Range"].startswith("bytes 10-19/")
        assert len(response.read()) == 10
    with urllib.request.urlopen(base + "/control?mode=ignore_range") as response:
        assert json.load(response)["mode"] == "ignore_range"
    connection = http.client.HTTPConnection("127.0.0.1", PORT)
    connection.request("HEAD", "/file/small", headers={"Range": "bytes=10-19"})
    response = connection.getresponse()
    assert response.status == 200
    with urllib.request.urlopen(base + "/control?mode=503&retry_after=2"): pass
    try:
        urllib.request.urlopen(base + "/file/small")
        raise AssertionError("503 expected")
    except urllib.error.HTTPError as error:
        assert error.code == 503 and error.headers["Retry-After"] == "2"
    with urllib.request.urlopen(base + "/control?mode=429&retry_after=3"): pass
    try:
        urllib.request.urlopen(base + "/file/small")
        raise AssertionError("429 expected")
    except urllib.error.HTTPError as error:
        assert error.code == 429 and error.headers["Retry-After"] == "3"
    with urllib.request.urlopen(base + "/control?mode=normal&expire_after=0.001"): pass
    time.sleep(0.01)
    try:
        urllib.request.urlopen(base + "/file/small")
        raise AssertionError("expired URL should return 403")
    except urllib.error.HTTPError as error:
        assert error.code == 403
    with urllib.request.urlopen(base + "/control?mode=normal&expire_after=0&close_after=65536"): pass
    short = urllib.request.Request(base + "/file/small", headers={"Range": "bytes=0-1048575"})
    try:
        with urllib.request.urlopen(short) as response: response.read()
        raise AssertionError("mid-read close should truncate the response")
    except (http.client.IncompleteRead, ConnectionError):
        pass
    with urllib.request.urlopen(base + "/control?close_after=0&stall_after=65536&stall_ms=20"): pass
    stalled = urllib.request.Request(base + "/file/small", headers={"Range": "bytes=0-1048575"})
    before = time.monotonic()
    try:
        with urllib.request.urlopen(stalled) as response: response.read()
    except http.client.IncompleteRead:
        pass
    assert time.monotonic() - before >= 0.02
    target = urllib.parse.quote(base + "/file/small", safe="")
    with urllib.request.urlopen(base + f"/control?stall_after=0&redirect_target={target}"): pass
    with urllib.request.urlopen(base + "/redirect-target") as response:
        assert response.status == 200
    with urllib.request.urlopen(base + "/control?mode=normal"): pass
    with urllib.request.urlopen(base + "/r/3/file/small") as response:
        assert response.status == 200
    with urllib.request.urlopen(base + "/accounting") as response:
        assert json.load(response)["connections"] >= 1
    print("rede_faltas: ok")
finally:
    proc.terminate()
    try: proc.wait(5)
    except subprocess.TimeoutExpired: proc.kill()

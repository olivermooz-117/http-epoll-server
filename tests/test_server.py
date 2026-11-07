#!/usr/bin/env python3

import os
import signal
import socket
import subprocess
import sys
import threading
import time

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
WWW_ROOT = os.path.join(REPO_ROOT, "www")


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_server(port, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def start_server(binary_path):
    port = find_free_port()
    proc = subprocess.Popen(
        [binary_path, "--port", str(port), "--quiet", "--root", WWW_ROOT],
        cwd=REPO_ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if not wait_for_server(port):
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        return None, port
    return proc, port


def stop_server(proc):
    if proc is None:
        return
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def raw_request(port, payload, timeout=5):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.settimeout(timeout)
        s.sendall(payload)
        data = b""
        while True:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            data += chunk
            if b"\r\n\r\n" in data:
                headers_text, _, body = data.partition(b"\r\n\r\n")
                headers = headers_text.decode("latin-1").split("\r\n")
                content_length = None
                for line in headers[1:]:
                    if line.lower().startswith("content-length:"):
                        content_length = int(line.split(":", 1)[1].strip())
                        break
                if content_length is None:
                    # If the server closes the connection, keep reading until EOF.
                    if "Connection: close" in headers_text.decode("latin-1").lower():
                        continue
                    break
                if len(body) >= content_length:
                    break
                while len(body) < content_length:
                    try:
                        chunk = s.recv(65536)
                    except socket.timeout:
                        break
                    if not chunk:
                        break
                    body += chunk
                    if len(body) >= content_length:
                        break
                data = headers_text + b"\r\n\r\n" + body
                break
    return data


def parse_response(data):
    if not data:
        return 0, {}, b""
    headers_text, sep, body = data.partition(b"\r\n\r\n")
    if not sep:
        return 0, {}, b""
    status_line = headers_text.decode("latin-1").splitlines()[0]
    status = int(status_line.split()[1])
    headers = {}
    for line in headers_text.decode("latin-1").splitlines()[1:]:
        if not line:
            continue
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()
    return status, headers, body


def send_http(port, method, path, headers=None, body=b"", close_after=True):
    request_headers = headers or {}
    if "Host" not in request_headers:
        request_headers["Host"] = "localhost"
    if close_after and "Connection" not in request_headers:
        request_headers["Connection"] = "close"
    lines = [f"{method} {path} HTTP/1.1"]
    for key, value in request_headers.items():
        lines.append(f"{key}: {value}")
    lines.append("")
    payload = ("\r\n".join(lines) + "\r\n").encode("latin-1") + body
    return raw_request(port, payload)


def check(name, func):
    try:
        ok = func()
    except Exception as exc:  # pragma: no cover
        print(f"FAIL: {name} ({exc})")
        return False
    print(f"{'PASS' if ok else 'FAIL'}: {name}")
    return ok


def test_basic_gets():
    proc, port = start_server(sys.argv[1] if len(sys.argv) > 1 else "./server_asan")
    if proc is None:
        return False
    try:
        for path in ("/", "/about.html", "/test.txt"):
            raw = send_http(port, "GET", path)
            status, headers, body = parse_response(raw)
            if status != 200:
                return False
            if headers.get("content-type") not in (
                "text/html; charset=utf-8",
                "text/plain; charset=utf-8",
            ):
                return False
            if headers.get("content-length") != str(len(body)):
                return False
        return True
    finally:
        stop_server(proc)


def test_missing_file():
    proc, port = start_server(sys.argv[1] if len(sys.argv) > 1 else "./server_asan")
    if proc is None:
        return False
    try:
        raw = send_http(port, "GET", "/missing.txt")
        status, _, body = parse_response(raw)
        return status == 404 and b"Not Found" in body
    finally:
        stop_server(proc)


def test_head_request():
    proc, port = start_server(sys.argv[1] if len(sys.argv) > 1 else "./server_asan")
    if proc is None:
        return False
    try:
        get_raw = send_http(port, "GET", "/")
        get_status, get_headers, get_body = parse_response(get_raw)
        head_raw = send_http(port, "HEAD", "/")
        head_status, head_headers, head_body = parse_response(head_raw)
        return (
            get_status == 200 and head_status == 200 and
            head_headers.get("content-length") == str(len(get_body)) and
            head_body == b""
        )
    finally:
        stop_server(proc)


def test_post_method():
    proc, port = start_server(sys.argv[1] if len(sys.argv) > 1 else "./server_asan")
    if proc is None:
        return False
    try:
        raw = send_http(port, "POST", "/")
        status, headers, body = parse_response(raw)
        return status == 405 and headers.get("allow") == "GET, HEAD" and b"Method Not Allowed" in body
    finally:
        stop_server(proc)


def test_http_validation():
    proc, port = start_server(sys.argv[1] if len(sys.argv) > 1 else "./server_asan")
    if proc is None:
        return False
    try:
        raw = raw_request(port, b"GET / HTTP/1.1\r\nConnection: close\r\n\r\n")
        status_no_host, _, _ = parse_response(raw)

        raw_empty_host = raw_request(
            port,
            b"GET / HTTP/1.1\r\nHost:   \r\nConnection: close\r\n\r\n"
        )
        status_empty_host, _, _ = parse_response(raw_empty_host)

        raw2 = raw_request(port, b"GET / HTTP/2.0\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        status_http2, _, _ = parse_response(raw2)
        raw3 = raw_request(port, b"GET / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
        status_chunked, _, _ = parse_response(raw3)
        big_header = "X-Test: " + ("A" * (20 * 1024))
        raw4 = raw_request(port, ("GET / HTTP/1.1\r\nHost: localhost\r\n" + big_header + "\r\nConnection: close\r\n\r\n").encode("latin-1"))
        status_big_header, _, _ = parse_response(raw4)
        long_target = "/" + ("A" * 8193)
        raw5 = raw_request(port, f"GET {long_target} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n".encode("latin-1"))
        status_long_target, _, _ = parse_response(raw5)
        raw6 = raw_request(port, b"garbage\r\n\r\n")
        status_garbage, _, _ = parse_response(raw6)
        return (
            status_no_host == 400 and
            status_empty_host == 400 and
            status_http2 == 505 and
            status_chunked == 501 and
            status_big_header == 431 and
            status_long_target == 414 and
            status_garbage == 400
        )
    finally:
        stop_server(proc)


def test_path_traversal():
    proc, port = start_server(sys.argv[1] if len(sys.argv) > 1 else "./server_asan")
    if proc is None:
        return False
    paths = [
        "/../main.cpp",
        "/%2e%2e/main.cpp",
        "/..%2fmain.cpp",
        "/..%5cmain.cpp",
        "/index.html%00.png",
        "//etc/passwd",
    ]
    try:
        for path in paths:
            raw = send_http(port, "GET", path)
            status, _, body = parse_response(raw)
            if status not in (400, 404):
                return False
            if b"#include" in body:
                return False
        return True
    finally:
        stop_server(proc)


def test_split_request_and_pipeline():
    proc, port = start_server(sys.argv[1] if len(sys.argv) > 1 else "./server_asan")
    if proc is None:
        return False
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.settimeout(5)
        s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n")
        time.sleep(0.2)
        s.sendall(b"\r\n")
        first = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            first += chunk
            if b"\r\n\r\n" in first:
                break
        status1, _, _ = parse_response(first)
        s.close()
        if status1 != 200:
            return False

        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.settimeout(5)
        payload = (
            b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
            b"GET /about.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
        )
        s.sendall(payload)
        first_resp = s.recv(65536)
        second_resp = s.recv(65536)
        status1, _, _ = parse_response(first_resp)
        status2, _, _ = parse_response(second_resp)
        s.close()
        return status1 == 200 and status2 == 200
    finally:
        stop_server(proc)


def test_keep_alive_and_duplicate_headers():
    proc, port = start_server(sys.argv[1] if len(sys.argv) > 1 else "./server_asan")
    if proc is None:
        return False
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.settimeout(5)
        for _ in range(5):
            s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n")
            raw = b""
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                raw += chunk
                if b"\r\n\r\n" in raw:
                    break
            status, _, _ = parse_response(raw)
            if status != 200:
                return False
        s.close()

        raw_dup = raw_request(port, b"GET / HTTP/1.1\r\nHost: localhost\r\nAccept: one\r\nAccept: two\r\nConnection: close\r\n\r\n")
        status_dup, _, _ = parse_response(raw_dup)
        if status_dup != 200:
            return False

        raw_host_dup = raw_request(port, b"GET / HTTP/1.1\r\nHost: localhost\r\nHost: example.com\r\nConnection: close\r\n\r\n")
        status_host_dup, _, _ = parse_response(raw_host_dup)
        return status_host_dup == 400
    finally:
        stop_server(proc)


def test_v052_security_regressions():
    binary_path = sys.argv[1] if len(sys.argv) > 1 else "./server_asan"
    proc, port = start_server(binary_path)

    if proc is None:
        return False

    link_path = os.path.join(
        WWW_ROOT,
        f".v052-outside-link-{os.getpid()}"
    )

    try:
        # Verify that a symlink cannot expose files outside WWW_ROOT.
        if os.path.lexists(link_path):
            return False

        os.symlink("/etc/passwd", link_path)

        symlink_request = (
            f"GET /{os.path.basename(link_path)} HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n\r\n"
        ).encode("ascii")

        symlink_response = raw_request(port, symlink_request)
        symlink_status, _, symlink_body = parse_response(symlink_response)

        if symlink_status != 400 or b"root:" in symlink_body:
            print("Symlink regression failed:", symlink_status)
            return False

        # Verify that oversized input receives HTTP 413.
        oversized_payload = b"A" * (1024 * 1024 + 1)
        oversized_response = b""

        with socket.create_connection(
            ("127.0.0.1", port), timeout=10
        ) as s:
            s.settimeout(10)

            try:
                s.sendall(oversized_payload)
            except (BrokenPipeError, ConnectionResetError):
                # The server may close while sendall is still transmitting.
                pass

            while True:
                try:
                    chunk = s.recv(65536)
                except socket.timeout:
                    break
                except ConnectionResetError:
                    break

                if not chunk:
                    break

                oversized_response += chunk

        oversized_status, headers, body = parse_response(
            oversized_response
        )

        if oversized_status != 413:
            print("Oversized request regression failed:", oversized_status)
            return False

        return (
            headers.get("connection") == "close"
            and body == b"Content Too Large\n"
        )

    finally:
        if os.path.lexists(link_path):
            os.unlink(link_path)

        stop_server(proc)


def get_cpu_ticks(pid):
    with open(f"/proc/{pid}/stat", "r", encoding="utf-8") as handle:
        data = handle.read().split()
    return int(data[13]) + int(data[14])


def test_cpu_regression():
    binary_path = sys.argv[1] if len(sys.argv) > 1 else "./server_asan"
    cases = [
        (
            "keep-alive disconnect",
            lambda port: [
                threading.Thread(target=lambda: (
                    s := socket.create_connection(("127.0.0.1", port), timeout=5),
                    s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"),
                    s.recv(4096),
                    s.close(),
                )) for _ in range(20)
            ],
        ),
        (
            "connect then close no data",
            lambda port: [
                threading.Thread(target=lambda: (
                    s := socket.create_connection(("127.0.0.1", port), timeout=5),
                    s.close(),
                )) for _ in range(20)
            ],
        ),
        (
            "partial header then shutdown",
            lambda port: [
                threading.Thread(target=lambda: (
                    s := socket.create_connection(("127.0.0.1", port), timeout=5),
                    s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n"),
                    s.shutdown(socket.SHUT_WR),
                    s.close(),
                )) for _ in range(20)
            ],
        ),
    ]
    for name, factory in cases:
        proc, port = start_server(binary_path)
        if proc is None:
            return False
        try:
            start_ticks = get_cpu_ticks(proc.pid)
            threads = factory(port)
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            time.sleep(2)
            end_ticks = get_cpu_ticks(proc.pid)
            delta = end_ticks - start_ticks
            limit = int(os.sysconf("SC_CLK_TCK") * 0.2)
            if delta >= limit:
                print(f"FAIL: {name} (delta={delta} limit={limit})")
                return False
        finally:
            stop_server(proc)
    return True


def test_graceful_shutdown():
    proc, port = start_server(sys.argv[1] if len(sys.argv) > 1 else "./server_asan")
    if proc is None:
        return False
    try:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            return False
        return proc.returncode == 0
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=3)


if __name__ == "__main__":
    binary = sys.argv[1] if len(sys.argv) > 1 else "./server_asan"
    tests = [
        ("GET pages", test_basic_gets),
        ("missing file", test_missing_file),
        ("HEAD request", test_head_request),
        ("POST method", test_post_method),
        ("HTTP validation", test_http_validation),
        ("path traversal", test_path_traversal),
        ("split request + pipeline", test_split_request_and_pipeline),
        ("keep-alive + duplicate headers", test_keep_alive_and_duplicate_headers),
        ("v0.5.2 security regressions", test_v052_security_regressions),
        ("CPU regression", test_cpu_regression),
        ("graceful shutdown", test_graceful_shutdown),
    ]
    ok = True
    for name, fn in tests:
        ok = check(name, fn) and ok
    sys.exit(0 if ok else 1)

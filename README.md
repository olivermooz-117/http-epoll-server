# HTTP Epoll Server

A lightweight HTTP/1.1 web server written in C++17 using Linux non-blocking sockets and the `epoll` I/O event notification mechanism.

The project explores low-level network programming, HTTP request handling, connection lifecycle management, and performance benchmarking against Nginx.

## Project Overview

This server is designed to demonstrate how an event-driven HTTP server can handle multiple client connections using Linux system calls and non-blocking I/O.

Unlike a traditional thread-per-connection architecture, the server uses a single event loop with `epoll` to monitor multiple client sockets.

### Current Version: v0.5.1 — HTTP correctness and CLI support

The project currently includes:

- Non-blocking TCP sockets.
- Linux `epoll` event monitoring.
- HTTP/1.1 request parsing.
- GET and HEAD methods.
- Static file serving.
- Persistent HTTP connections.
- Connection idle timeout management.
- Maximum requests per connection.
- Partial socket read and write handling.
- HTTP error responses.
- Performance benchmarking against Nginx.
- CPU and memory usage measurements.

## Features

### Network and I/O

- Uses Linux TCP sockets.
- Sets client sockets to non-blocking mode.
- Uses `epoll` to monitor multiple connections.
- Implements an event-driven server architecture.
- Handles partial reads and writes.
- Supports persistent connections using HTTP keep-alive.

### HTTP Protocol

Supports the following HTTP methods:

| Method | Support |
|---|---|
| GET | Yes |
| HEAD | Yes |
| POST | Not implemented |

Supported HTTP response status codes include:

| Status | Description |
|---|---|
| 200 | OK |
| 400 | Bad Request |
| 404 | Not Found |
| 405 | Method Not Allowed |
| 413 | Content Too Large (request body) |
| 414 | URI Too Long |
| 431 | Request Header Fields Too Large |
| 500 | Internal Server Error (including oversized static files) |
| 501 | Not Implemented |
| 505 | HTTP Version Not Supported |

### Static File Serving

The server serves static files from the `www/` directory.

It includes:

- HTML file serving.
- Plain-text file serving.
- MIME type detection.
- URL percent decoding.
- Basic path traversal protection.
- HTTP content-length headers.

### Connection Lifecycle Management (v0.4)

Version 0.4 introduced connection lifecycle management.

Features include:

- 30-second idle connection timeout.
- Maximum of 100 requests per connection.
- Per-connection request counting.
- Connection activity tracking.
- Periodic cleanup using the `epoll_wait()` timeout.
- Automatic connection closure when limits are reached.

These features help prevent indefinitely idle connections and limit the lifetime of persistent connections.

## Technologies Used

- **Language:** C++17
- **Operating System:** Linux
- **Networking:** TCP sockets
- **I/O Multiplexing:** Linux epoll
- **Build:** g++
- **Benchmarking:** wrk
- **Comparison Server:** Nginx
- **System Monitoring:** sysstat / pidstat

## Project Structure

```text
http-epoll-server/
├── main.cpp
├── www/
│   ├── index.html
│   ├── about.html
│   └── test.txt
├── .gitignore
└── README.md
```

## Requirements

- Linux operating system.
- g++ compiler with C++17 support.
- Nginx (for the comparison benchmark).
- wrk (for HTTP load testing).
- sysstat (for CPU and memory monitoring).

Install the required tools on Ubuntu:

```bash
sudo apt update
sudo apt install -y g++ nginx wrk sysstat
```

## Building the Server

Clone the repository:

```bash
git clone https://github.com/olivermooz-117/http-epoll-server.git

cd http-epoll-server
```

Compile:

```bash
g++ -std=c++17 -Wall -Wextra -O2 main.cpp -o server
```

## Running the Server

Start the server:

```bash
./server
```

The C++ server listens on port `8080` by default. Custom ports and roots can be set with CLI options:

```bash
./server --port 8080 --root www --quiet
```

Open another terminal and test it:

```bash
curl -i http://127.0.0.1:8080/
```

Test the other endpoints:

```bash
curl -i http://127.0.0.1:8080/about.html

curl -i http://127.0.0.1:8080/test.txt

curl -i http://127.0.0.1:8080/missing.html
```

Test HEAD:

```bash
curl -I http://127.0.0.1:8080/
```

## Performance Benchmarking (v0.5)

Version 0.5 introduces performance benchmarking to evaluate the C++ epoll server against Nginx.

The comparison measures:

- Requests per second.
- Average latency.
- 99th-percentile latency.
- CPU consumption.
- Memory usage.

### Benchmark Environment

The tests were performed locally on the same Linux machine.

| Component | Configuration |
|---|---|
| C++ server | HTTP epoll server |
| Comparison server | Nginx 1.28.3 |
| C++ server port | 8080 |
| Nginx port | 8081 |
| Benchmark tool | wrk |
| Test page | Static HTML |
| HTML content length | 811 bytes |
| Network | Localhost / loopback |

Both servers served the same static HTML page.

The benchmarks were run sequentially against each server using the same machine.

### Nginx Configuration

Nginx was configured to listen on port `8081` and serve the same static files.

Configuration:

```nginx
server {
    listen 8081;
    server_name localhost;

    root /var/www/epoll-benchmark;
    index index.html;

    location / {
        try_files $uri $uri/ =404;
    }
}
```

The document root was populated with the project's `www/` directory.

Validate the Nginx configuration:

```bash
sudo nginx -t
```

Reload Nginx:

```bash
sudo systemctl reload nginx
```

Verify both servers:

```bash
curl -i http://127.0.0.1:8080/

curl -i http://127.0.0.1:8081/
```

Both endpoints returned HTTP 200 OK with matching HTML content.

### Benchmark Commands

The `wrk` tool was used with latency reporting enabled.

Example C++ server benchmark:

```bash
wrk -t2 -c10 -d30s --latency http://127.0.0.1:8080/
```

Example Nginx benchmark:

```bash
wrk -t2 -c10 -d30s --latency http://127.0.0.1:8081/
```

The thread and connection settings were increased for the higher-concurrency tests.

### Throughput and Latency Results

The following results were collected during the benchmark tests.

| Connections | Threads | C++ Requests/sec | C++ Avg Latency | C++ P99 Latency | Nginx Requests/sec | Nginx Avg Latency | Nginx P99 Latency |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 10 | 2 | 15,740.49 | 635.07 µs | 1.22 ms | 50,111.51 | 200.59 µs | 712 µs |
| 50 | 2 | 18,316.44 | 2.72 ms | 4.82 ms | 59,844.53 | 772.47 µs | 3.81 ms |
| 100 | 4 | 17,307.99 | 5.76 ms | 9.79 ms | 72,330.44 | 1.43 ms | 5.61 ms |
| 200 | 4 | 18,855.40 | 10.59 ms | 17.46 ms | 64,496.16 | 2.90 ms | 9.02 ms |

### Throughput Comparison

Nginx achieved higher throughput in every tested configuration.

| Connections | Nginx Throughput Advantage |
|---:|---:|
| 10 | 3.18x |
| 50 | 3.27x |
| 100 | 4.18x |
| 200 | 3.42x |

The C++ server maintained throughput in the approximate range of 17,000–19,000 requests per second.

Nginx achieved higher throughput and handled increasing concurrency more effectively.

### Benchmark Analysis

The results show that the C++ epoll server can handle a substantial number of HTTP requests using a lightweight event-driven architecture.

However, throughput remained relatively stable as the number of concurrent connections increased.

Nginx achieved significantly higher throughput, particularly at 100 concurrent connections.

The C++ server also experienced increasing average latency as concurrency increased.

This indicates that the current single-event-loop architecture has a performance limitation under higher load.

## CPU Usage Analysis

CPU utilization was monitored using `pidstat`.

Example monitoring command:

```bash
pidstat -u -r -p <PID> 1 35
```

The monitoring was performed while running a 30-second benchmark with 100 concurrent connections.

### Observed CPU Usage

| Metric | C++ Epoll Server | Nginx |
|---|---:|---:|
| Process architecture | Single process | Master + 8 workers |
| CPU usage under sustained load | Approximately 98–101% | Approximately 394% combined worker CPU |
| CPU cores utilized | Approximately 1 core | Approximately 4 cores |

The C++ server reached approximately 100% CPU utilization during sustained load.

Nginx distributed processing across its eight worker processes, with combined worker CPU usage reaching approximately 394%.

### CPU Analysis

The results suggest that the C++ server's single event loop is a significant performance bottleneck.

Although `epoll` allows one process to monitor many connections efficiently, it does not automatically distribute request processing across multiple CPU cores.

Nginx uses multiple worker processes, allowing it to take advantage of additional CPU cores.

This provides a likely explanation for much of the throughput difference observed during testing.

## Memory Usage Analysis

Memory usage was evaluated using Resident Set Size (RSS) and Proportional Set Size (PSS).

PSS is particularly useful when comparing multi-process applications because it apportions shared memory pages across processes.

### RSS Observations

| Metric | C++ Epoll Server | Nginx |
|---|---:|---:|
| C++ server RSS | Approximately 4,420 KB | — |
| Nginx worker RSS | — | Approximately 5,800–6,000 KB per worker |
| Nginx master RSS | — | Approximately 9,780 KB |

The combined Nginx RSS is not a fair measure of unique memory consumption because shared memory pages can be counted in multiple processes.

### PSS Measurements

PSS was measured using `/proc/<PID>/smaps_rollup`.

C++ server:

```bash
SERVER_PID=$(pgrep -n -x server)

sudo awk '/^Pss:/ {print $2 " KB"}' \
    /proc/$SERVER_PID/smaps_rollup
```

Nginx:

```bash
for pid in $(pgrep -x nginx); do
    sudo awk '/^Pss:/ {print $2 " KB"}' \
        /proc/$pid/smaps_rollup
done
```

### Measured PSS Results

| Process | PSS |
|---|---:|
| C++ epoll server | 912 KB |
| Nginx master | 1,259 KB |
| Nginx worker 1 | 958 KB |
| Nginx worker 2 | 944 KB |
| Nginx worker 3 | 944 KB |
| Nginx worker 4 | 954 KB |
| Nginx worker 5 | 958 KB |
| Nginx worker 6 | 947 KB |
| Nginx worker 7 | 947 KB |
| Nginx worker 8 | 947 KB |
| **Total Nginx PSS** | **8,858 KB** |

### Memory Analysis

The measured C++ server PSS was approximately 912 KB, compared with 8,858 KB for the Nginx master and workers combined.

This is approximately 9.7 times more PSS for the Nginx process group.

The C++ server therefore demonstrates a much smaller measured process memory footprint in this test.

These PSS values are snapshots, not peak measurements or measurements confirmed during active load. They should not be interpreted as total system memory consumption.

## Overall Performance Analysis

The benchmark results demonstrate a trade-off between simplicity, memory footprint, and throughput.

| Area | C++ Epoll Server | Nginx |
|---|---|---|
| Architecture | Single event loop | Multi-worker |
| Throughput | Lower | Higher |
| Latency | Higher under concurrency | Lower in these tests |
| CPU utilization | Approximately one core | Multiple cores |
| Measured total PSS | 912 KB | 8,858 KB |
| Implementation complexity | Custom implementation | Mature production server |

### Key Findings

1. The C++ server successfully handles concurrent HTTP connections using Linux epoll.
2. The server maintains throughput of approximately 17,000–19,000 requests per second in the tested configurations.
3. Nginx achieved approximately 3.18x–4.18x the C++ server's throughput.
4. The C++ server reached approximately one fully utilized CPU core under sustained load.
5. Nginx distributed processing across multiple worker processes.
6. The C++ server demonstrated a substantially smaller measured PSS footprint.

The results suggest that the C++ server's single-event-loop architecture is a major limitation to scaling throughput across CPU cores.

Nginx is a mature, highly optimized web server, so these results should be understood as a comparison between a learning project and a production-grade implementation, not as a claim that the two implementations have equivalent optimization or feature coverage.

## Benchmark Limitations

The benchmark results should be interpreted within the following limitations:

- Tests were performed on localhost using loopback networking.
- The benchmark client and servers shared the same physical machine.
- `wrk` consumes CPU resources and may influence the results.
- The tests focused on serving a small static HTML file.
- The tests do not represent all real-world workloads.
- CPU and memory usage can vary depending on system load and background processes.
- PSS measurements represent snapshots rather than peak memory usage.

The results are useful for comparing the current implementations under the same local test conditions, but should not be treated as universal performance figures.

## Future Improvements

Potential future development work includes:

- Multi-worker server architecture.
- CPU-core-aware event loops.
- Improved request parsing and buffering.
- More efficient memory management.
- Additional HTTP methods.
- Improved error handling and logging.
- Stress testing with larger static files.
- Testing with more diverse HTTP workloads.
- Repeated benchmark runs and statistical comparison.
- Profiling with tools such as `perf`.

A multi-worker architecture is a particularly interesting next step because it could allow the server to distribute connections across multiple CPU cores.

## Development Roadmap

| Version | Focus | Status |
|---|---|---|
| v0.3 | Core HTTP server and epoll implementation | Completed |
| v0.4 | Connection lifecycle management | Completed |
| v0.5 | Performance benchmarking against Nginx | Benchmarking completed |
| v0.6 | Multi-worker architecture and performance optimization | Planned |

## Author

**Oliver Moosberger**

GitHub: https://github.com/olivermooz-117

Repository: https://github.com/olivermooz-117/http-epoll-server

---

*This project is an exploration of Linux systems programming, network programming, event-driven I/O, HTTP server architecture, and performance engineering.*
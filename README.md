# High-Performance Multiplexed HTTP Server

A lightweight, high-performance HTTP/1.1 web server written entirely in C. 

This project was built to explore advanced Linux systems programming and high-concurrency network architectures. It utilizes `epoll` for event-driven multiplexing, zero-copy data transfers via `sendfile`, and the Linux Asynchronous I/O (`libaio`) subsystem for non-blocking dynamic file reads.

## Key Features

* **Event-Driven Architecture:** Utilizes an `epoll`-based event loop to handle thousands of concurrent connections efficiently without the overhead of thread-per-connection models.
* **Zero-Copy Static Routing:** Serves static assets using the `sendfile()` system call, allowing data to be transferred directly from the disk to the network socket entirely within kernel space.
* **Asynchronous I/O (AIO):** Implements `libaio` with `eventfd` to handle dynamic file reading asynchronously, ensuring the main event loop is never blocked by disk I/O.
* **Persistent Connections:** Fully supports HTTP/1.1 `Connection: keep-alive` to bypass TCP handshake overhead and Nagle's algorithm for consecutive requests.
* **Memory Safe & Graceful Teardown:** Implements `SIGINT` signal handling to safely drain active file descriptors and free memory. Verified strictly with Valgrind (`0 bytes in 0 blocks lost`).

## Performance Benchmarks

The server is highly optimized for throughput and connection reuse. Below are the results of a 10-second load test using `wrk` on a local machine, requesting a large static HTML file.

**Command:**
`wrk -t4 -c100 -d10s http://127.0.0.1:8888/static/file.html`

**Results:**
```text
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    35.09ms  131.72ms   1.84s    98.00%
    Req/Sec   601.85    452.69     2.75k    55.14%
  22219 requests in 10.02s, 5.64GB read
  Socket errors: connect 0, read 0, write 0, timeout 12
Requests/sec:   2217.91
Transfer/sec:    576.59MB
```

Currently bottlenecked by the fact that the server does not keep any files in a cache, thus leading to more costly I/O syscalls.

## Running with Docker

The easiest way to run this server is via the pre-built Docker container hosten on GitHub Container Registry.

```bash
docker run -p 8888:8888 ghcr.io/vjio/so_async_server:latest
```

## Building from source

Will only work on a Linux environment!

### Prerequisites
GCC/Clang and libaio-dev

### Compilation
Simply run 'make' and then execute ./aws

## Testing

The docker image comes with a file you can curl for testing

```bash
curl http://127.0.0.1:8888/static/index.html
```

You can change the dockerfile to add heftier files to test with. Or compile the server on your own machine and curl some of you local files.

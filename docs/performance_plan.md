# Performance Enhancement Plan

## Overview
A review of the UDP ingest and buffering path highlighted several hot spots that can be tuned to reduce CPU load and latency. The items below are ordered by expected impact.

## 1. Reuse RTP packet buffers instead of allocating per datagram
*Finding.* The UDP worker allocates and maps a fresh `GstBuffer` for every packet before issuing `recvfrom`, which forces repeated heap allocations and memory mappings in the hottest loop of the program.【F:src/udp_receiver.c†L335-L377】

*Plan.*
1. Introduce a small fixed-size buffer pool sized for the maximum RTP datagram (4 KiB today) using `GstBufferPool` or a project-local free list of `GstBuffer` objects.
2. Warm the pool during receiver start-up and hand buffers to the socket thread from the pool rather than calling `gst_buffer_new_allocate`.
3. After `gst_app_src_push_buffer`, recycle buffers back into the pool via a custom `GDestroyNotify` or the pool's release callback so they can be reused without hitting the allocator.
4. Expose pool size and buffer size through configuration if different transmitters require larger MTUs.

## 2. Remove the per-packet global mutex from the receive hot path
*Finding.* The receiver thread acquires the same `GMutex` both to check the stop flag and to update statistics for every packet, even though the thread is the sole writer of the stats struct. This adds uncontended but still costly kernel transitions around every datagram.【F:src/udp_receiver.c†L327-L391】【F:src/udp_receiver.c†L372-L376】

*Plan.*
1. Replace the stop flag with an atomic boolean (e.g., `g_atomic_int`) so the loop can check for shutdown without locking.
2. Guard statistics with a reader/writer strategy: keep the writer lock-free on the UDP thread and let readers take a snapshot via `g_rw_lock_reader_lock`/`_unlock`, or publish stats through atomics and sequence counters.
3. Restrict the existing mutex to lifecycle transitions (`start`, `stop`, `destroy`) where multi-threaded coordination is actually required.
4. Re-run profiling on the receiver thread to confirm the removal of lock/unlock pairs decreases CPU usage and packet processing jitter.

## 3. Batch UDP reads with `recvmmsg`
*Finding.* Each iteration issues a single `recvfrom` call and immediately hands the payload to GStreamer, even though multiple RTP packets often arrive back-to-back. That pattern wastes system call overhead and increases context switches under high bitrate workloads.【F:src/udp_receiver.c†L350-L385】

*Plan.*
1. Switch the socket to non-blocking mode and replace `recvfrom` with `recvmmsg`, requesting up to N datagrams per call (tune N experimentally, e.g., 4–8).
2. Use the buffer pool from item #1 to supply the `mmsghdr` array with pre-sized payloads before each batch receive.
3. After each successful `recvmmsg`, push each filled buffer into the appsrc in order, updating stats once per packet but amortizing the syscall overhead across the batch.
4. Preserve the existing timeout/shutdown behaviour by combining `recvmmsg` with `ppoll` or by using the `MSG_WAITFORONE` flag to ensure timely responsiveness when the socket is idle.

Implementing these three enhancements will shrink allocator churn, eliminate unnecessary locking, and reduce system call pressure, yielding lower CPU usage and tighter latency across the video pipeline.

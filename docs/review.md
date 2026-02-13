# Code Review Findings

## 1. Repeated stats toggles in the main loop
* `main.c` invokes `pipeline_set_receiver_stats_enabled` with the same computed value multiple times in a row during startup, hotplug handling, and OSD toggles. Each call takes a lock inside the pipeline, so the repeated invocations provide no new information but still pay the synchronization cost.【F:src/main.c†L213-L348】
* **Suggestion:** Cache the previously applied flag and only call into the pipeline when the desired enabled state actually changes.
* **Fix:** The main loop now caches the last applied stats state so redundant toggles become no-ops.

## 2. SSE streamer shutdown flag races
* `sse_streamer.c` reads and writes the `shutdown` flag from different threads without any synchronization. The accept loop and client threads spin on `while (!streamer->shutdown)` while `sse_streamer_stop` writes to the same field without holding a lock or using atomics, which is undefined behaviour in C and can manifest as hung threads on shutdown.【F:src/sse_streamer.c†L134-L327】
* **Suggestion:** Promote `shutdown` to an atomic (e.g., `gatomic`) or guard it with the existing mutex so that background threads see the update promptly.
* **Fix:** The shutdown flag is now implemented with GLib atomics, ensuring worker threads observe stop requests safely.

## 3. Redundant property programming in the GStreamer pipeline
* The UDP pipeline setup configures appsrc/appsink limits twice: once via `g_object_set` and again through the specialized helper APIs. This duplication appears in both the video appsink (`max-buffers`, `drop`) and the audio appsrc (`max-bytes`). The double writes add maintenance burden and make it unclear which path is authoritative.【F:src/pipeline.c†L517-L606】
* **Suggestion:** Pick one configuration style per element (either property setter or helper) to keep the pipeline definition declarative and easier to audit.
* **Fix:** The pipeline code now configures the appsink/appsrc limits through the helper APIs only once per element.

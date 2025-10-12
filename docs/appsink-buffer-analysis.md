# Appsink Buffer Configuration Investigation

The pipeline now looks up `[pipeline].appsink-max-buffers` when it builds either
receiver graph. During construction, the code resolves the configured value,
defaults to 4 when the setting is absent or non-positive, and programs the
appsink via `gst_app_sink_set_max_buffers`. It still enables frame dropping to
avoid blocking upstream producers. 【F:src/pipeline.c†L514-L520】

The on-demand UDP source pipeline built through `gst_parse_launch` reuses the
same helper, applying the configured maximum both through the generic property
setter and the specialized helper API. The drop behaviour remains enabled.
【F:src/pipeline.c†L766-L772】

Comparing to the previous revision (`aae36dd^`), the only change around this
code path is the replacement of the hard-coded literal `4` with the configurable
`cfg->appsink_max_buffers` value; the rest of the pipeline setup is unchanged.
【81aad3†L1-L27】

Because of that, the runtime behaviour with `appsink-max-buffers = 4` should be
the same as before. If failures occur, they are likely due to timing or workload
changes elsewhere (for example, more work being done in the appsink consumer
thread or higher upstream burstiness) rather than the buffer programming itself.
Increasing the buffer count to `8` simply provides more elasticity before frames
are dropped.

## I-frame starvation hypothesis

The failure mode you described would require the receiver to discard every
incoming access unit until the next IDR arrives, leaving the decoder stuck in a
loop that never sees a key frame. The current implementation does not match that
pattern:

- The appsink is configured with `drop = TRUE` so upstream producers are never
  blocked, but every buffer is still queued in order up to the configured
  `max-buffers` before the oldest sample is evicted. 【F:src/pipeline.c†L517-L543】
- The dedicated appsink pump thread immediately forwards each pulled buffer to
  the MP4 recorder and hardware decoder without first scanning for IDR markers,
  so access units are consumed as they arrive rather than being filtered by
  software. 【F:src/pipeline.c†L980-L1044】
- The decoder hands every buffer to the Rockchip MPP via `decode_put_packet`
  until the hardware accepts it; there is no retry loop that waits specifically
  for key frames. 【F:src/video_decoder.c†L665-L688】
- When the hardware reports that a frame had to be dropped (for example because
  reference data was missing after packet loss), the frame thread raises an IDR
  request so the transmitter can send a fresh key frame. 【F:src/video_decoder.c†L362-L393】【F:src/idr_requester.c†L337-L418】

If initialisation still stalls until more buffers are allowed, the most likely
cause is that upstream is not honouring those automatic IDR requests. Verifying
that the `[idr]` block is enabled and points to a responsive endpoint ensures the
receiver can recover quickly. 【F:src/pipeline.c†L1221-L1294】【F:README.md†L165-L169】

## Understanding repeated IDR attempts with `errinfo=1`

The trace where every dequeue reports `H265_PARSER_REF: cur_poc … Could not find
ref` shows the decoder repeatedly encountering frames that depend on references
it never received. Each time the Rockchip MPP reports that condition the frame
thread logs `MPP: dropping frame errinfo=1 discard=0` and notifies the IDR
requester. 【F:src/video_decoder.c†L362-L392】 The requester immediately enters
its recovery loop, issuing HTTP GETs in bursts of four spaced by 50 ms before
backing off exponentially up to a 500 ms interval while the receiver keeps
dropping frames. 【F:src/idr_requester.c†L14-L48】【F:src/idr_requester.c†L337-L399】

Because the IDR requester resets its quiet timer only after the decoder has
gone at least 750 ms without new warnings, a steady stream of decode errors will
never let the back-off expire. 【F:src/idr_requester.c†L356-L374】 That is why the
log shows `attempt` climbing monotonically even though the HTTP worker threads
complete successfully and clear `request_in_flight`. 【F:src/idr_requester.c†L448-L479】
The only way out of that state is for the transmitter to deliver an actual IDR
frame (or for the pipeline to be restarted so the decoder discards its damaged
reference picture list).

If the log does not contain `HTTP request … did not succeed`, the camera is
acknowledging the GET request and still failing to inject a key frame, which
points to an upstream firmware or configuration issue rather than the appsink
queue depth. 【F:src/idr_requester.c†L92-L200】 Common fixes are to verify the
`[idr]` configuration (host, port, and path) matches the camera API that
actually triggers an IDR, and to report the problem to the vendor when the
firmware ignores repeated requests. In the meantime restarting the receiver
pipeline forces the decoder to reset its state so normal playback can resume
once a valid key frame arrives.

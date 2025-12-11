# GStreamer chain opportunities with custom H.265 elements

With the `sstarh265depay` and `sstarh265parse` plugins registered in-process, we now control the RTP-to-byte-stream path and can hook into it without relying on system plugins. The pipeline keeps references to the key video elements so they can be inspected or retuned while running via `pipeline_get_video_chain_handles`.

## Tunable points worth exposing
- **Depayloader (`sstarh265depay`)**: monitor sequence gaps or corruption marks and surface stats to the UI or telemetry. Its payload-type property can be changed when negotiating new senders.
- **Parser (`sstarh265parse`)**: adjust alignment (AU vs. NAL) or add pad probes to rewrite VPS/SPS/PPS if we detect camera metadata changes mid-stream.
- **Capsfilter (`video_capsfilter`)**: swap caps to experiment with bytestream vs. HVCC framing or to force downstream colorimetry/bitdepth fields when cameras fail to signal them.

## Next enhancements to consider
- Add a configurable **jitter buffer** before the depayloader (e.g., `rtpjitterbuffer` tuned for slice-unit transport) so we can control lateness and packet-loss concealment.
- Insert an **identity/queue tap** after the parser for lightweight latency metrics or optional capture of the raw H.265 stream to disk without engaging the recorder path.
- Allow runtime **property overrides** (e.g., depayloader payload-type, parser drop/insert AUD) via OSD or control socket using the retained element handles.

These hooks give us room to iterate on packet recovery and metadata repair without rebuilding third-party plugins.

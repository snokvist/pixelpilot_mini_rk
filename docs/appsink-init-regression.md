# Appsink initialization regression scan

To understand why `appsink-max-buffers = 4` sometimes fails to start the
stream after the recent configurability work, I checked the latest six commits
on the `work` branch.

## Summary of recent commits

1. **Document appsink buffer configuration behaviour** – documentation only
   (no code path changes).
2. **Update sample INI profiles** – the three profile files now set
   `appsink-max-buffers = 8` so that field tests use the higher queue depth by
default.
3. **Update osd-sample.ini** – adjusts splash idle timeout and recording mode.
4. **Merge PR #82** – introduces the configurable appsink queue depth and new
   telemetry hooks.
5. **Merge PR #81** – hardens the SSE streamer against misbehaving clients.
6. **Merge PR #80** – clarifies recording indicator states.

## Findings

- The runtime default for the queue depth is still four buffers. The
  `cfg_defaults` routine initializes `appsink_max_buffers` to `4`, so builds
  that do not override the value continue to use the previous limit.【F:src/config.c†L131-L188】
- The pipeline now resolves the limit from the parsed configuration before it
  creates the appsink. If the configuration supplies a positive value it is
  used; otherwise the code falls back to `4` – matching the historical
  behaviour.【F:src/pipeline.c†L510-L571】【F:src/pipeline.c†L719-L780】
- The sample configuration files were adjusted to request `8` buffers. This
  was the only change in the last six commits that altered the queue depth, and
  it affects only deployments that load these example INI files.【F:config/osd-sample.ini†L12-L35】【F:config/minimal-udpsrc.ini†L11-L20】【F:config/psd-sample.ini†L28-L40】
- None of the other recent commits touch the appsink consumer thread or the
  decoder handoff. The SSE hardening and recording indicator tweaks stay clear
  of the max-buffer logic, so they are unlikely to influence the observed
  initialization failures.

## Conclusion

The code paths that manage the appsink queue still clamp to four buffers by
default, so reverting the configuration to `appsink-max-buffers = 4` should
reproduce the behaviour that existed before PR #82. The only material change
among the last six commits is that the sample configurations now request a
larger queue depth, which simply increases elasticity during slow start.

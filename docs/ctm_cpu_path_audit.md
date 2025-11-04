# CTM CPU Fallback Footprint

The current `video_ctm` implementation still retains the original CPU/RGA
fallback alongside the GPU-backed pipeline. The tables below capture the main
areas that are only exercised when we drop out of the GPU path and therefore
represent removable surface area if the project decides to require the GPU
backend.

## Data structures

| Location | Description | Approx. LOC |
| --- | --- | --- |
| `include/video_ctm.h` lines 31-38 | Intermediate RGBA buffer metadata and LUT storage only used by the CPU/RGA conversion path. | 8 |

## Lifecycle glue

| Location | Description | Approx. LOC |
| --- | --- | --- |
| `src/video_ctm.c` lines 662-666 | CPU-specific initialisation toggles (`lut_ready`, `gpu_active`, `gpu_forced_off`, `gpu_state`). | 5 |
| `src/video_ctm.c` lines 675-692 | CPU cleanup during `video_ctm_reset`, including freeing the RGBA staging buffer and clearing LUT state. | 18 |

## CPU colour transform logic

| Location | Description | Approx. LOC |
| --- | --- | --- |
| `src/video_ctm.c` lines 735-812 | Fixed-point LUT generation, clamping helpers, and the per-pixel `apply_rgba_matrix` loop. | 78 |
| `src/video_ctm.c` lines 870-909 | CPU portion of `video_ctm_prepare` that allocates/interprets the RGBA staging buffer when the GPU path is unavailable. | 40 |
| `src/video_ctm.c` lines 931-1000 | CPU portion of `video_ctm_process` that performs the NV12↔RGBA conversions and applies the LUT. | 70 |

## Summary

Roughly **219 lines** across `video_ctm.c` plus 8 struct fields in
`include/video_ctm.h` are dedicated to keeping the CPU-only path alive. Removing
these sections would substantially shrink the CTM module and eliminate the
runtime fallback that can be triggered when GPU initialisation fails.

# Production Linux NGX backend probe

`ngx_backend_probe` exercises the emulator's actual `NgxDlssUpscaler`, Vulkan device,
memory allocator, scheduler, resource transitions and runtime discovery. It creates
no window, frontend, `Core::System` or guest process. The separate
[SDK probe](linux-ngx-probe/README.md) tests NVIDIA's API directly.

Build with `ENABLE_LINUX_NVIDIA_DLSS=ON` and `YUZU_TESTS=ON`, using the
[normal build configuration](../docs/linux-dlss.md):

```sh
cmake --build build --target ngx_backend_probe --parallel 4
probe_state=$(mktemp -d /tmp/lichii-backend-probe.XXXXXX)
mkdir -p "$probe_state/config" "$probe_state/data" "$probe_state/cache" "$probe_state/empty"
env -u DISPLAY -u WAYLAND_DISPLAY \
  XDG_CONFIG_HOME="$probe_state/config" XDG_DATA_HOME="$probe_state/data" \
  XDG_CACHE_HOME="$probe_state/cache" EDEN_NGX_DIR="$PWD/.cache/ngx/lib" \
  VK_LAYER_VALIDATE_SYNC=1 build/bin/ngx_backend_probe --validation
env -u DISPLAY -u WAYLAND_DISPLAY \
  XDG_CONFIG_HOME="$probe_state/config" XDG_DATA_HOME="$probe_state/data" \
  XDG_CACHE_HOME="$probe_state/cache" EDEN_NGX_DIR="$probe_state/empty" \
  build/bin/ngx_backend_probe --expect-unavailable
```

`--validation` requires the Khronos validation layer; it fails if the layer is
missing or reports an error. `VK_LAYER_VALIDATE_SYNC=1` also enables synchronization
validation. For privately unpacked layers, set `VK_LAYER_PATH` to their manifest
directory and `LD_LIBRARY_PATH` to their library directory. Without `--validation`,
the probe still evaluates and checks pixels but does not establish validation
coverage. The executable is deliberately separate from CTest because it requires
an NVIDIA GPU, the external proprietary runtime, and optionally validation layers.

The probe runs 40 evaluations across two complete device/NGX lifetimes. Cases cover
RGBA16F/D32 inputs at 640×360→960×540, 800×450→1200×675 with 832×480 backing images,
Mario Kart World's B10G11R11/D24S8 formats at 1248×700→1920×1080, and 640×360 DLAA.
Each case checks retained history, nonzero jitter, reset after a scene-color change,
and feature recreation when only the forward/reversed depth convention changes.
Every output pixel must have finite RGBA and RGB within 0.025 of the constant
synthetic scene; channel means must be within 0.015. Input checks reject invalid
geometry, missing views, insufficient backing extents, nonfinite/out-of-range
jitter, zero motion scales, aliased inputs and output feedback. Rejected inputs
must leave the adapter usable for the next valid evaluation.

On the RTX 3070, NVIDIA driver 615.78.08 and DLSS runtime 310.9.16, all 40
evaluations passed with clean Khronos synchronization validation. Empty-runtime
discovery was rejected as expected. These uniform-color scenes establish execution,
resource handling and readback. They cannot establish moving-object motion,
depth-edge reconstruction, jitter correctness during rendering, game image quality
or performance. The MKW math helpers and captured viewport evidence are documented
in [the investigation](../docs/mkw-dlss-investigation.md); race integration remains
unfinished.

The near-equal 640×360→641×361 case checks that NGX DLAA is selected only for exactly equal input/output extents. Evaluation parameters use a separately allocated NGX parameter map rather than pre-populating capability queries. See larryliu985/Eglantine commit 1176220760758bf5e1c5cc2ef184325bd742f8ef for the independent implementation reviewed for this change; NVIDIA’s SDK permits both map types, while recommending capability maps for capability queries.

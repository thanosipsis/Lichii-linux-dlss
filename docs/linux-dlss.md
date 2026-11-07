# Native Linux DLSS development

This fork adds an optional native Linux x86-64 NGX backend alongside the existing Windows Streamline backend. It uses the renderer's validated temporal inputs rather than applying DLSS to the final window image. The desktop frontend is Qt 6 Widgets in C++.

## Current scope

The standalone Vulkan probe runs real DLSS Super Resolution on an RTX 3070 with NVIDIA driver 615.78.08. Nine evaluations cover two resolutions, retained history, scene resets, feature recreation, and oversized backing images with a smaller active render subrect. Khronos synchronization validation reports no errors. An absent runtime fails feature discovery as expected.

The [production adapter probe](../tools/linux-ngx-backend-probe.md) additionally runs the emulator's real device, allocator, scheduler and NGX adapter without starting a frontend or game. It checks 40 evaluations, including MKW's captured formats and active resolution, forward/reversed depth, DLAA, invalid-input rejection and device recreation.

The recognized game replacement paths remain title-specific. Fire Emblem has a temporal input contract; Star Fox retains its spatial path. Mario Kart World's saved race captures now identify pre-HUD scene color, depth and current/previous camera matrices. Tested camera reprojection and jitter helpers are available, but moving-object motion, coherent scene jitter and the renderer hook remain unfinished. Its name or the NVIDIA setting alone does not cause an evaluation. See the [MKW investigation](mkw-dlss-investigation.md). The upstream docked-photo-mode bilinear fix is not a temporal-racing integration.

Frame generation and ray reconstruction are outside this implementation.

## Reproduce the build

Obtain NVIDIA's SDK/runtime as external, SHA-256-verified inputs pinned to `NVIDIA/DLSS` commit `9981c6350dc58c0ce21f024fd65e31e851c31689`:

```sh
python3 tools/setup-linux-ngx.py
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DENABLE_LINUX_NVIDIA_DLSS=ON -DEGLANTINE_NGX_SDK="$PWD/.cache/ngx" \
  -DYUZU_TESTS=ON -DBUILD_TESTING=ON \
  -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF \
  -DUSE_DISCORD_PRESENCE=OFF -DENABLE_UPDATE_CHECKER=OFF \
  -DENABLE_WERROR=OFF -DYUZU_USE_BUNDLED_SDL3=OFF \
  -DYUZU_USE_EXTERNAL_FFMPEG=OFF
cmake --build build --parallel 6
ctest --test-dir build --output-on-failure
```

Qt Charts is required for the Qt frontend. All SDK headers, the NGX stub archive, DLSS runtime and NVIDIA license remain outside tracked source in `.cache/ngx`. The setup script records the upstream license path. CMake accepts an official SDK layout (`include/`, `lib/Linux_x86_64/`) or the setup script's staging layout (`include/`, `lib/`). `ENABLE_LINUX_NVIDIA_DLSS` defaults to OFF.

## Runtime discovery and verification

Enable the NVIDIA DLSS setting before starting a supported title. `EDEN_NGX_DIR` overrides the runtime search directory; otherwise it is the `ngx` directory under the emulator's data folder. Use NVIDIA's Linux `libnvidia-ngx-dlss.so` or versioned `libnvidia-ngx-dlss.so.*`, not Windows DLLs. Capability availability is logged separately from the first successful game evaluation (`Linux NGX DLSS evaluated successfully`).

The backend queries NGX's required Vulkan extensions before creating the device, enables buffer-device-address features, and serializes SDK calls. It rejects invalid geometry, unsupported formats/aspects and aliased input/output images before recording. Inputs are transitioned for sampled reads and restored afterwards. Output synchronization covers transfer clears as well as compute writes. Depth direction is explicit per input; changing it recreates the feature and resets history. Existing Fire Emblem callers retain reversed depth. The recording worker is drained before replacing the guest output so a failed evaluation can use the same-frame FSR fallback. Renderer teardown drains recorded work before releasing NGX resources.

The existing `EDEN_NX2_DLSS_REPLACEMENT=fsr` override forces FSR; explicit native/standin overrides do not enable temporal history. The NVIDIA checkbox enables history only through a validated temporal path. The render preset defaults to K; `EDEN_NX2_DLSS_PRESET=J|K|L|M|default` selects another diagnostic preset.

Run the [standalone probe](../tools/linux-ngx-probe/README.md) before investigating game-specific issues. Synthetic success establishes runtime execution and synchronization, not game image quality or an FPS improvement. Keep game captures local.

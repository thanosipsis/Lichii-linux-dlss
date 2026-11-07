# Mario Kart World DLSS investigation

The native Linux NGX backend is implemented. Its runtime passes standalone GPU evaluations, and the emulator initializes NGX successfully. Mario Kart World does not yet use it during races. Enabling the NVIDIA setting establishes runtime availability, not a validated game reconstruction path.

## Verified scene boundaries

Observed in a live handheld solo race for program ID `0400C3F00006E000`:

- Active world rendering is 1248x700 in `B10G11R11_FLOAT`; camera constants also carry nominal 1280x720 dimensions.
- The same scene color surface receives major geometry passes and subsequent postprocessing. The final internal-color filter uses fragment program `7e53c9e2d749ec45`, with ten samples from one color texture at fragment cbuf2+0x80. No separate temporal history, motion or depth input was observed in that shader.
- The first draw of the following 1920x1080 pass samples `B10G11R11_FLOAT` via fragment cbuf2+0x20, using fragment program `e43c4b68e728622e` and vertex program `8e97cea098cfdbc6`. Its color mapping and clamping cross from HDR values above 1 to display-range output. Subsequent draws compose the HUD. Replacing the entire pass would also replace HUD draws and is inappropriate.
- Final presentation uses a larger 2560x1440 allocation with a 1920x1080 active image. Allocation extent must not be treated as a DLSS upscaling ratio. Alpha bounds differ between paused and racing frames.

Pass numbers are diagnostic, not stable identifiers. In the retained full race capture, the final internal-color pass is 117, the scene-to-HUD composition pass is 118, and the final presentation draw is 119. An earlier frame had different pass and draw counts.

## Depth and camera evidence

CPU-only RenderDoc structured-event extraction confirms that the world geometry co-attaches
1248x700 `B10G11R11_FLOAT` color and 1248x700 `D24_UNORM_S8_UINT` depth. The pre-HUD
`e43c4b68e728622e` draw samples that exact world-color resource and writes the 1920x1080
composition target. The depth-derived `d7802c964028e6ad` draw samples the full-size scene
depth and a half-size D24S8 view. These associations are verified in saved frame 4567,
without GPU replay. Resource IDs and event/chunk numbers belong to that capture and are
not portable runtime identifiers.

Shader-cache environments identify `D24_UNORM_S8_UINT` inputs at fragment cbuf2+0x20 and +0x28 in fragment program `d7802c964028e6ad`. That program derives a biased two-channel field from scalar depth samples and neighboring depth gathers. Its RG8 output is not a verified motion-vector texture and must not be passed directly to DLSS.

A shared vertex camera constant buffer at guest cbuf index 4 contains:

| Offset | Observed structure |
| --- | --- |
| 0x00 | Row-major 3x4 view matrix |
| 0x30 | 4x4 view-projection matrix |
| 0x70 | 4x4 projection matrix |
| 0xb0 | 3x4 inverse view matrix |
| 0x120 | Active dimensions and reciprocals |
| 0x130 | Nominal dimensions |
| 0x150 | Second 3x4 view matrix |
| 0x180 | Second 4x4 view-projection matrix |
| 0x1c0 | Second 4x4 projection matrix |
| 0x200 | Second 3x4 inverse view matrix |

For both blocks, captured P*V agrees with VP (relative error below 7.1e-8), and V*inverse(V) agrees with identity within 0.00018.

The consecutive RenderDoc captures verify the previous-frame relationship. For world vertex program `9198357f289adf23`, an 864-byte camera block in frame 4568 contains previous view (+0x150), previous VP (+0x180), and previous projection (+0x1c0) that are byte-for-byte identical to frame 4567's current view (+0x00), VP (+0x30), and projection (+0x70). The current VP changes between the frames, while the projection stays identical. Both active dimensions are 1248x700. This supplies a verified camera-only reprojection basis, not moving-object velocity.

The two projection blocks were byte-identical in the inspected frame. A vertical projection term of -0.18 represents a large camera offset, not subpixel jitter. No frame-to-frame jitter sequence has been verified. A temporal port needs coherent scene/depth jitter and appropriate handling of moving objects; camera-only reprojection is not proof of per-object velocity.

## Captured viewport and depth mapping

Four world draws in handheld race frame 4567 used viewport 0 with origin (0, 700),
size (1248, −700), and depth endpoints (0, 1). All four graphics pipelines set
`VkPipelineViewportDepthClipControlCreateInfoEXT::negativeOneToOne` to true and
used identity viewport swizzles. For upper-left render-pixel coordinates, positive
NDC Y maps to the top edge. Clip Z of −1 maps to depth 0, and +1 maps to depth 1.

| Structured draw chunk | Viewport state chunk | Depth comparison | Depth writes |
| --- | --- | --- | --- |
| 27490 | 27483 | LESS | enabled |
| 30850 | 30529 | LESS | enabled |
| 32869 | 32858 | EQUAL | disabled |
| 36969 | 36800 | LESS_OR_EQUAL | disabled |

At draw 32869, the captured SPIR-V for vertex shader `9198357f289adf23` writes
`gl_Position.x/y/z/w` directly from the corresponding cbuf4 projection rows at
0x70, 0x80, 0x90, and 0xa0. The shader uses viewport 0. The projection maps
view-space Z of −1 to depth 0 and approximately −10000 to depth 1. This is forward
depth. A renderer integration should derive these settings from the active draw
when the camera, viewport, or game mode changes.

## Offline camera math

`src/video_core/mkw_dlss_contract.h` parses the captured cbuf4 layout, checks the
camera matrices and active image extent, and reprojects a stationary world point to
its previous render pixel. The caller supplies the Y orientation, clip-depth range,
device-depth endpoints, and raster jitter applied to the depth pass. The four draws
above use `{NdcY::PositiveAtTop, ClipDepth::MinusOneToOne, 0, 1}`. Jitter shifts
clip X and Y by the requested pixel displacement and preserves clip Z and W. The
game's optical projection offset remains part of the camera.

The helper also distinguishes duplicate evaluations from resets on cuts, gaps,
resize, or a mismatched previous camera. An explicit cut takes precedence over
duplicate suppression. These helpers are covered by synthetic tests and await
integration with the world renderer. The independently moving kart and character
velocities still need a verified source or a separately validated reconstruction.

Run the camera tests from the repository root:

```sh
cmake --build build --target tests
build/bin/tests '[mkw_dlss_contract]'
```

The tests cover stationary points under camera translation and rotation, both
Y orientations, both clip-depth conventions, normal and reversed device-depth
ranges, offscreen and behind-camera points, jitter, invalid camera data, and
history resets. The separate private check on the consecutive camera buffers
parsed both and matched their previous/current view-projections. Reprojecting a
synthetic stationary point with those camera matrices agreed with direct forward
projection within 0.008 render pixels.

## Capture constraints and retained evidence

The initial sampled+buffer frame dump reached its 2-GiB uncompressed-readback budget. Large sampled images accounted for 1.75 GiB. Its compressed file size was much smaller, so file size does not establish capture completeness. No conclusion about absent late-frame resources should be drawn from that dump.

An attachment-only race capture completed without truncation (120 passes, 2114 draws, 108 dispatches, 284 images). A separate attachment-only capture contains the pause menu, not a freshly rendered race. The built-in color-attachment dump does not record original depth attachments.

RenderDoc 1.46 is attached successfully in a separate capture run. Three private captures are retained locally, including consecutive host frames 4567 and 4568. The physical RTX 3070 replay attempt failed at `vkAllocateMemory` with `VK_ERROR_OUT_OF_DEVICE_MEMORY` before event inspection. Structured-event extraction successfully reads the saved Vulkan events and mapped buffer data without launching the game. Software replay is also unavailable because the capture requires VK_EXT_conservative_rasterization, which llvmpipe does not support. Capture instrumentation hides some NVIDIA extensions, so the RenderDoc run is not a validation of NGX evaluation or performance.

Game-derived images, shaders, buffers and `.rdc` files remain outside Git under the owner's experimental data directory. Do not upload them.

## Remaining verification

1. Bind the verified depth/color/camera inputs from live renderer state with strict title, shader, format and extent checks; capture-local resource IDs must never be hardcoded.
2. Establish a genuine motion source or implement and validate appropriate velocity generation, including moving karts and characters. The inspected frame has no full-size velocity attachment or full-size compute storage image; packed SSBO or auxiliary fields have not been excluded.
3. Apply the tested jitter transform across all participating depth and color draws. Bind history resets to the actual game-frame, camera-cut, and viewport lifecycle.
4. Resolve scene color before the verified composition draw, retaining the game's original tone mapping and subsequent HUD. Account for the preceding spatial filter rather than applying unverified double filtering.
5. Verify actual emulator-side NGX evaluations, moving-image quality and frame times in a future explicitly authorized game run.

The emulator was stopped at the user's request. It must not be launched again without a new instruction.

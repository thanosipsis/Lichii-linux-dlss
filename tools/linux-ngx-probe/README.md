# Linux NGX evaluation probe

This headless Vulkan executable exercises the driver and DLSS Super Resolution runtime directly. It creates synthetic HDR color, reversed depth, and zero motion-vector images and evaluates three frames per feature. It tests two render resolutions, then repeats the larger one with oversized input textures. All three inputs in that case have 832×480 backing images, with an 800×450 active region rendered to 1200×675. The unused right and bottom color strips are filled with bright RGB (2, 2, 2). Each sequence includes a history frame and a scene-color change with a reset. The probe reads the output back, checks every RGB pixel for finite values, compares the mean of each channel to the input scene color, and checks for output RGB values above 1.0 that would reveal sampling of the padding.

## Build and run

The probe needs a C++20 compiler with `_Float16`, Vulkan headers including Vulkan-Hpp, the Vulkan loader, and the Linux x86-64 NGX SDK. From the repository root, point CMake at an SDK staging directory containing `include/nvsdk_ngx_vk.h` and `lib/libnvsdk_ngx.a`. Put the DLSS runtime in the directory passed as the first argument. The second argument is a writable directory for NGX data.

```sh
cmake -S tools/linux-ngx-probe -B /tmp/lichii-ngx-probe-build -G Ninja \
  -DNGX_SDK_ROOT="$PWD/.cache/ngx"
cmake --build /tmp/lichii-ngx-probe-build
VK_LAYER_VALIDATE_SYNC=1 /tmp/lichii-ngx-probe-build/linux-ngx-probe \
  "$PWD/.cache/ngx/lib" /tmp/lichii-ngx-probe-data
```

When using the emulator's cached Vulkan headers, add `-DVulkan_INCLUDE_DIR="$PWD/.cache/cpm/vulkanheaders/v1.4.345/include"` to the configure command. The probe enables the Khronos validation layer when available and exits with a failure if it reports errors. A passing run prints the GPU, required extensions, `SuperSampling.Available=1`, each active size, backing size, output size, and nine RGB measurements.

On an RTX 3070 with driver 615.78.08, NVIDIA SDK commit `9981c6350dc58c0ce21f024fd65e31e851c31689`, runtime `libnvidia-ngx-dlss.so.310.9.16`, and validation layers 1.4.363, all nine evaluations passed with synchronization validation enabled. The first two frames at each size averaged approximately (0.10003, 0.25, 0.75). The reset frame averaged (0.75, 0.25, 0.10003). The exact-size and padded 800×450 runs produced identical printed means and zero pixels above 1.0. An empty runtime directory on this host returned NGX error `0xbad00012` at feature discovery.

The synthetic scene verifies library execution and readback. Game-specific motion vectors, camera jitter, render pass selection, and useful temporal reconstruction require separate gameplay captures.

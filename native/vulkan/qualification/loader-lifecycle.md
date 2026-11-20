# Linux Vulkan driver initialization and LeakSanitizer

On openSUSE Tumbleweed, the NVIDIA 620.70.06 desktop ICD lost seven host allocations each time a process created and destroyed a Vulkan instance. The same process using the Intel ARL ICD exited cleanly. A standalone Vulkan program reproduced the NVIDIA failure without linking Kalandra, submitting GPU work, or allocating a device buffer.

## Minimal driver reproducer

From the repository root on Linux with Clang and Vulkan development headers:

```sh
clang++ -std=c++23 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  native/vulkan/tests/LoaderControl.cpp -lvulkan -o loader-control
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:fast_unwind_on_malloc=0
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export LSAN_OPTIONS=detect_leaks=1:exitcode=23
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json ./loader-control --maps-iterations 1
```

The control calls `vkCreateInstance`, enumerates and identifies the physical devices, and calls `vkDestroyInstance` with the same allocator. Its map snapshot is taken before destruction. For an unloaded frame, subtract the mapping start from the program counter and add the file offset to identify the instruction in the shared library. Use a single iteration for this attribution because addresses can be reused after unloading.

## Allocation ownership

The first NVIDIA run reported **2,061 bytes in seven allocations**. Both paths originate inside the NVIDIA GLX ICD during `vkCreateInstance` (Vulkan loader `loader_icd_scan, libvulkan +0x298c3`). Disabling implicit Vulkan layers produced identical counts at 1, 2 and 11 cycles.

| Objects from one cycle | Allocation path |
| --- | --- |
| One 104-byte direct and one 104-byte indirect allocation | NVIDIA GLX `+0x9fe0d` queries EGL devices through GLVND EGL `QueryVendorDevices` (libegl.c:969). Mesa EGL `+0x1860e → +0x25487 → +0x252fd` allocates two device records. |
| Two indirect allocations of 144 bytes each | The same Mesa device discovery path calls libdrm `drmGetDevices2 → process_device → drmDeviceAlloc` (xf86drm.c:4861, 4105, 4073) for the underlying DRM device records. |
| One 184-byte direct allocation and two indirect allocations of 88 and 1,293 bytes | NVIDIA GLX `+0x9fe6c` initializes an NVIDIA EGL display. NVIDIA EGL core `+0x99fe96` registers a D-Bus connection. Its `+0x9a0426` call requests `org.freedesktop.DBus.ListNames`. libdbus queues the reply through `dbus_connection_send_with_reply_and_block` and `_dbus_message_loader_queue_messages`, allocating the message in `dbus_message_new_empty_header` and its buffers in `reallocate_for_length` (dbus-string.c:397). Reply sizes vary with bus contents. |

The proprietary frames are identified by module and offset. These measurements establish where ownership is lost from the process. They do not establish which internal vendor instruction should release the last reference. The affected stack used Linux 7.3.3, Vulkan loader 1.4.363, Mesa EGL 26.2.6, libdrm 2.4.134, libglvnd 1.7.0 and libdbus 1.14.10. The NVIDIA EGL core build ID was `3c84544d7d759ed33c744201f015d20c6685d653`.

## Compute-only workaround

Select the NVIDIA EGL ICD directly and skip the driver's desktop D-Bus probe for the compute process. The helper copies the installed manifest's metadata to stdout and substitutes the sibling EGL library. Its output belongs in a local application or test directory. [NVIDIA documents both Linux Vulkan ICD entry points](https://download.nvidia.com/XFree86/Linux-x86_64/595.99.02/README/installedcomponents.html).

```sh
python3 native/vulkan/NvidiaComputeIcd.py > nvidia-compute.json
VK_DRIVER_FILES="$PWD/nvidia-compute.json" __GL_DisableDBus=1 ./loader-control --iterations 64
```

The `__GL_DisableDBus` setting was verified on 620.70.06. Requalify it after driver upgrades. The desktop configuration remains available for programs that need display integration. To expose Intel alongside NVIDIA, append the Intel ICD path to `VK_DRIVER_FILES` with a colon. All Kalandra instance, device, queue and buffer destruction follows the normal code path.

| NVIDIA configuration | Result |
| --- | --- |
| Stock GLX ICD | 7, 14 and 77 leaked allocations after 1, 2 and 11 lifecycles. The 11-cycle run leaked 23,767 bytes. |
| EGL ICD alone | 6 D-Bus allocations after 2 cycles, 3,202 bytes. |
| Stock GLX ICD with the D-Bus probe disabled | 44 Mesa/DRM allocations after 11 cycles, 5,456 bytes. |
| EGL ICD with the D-Bus probe disabled | Clean exit at 1, 2, 11 and 64 cycles. Each cycle enumerated the physical RTX 5090. |

Keeping the GLX library loaded across 11 cycles and explicitly releasing it at the end still leaked the final seven objects. Restricting GLVND's EGL vendor list also lost NVIDIA device availability during a 64-cycle test. Neither configuration is used for acceptance.

## Kalandra acceptance

```sh
python3 native/vulkan/qualification/public_sanitizers.py --compiler /path/to/kalandra-test-observer \
  --output results --nvidia-compute
```

To replay compiled CI bundles, replace `--compiler /path/to/kalandra-test-observer` with `--artifacts /path/to/public-sanitizers`. Omit `--nvidia-compute` to test the stock NVIDIA ICD. Both modes require exact numerical output, empty standard error and exit zero. The harness retains each execution in `results.json`.

- Physical RTX 5090 (`10de:2b85`) with the compute profile and Intel ARL (`8086:7d67`) with its standard ICD each passed 64 consecutive instance lifecycles. Both devices also remained visible through 64 cycles in a single process.
- All 18 sanitized application runs passed. Elementwise multiply/add, mixed CPU/GPU work and optimized matrix multiplication each produced the expected float32 bits at 1, 2 and 4 workers on each GPU. The binaries came from [the Vulkan source qualification](https://github.com/kalandra-lang/kalandra/actions/runs/44627060005). The stock-ICD control rejected all nine NVIDIA runs with LSan exit 23 and passed all nine Intel runs. On stock NVIDIA the elementwise program produced the same bits but leaked 4,194 bytes across two initializations.
- Both GPUs passed instrumented runtime and task suites with Vulkan validation enabled. Coverage included independent matrix references, escaped buffers, concurrent callers, a 257-stage program, cancellation and deadline precedence, 13 raw failure points and 36 managed fault/cancellation cases. Resource accounting returned to zero after settlement.

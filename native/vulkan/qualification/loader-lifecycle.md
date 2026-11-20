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

## Normal launch remains affected

The package-level compute profile tested in the first draft bypassed the two
driver paths. It did not free their allocations. It was withdrawn.

The stock NVIDIA GLX ICD reported 7, 14 and 77 leaked allocations after 1, 2
and 11 lifecycles. The corresponding totals were 2,061, 4,194 and 23,767
bytes. A single Intel ARL ICD run and 11 repeated Intel cycles exited cleanly.
Disabling implicit Vulkan layers did not change the NVIDIA counts.

A diagnostic copy of libdbus 1.14.10 traced the exact leaked 184-byte
`ListNames` reply. `dbus_connection_send_with_reply_and_block` returned it to
NVIDIA EGL core with one reference remaining. Two Vulkan instance cycles
returned two distinct reply addresses, both identified by LeakSanitizer as
leaked. The application never receives these D-Bus pointers. Calling the
public `dbus_shutdown` after Vulkan destruction left the same seven
allocations, so a process-global cleanup call cannot fix the missing release.

Mesa 26.2.6 and 26.2.7 source shows that `eglQueryDevicesEXT` refreshes
and caches DRM devices, while `eglInitialize` registers the callback that
frees them. The GLX ICD's cross-vendor EGL device query reaches the first
path without registering Mesa's cleanup callback. The Mesa EGL and libdrm
frames account for the other four allocations. Both this cache and the D-Bus
reply are owned by libraries outside Kalandra.

The nine stock-ICD Kalandra executions each reported 14 leaked allocations
with the same Mesa/libdrm and D-Bus stack categories. Their exact numerical
outputs were correct. LeakSanitizer counted 4,274 bytes in each run; the
standalone two-cycle control counted 4,194 bytes in the same 14 categories.
The D-Bus reply payload depends on the current bus names. All nine Intel
executions passed with no leaks. No Kalandra allocation appears as a leak
source in these traces, but the NVIDIA qualification still fails.

For an isolated diagnostic, the checksum-verified NVIDIA 620.73.04 userspace
archive was extracted into a temporary directory and loaded without
installation. One and two control lifecycles each reported 992 bytes in
eight Mesa/libdrm allocations and no D-Bus reply leak. That build still
fails unsuppressed LeakSanitizer. Its userspace version differed from the
installed kernel module, so this result does not qualify a driver upgrade.
The temporary archive and extracted directory were removed after testing.

Neither Kalandra's instance/device destructor nor its dynamic loader owns
these allocations. The standalone control links no Kalandra runtime and
reproduces the exact loss. An application-side driver setting, suppressing
LeakSanitizer, or retaining the ICD indefinitely does not repair the missing
releases. Qualification must use the stock ICD and exit zero before this
issue can close.

## Kalandra acceptance

```sh
python3 native/vulkan/qualification/public_sanitizers.py --compiler /path/to/kalandra-test-observer \
  --output results
```

To replay compiled CI bundles, replace `--compiler /path/to/kalandra-test-observer` with `--artifacts /path/to/public-sanitizers`. Both modes require exact numerical output, empty standard error and exit zero. The harness retains each execution in `results.json`.

- The stock NVIDIA ICD fails every LeakSanitizer application execution. The
  strict harness records each failure and exits unsuccessfully. The stock
  Intel ICD passes.
- The earlier experimental EGL/D-Bus profile passed 18 numerical executions,
  64 lifecycle cycles per GPU, and the runtime/task suites with validation.
  Those results demonstrate a path around the vendor calls, not a leak fix.

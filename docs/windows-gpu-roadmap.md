# Windows CUDA and Vulkan

One implementation branch/PR covers both backends. This follows the Linux CUDA
and Vulkan work; macOS continues to use Metal. The owner requested Windows
build/package CI first and physical GPU testing later.

## Delivery sequence

1. Native Windows runtime builds for x86-64 and ARM64, secure system-driver
   loading, executable-relative resources, ABI/admission tests and Linux
   regression checks. Header-only SDK inputs stay in private build directories.
2. Complete Windows compiler commands: executable suffixes, matching static CRT,
   native object/link options, and shader lowering/validation. CUDA generation
   must not require an unsupported host NVRTC installation; portable precompiled
   resources may be produced by the pinned Linux build and verified on Windows.
3. Relocatable standalone Windows compiler packages for both GPU targets, with
   owned tools, shader resources, notices and PE dependency audits. Validate
   compilation of complete user programs, CPU execution, missing-driver/resource
   handling, reproducibility and the existing CPU target-runtime bundles.
4. Physical Windows GPU qualification: all numerical policies/types/ranks,
   cancellation/deadlines, ownership, fault precedence and resource limits.
   Performance is measured separately with idle and stability controls.

The current runtime slice is not a Windows GPU execution or installed-compiler
qualification. Windows ARM64 runtime compilation is not evidence that a vendor
provides an ARM64 CUDA driver. Do not claim a supported hardware matrix from
header/object compilation, a software Vulkan device or a CPU substitute.

Tracking: #263 (Vulkan), #264 (CUDA). Keep both capabilities in this PR and retain
explicit qualification boundaries in package metadata, documentation and CI.

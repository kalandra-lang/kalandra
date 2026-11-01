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

## Qualification status

The native runtime slice passed both backends on Windows x86-64 and ARM64.
Full installed compiler/package CI is in progress in PR #275. It includes native
CPU execution, all ten CUDA types with three arithmetic policies (51 complete
source builds), Vulkan public/source-capability compilation, secure loader tests,
PE import closure, missing owned resources and reproducible archives.

The device-resource producer builds PTX/SPIR-V from the same canonical Git source
inputs on Linux. Windows verifies the source inventory and each payload before
compilation. This avoids requiring an ARM64 NVRTC host binary. No Linux host
executable or SDK library crosses into the Windows package. The Windows-native
SPIR-V validator is built through the existing pinned vcpkg baseline with a static
CRT; its Apache notice and installed binary digest are retained.

Windows GPU execution remains unqualified. Windows ARM64 runtime compilation is not evidence that a vendor
provides an ARM64 CUDA driver. Do not claim a supported hardware matrix from
header/object compilation, a software Vulkan device or a CPU substitute.

Tracking: #263 (Vulkan), #264 (CUDA). Keep both capabilities in this PR and retain
explicit qualification boundaries in package metadata, documentation and CI.

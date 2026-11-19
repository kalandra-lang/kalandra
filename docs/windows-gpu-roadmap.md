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
Full installed compiler/package CI is tracked in PR #275. Its gates include native
CPU execution, all ten CUDA types with three arithmetic policies (51 complete
source builds), Vulkan public/source-capability compilation, secure loader tests,
PE import closure, missing owned resources and reproducible archives.

The device-resource producer builds PTX/SPIR-V from the same canonical Git source
inputs on Linux. Windows verifies the source inventory and each payload before
compilation. This avoids requiring an ARM64 NVRTC host binary. No Linux host
executable or SDK library crosses into the Windows package. The Windows-native
SPIR-V validator is built through the existing pinned vcpkg baseline with a static
CRT; its tool/dependency notices and installed binary digest are retained.

Physical Windows x64 execution passed the RTX 5090 correctness suite described
below. The hosted jobs qualify Windows ARM64 compilation and packaging. Windows
ARM64 GPU execution and other hardware still need physical qualification.

Tracking: #263 (Vulkan), #264 (CUDA). Keep both capabilities in this PR and retain
explicit qualification boundaries in package metadata, documentation and CI.

## Windows stack admission

The wide indirect-argument capability fixture exposed a CPU compiler admission
error after Windows tail-signature normalization. Sequential outgoing call
packets were summed as if they were simultaneously live. Windows admission now
charges the largest scalar outgoing packet, rounded to full eight-byte argument slots
with shadow-space allowance. All SSA values, incoming arguments and fixed
allocations remain conservatively charged. Dynamic allocations and genuinely
oversized frames remain rejected. The 16 KiB frame budget, 128 KiB runtime stack
reserve, cancellation checkpoints and unwind protocol are unchanged.

Regression coverage includes oversized packets/allocations/SSA pressure and
both Windows triples, plus O0/O2 native execution of a forty-tensor-argument CPU
program through the existing cross-platform matrix. Physical Windows GPU tests
remain separately deferred.

The expanded native matrix also exposed an actual O0 ARM64 call-ABI failure:
forty aliased tensor arguments crashed on Linux and Windows ARM64, while the
same O2 program returned 426. Stack-passed ARM64 signatures now use the existing
macOS caller-pop safeguard on all three operating systems. Register-only calls
retain `tailcc`, and matching large signatures remain eligible for `musttail`.
Different large signatures can use ordinary calls and consume stack; this is
an explicit ABI fallback, without removing cancellation or ownership checks.

## Physical Windows qualification (#276)

Windows 11 x64 (build 26300) in the KVM guest passed all 956 correctness
executions on the RTX 5090 with NVIDIA driver 610.88 and four guest-visible
logical CPUs. The suites ran the installed compiler packages from develop
commit `59072bfb`. CUDA and Vulkan each passed 478 executions. The deployed
applications lived in a Unicode path and ran from a separate working directory.

Both Vulkan timing trials passed the stability gates for the two tested matrix
shapes. Optimized and fixed-tree whole-process latency was within about 1.2%
in the median paired comparisons. CUDA missed the stability gates on both
trials. Its observed optimized/fixed-tree ratios were 1.23–1.32. Both raw
trials were retained.

The CUDA follow-up on the same Windows guest measured a median 16.925 ms for
driver module loading and function reflection in the previous optimized
bundle. The fixed-tree bundle took 0.198 ms. The candidate omitted the
optimized catalog and took 0.206 ms. Each candidate reused the previous
Windows executable and its byte-identical fixed-tree shader companion.
The patched compiler selects that shader directly for these shapes.

| Matrix shape (M × K × N) | Previous optimized / fixed-tree | Candidate / fixed-tree |
| --- | ---: | ---: |
| 128 × 32 × 128 | 1.26382 | 1.00147 |
| 129 × 17 × 128 | 1.22982 | 1.00004 |

All six comparison groups passed their stability gates with five blocks
and six fresh processes per arm in each block. All 360 timed processes and
18 separate correctness checks reproduced the exact expected outputs.
These are whole-process latencies, including startup and transfers.
Repeat the package checks with the rebuilt Windows compiler before closing
the qualification.

Preparation found that the Windows SPIR-V validator lost non-ASCII output path
characters. The tested release packages built Vulkan applications in an ASCII
scratch directory and replayed them from the Unicode path. The compiler fix
uses binary standard input to validate the same bytes. The Windows package
regression compiles directly into a Unicode output path.

Run the qualification from a Windows x64 Visual Studio developer shell with
the Windows SDK configured and both checksum-verified standalone packages
extracted. Use the packages and test sources from the same implementation
revision. The prepare stage only builds. It can run with the RTX assigned to
Linux. The frozen directory and its manifest can then be moved together.

```powershell
$runner = "native/gpu-runtime/qualification/windows_physical.py"
python $runner prepare --revision $revision --cuda $cudaPackage --vulkan $vulkanPackage --bundle $bundle
python $runner replay --bundle $bundle --output $correctness
python $runner measure --bundle $bundle --correctness $correctness --output $timing
```

Use a fresh output directory for each invocation. Include spaces and non-ASCII
characters in the package and frozen directory paths. Replay uses a separate
working directory and checks byte-exact output, exit status, missing deployed
resources, all ten numeric types, three arithmetic policies, both matrix
policies and several ranks. Prepare builds the existing asynchronous runtime
probes against the installed libraries. Replay runs them to check transfers, in-flight
cancellation and deadlines, fault precedence and cleanup.

Timing requires a passing replay on the same frozen inputs and GPU driver.
Run it by itself with the guest and host otherwise idle. It compares fixed-tree
and optimized application latency in the VM, including process startup and
transfers. Identical-build controls and interleaved comparisons must pass their
stability gates. Keep the raw logs and summaries for failed controls too.

Record the Linux host configuration, Windows build, guest vCPU/RAM, GPU driver,
package digests and results. Use an exclusive passthrough window and keep the
host administration terminal open. Shut down Windows normally at the end of
the run and verify Linux has reclaimed both PCI functions and its graphics
services. ARM64 GPU execution and other GPUs require separate evidence.

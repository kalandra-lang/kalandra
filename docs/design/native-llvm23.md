# LLVM 23 toolchain

The native compiler now targets LLVM/MLIR/Clang **23.1.3**, selected by the
[toolchain lock](../../native/toolchain-proof/toolchain.json). The Apple shader
pipeline also uses SPIRV-Tools **1.4.363.0**, matching the updated local installation;
SPIRV-Cross remains **1.4.363.0** and Apple's Metal compiler remains **32023.864**.
Older qualification and benchmark records retain the versions actually used.

Use a fresh CMake build directory when changing LLVM major versions. Compiler,
MLIR headers/libraries, command-line tools and generated objects must agree:

```sh
cmake -S . -B build/native-llvm23 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DKALANDRA_BUILD_METAL_RUNTIME=ON -DKALANDRA_BUILD_METAL_PROOF=ON
cmake --build build/native-llvm23 --target native-metal-package
```

The default prefix is the installed Homebrew LLVM 23.1.3 revision-2 keg. Supply
`-DKALANDRA_LLVM_ROOT=/path/to/llvm-23.1.3` on initial configuration if necessary.
An existing cache retains its previous compiler and dependency selections; an
environment variable does not replace them. To reconfigure the default preset,
use `cmake --fresh --preset native` and restore desired Metal options explicitly.
No Homebrew update or global PATH/version-link change is part of configuration.

## Compatibility changes

LLVM 23 can emit Objective-C class selector stubs unsupported by the qualified
Apple linker. The private Metal runtime, proof harnesses and benchmark driver
explicitly disable that optimization with
`-fno-objc-msgsend-class-selector-stubs`; ordinary Objective-C message dispatch
and ARC semantics are unchanged.

LLVM 23 canonicalization can replace integer compare/select expressions with
64-bit min/max operations. Metal 2.2 lacks the corresponding signed/unsigned
64-bit intrinsic overloads. The Metal extraction stage restores exact signed or
unsigned comparison/select operations after canonicalization, before SPIR-V
serialization. It leaves supported 32-bit min/max and the separate decorated
SPIR-V experiment unchanged. No floating-point approximation, relaxed rounding
or wider source-language GPU numeric support is introduced.

The `metal-integer-minmax` gate checks both extraction policies, all four scalar
64-bit signed/unsigned min/max forms, unchanged 32-bit forms, serialization and
SPIRV-Tools validation. Strict CPU/GPU numerical tests continue to check results
against the independent reference, including division and exceptional float bits.
Native source/worker/sanitizer and relocated package gates qualify the upgraded
toolchain separately from historical LLVM 22 evidence. This is focused upgrade
qualification, not a claim that the entire native roadmap or every test has run.

The tag-release workflow now provisions `llvm@23`, enforces the locked version,
and disables Homebrew automatic updates. Generated release notes and package
acceptance read the LLVM version from the shared lock. The mixed CPU/Metal
distribution now participates in automated publication with standalone private
dependencies and hosted Developer ID signing/notarization. Current platform
qualification is recorded in the [N5 checklist](native-distribution-qualification.md).

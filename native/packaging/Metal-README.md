# Kalandra native CPU and Apple Metal compiler

This bundle extends the CPU compiler package with Apple Metal source compilation.
Extract it and add `bin` to PATH. Keep the entire directory together: the compiler
resolves its private shader tools, generated-host headers and runtime archives
relative to its executable, including symlink invocation.

```sh
kalandra main.k --target metal -O2 -o app
./app/program
```

The output is an application directory, not one standalone executable. Keep its
`program` and generated `.metallib` files together. Copy the entire application
directory to move it. Normal, optimized, ASan/UBSan and separate TSan host builds
are supported through the existing `-O0`, `-O2`, `--sanitize` and
`--thread-sanitize` options. CPU compilation remains available without `--target`.

This external-toolchain variant requires macOS 26 or later on ARM64. LLVM/MLIR and
Clang 23.1.3, SPIRV-Cross, SPIRV-Tools, the selected Apple Metal toolchain and the
Apple SDK must remain installed at their configured locations. Exact SPIR-V roots, verified Metal compiler path, SDK path
and Metal toolchain identity are recorded in `share/kalandra-native-metal/toolchain.json`.
Moving the Kalandra bundle does not move these dependencies. Ordinary generated
applications need Metal-capable Apple hardware and system frameworks, but do not
need the compiler bundle or LLVM/MLIR. Sanitized applications additionally need
platform sanitizer runtimes.

The tools, headers and archives under `lib/kalandra/native` are compiler-private;
they do not define a public C/C++ API. See the CPU/editor READMEs for their usage
and licenses. SHA256SUMS covers the complete installed bundle; an archive checksum
is supplied separately. The native release workflow builds external-toolchain
and standalone mixed CPU/Metal variants. Standalone bundles include their private
open-source dependencies; their DMGs are signed and notarized on GitHub Actions.
Apple SDK/linker and the pinned Apple Metal compiler remain platform prerequisites.

Local or public HTTPS Git dependencies can be installed explicitly with `kalandra install --project path/to/kalandra.toml`. Installation requires system Git; subsequent project builds verify the activated cache offline. Remote installs verify TLS and require public repositories. Commit IDs and supported branch/tag names are accepted; names are locked to exact commits and unchanged requests keep their pins. Commit the generated `kalandra.lock`. Use `--frozen` to populate a cache from a matching lock without rewriting it, or `--offline` to require verified cached sources without invoking Git. Use `kalandra update name [name ...] --project path/to/kalandra.toml` to refresh selected root aliases from a matching lock; unrelated roots and unchanged-owner private pins stay fixed. Check and rebuild after updating. SSH and authenticated repositories are not supported yet.

Typed GPU programs support all ten native numeric types, dense tensor ranks
zero through eight, balanced sums, rank-two matrix products, explicit numeric
conversions and resident numerical/control graphs. Numerical computations use
checked compiler-generated shaders. Hardware float32 matmul defaults to optimized
full-precision accumulation, with pinned MIT-licensed MLX Steel SIMD-group
kernels on qualified Apple M2 Max shapes. `--matmul-policy fixed-tree` retains
separately rounded products and the historical balanced tree; explicit software
arithmetic always selects that policy. Scalar operations, ReduceSum and integer
fault checks are unchanged. Fixed-tree hardware still follows device underflow
and NaN behavior. Optimized accumulation may change rounding, signed zero and
intermediate overflow; neither policy guarantees exact real arithmetic.
Native C++ clients must rebuild with the matching headers and static runtime:
`Options` now includes `fixedTreeMatmul`. The existing shader buffer ABI and
older compiled matrix bundles remain supported.

The Steel subset's MIT notice is installed as `MLX-Steel-LICENSE.txt` beside this
README and included in generated optimized bundles as `THIRD_PARTY_NOTICES.txt`.
No MLX framework runtime, NAX kernels, CUTLASS or separately licensed CUDA DSL
components are included. Public GPU entries take numeric
scalars/tensors; internal Boolean predicates select stages and shape checks.
CPU/device transfers remain explicit. Managed closures and recursive GPU
helpers are outside this numerical contract.

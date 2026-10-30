# Typed Apple GPU numerical programs

The native Metal backend supports the ten native numeric types: `int8`, `uint8`,
`int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float32` and `float64`.
Dense row-major tensors have rank zero through eight. Elementwise operations
require equal complete tensor shapes; scalar operands lift over every element.
Matrix products remain rank two. Higher ranks add neither broadcasting nor
batch-matmul semantics.

## Numerical behavior

Integer scalar and elementwise arithmetic checks every operation at the declared
width. Overflow, division by zero and signed minimum divided by minus one fail
the task. Reductions use the balanced adjacent-pair tree, carrying an unmatched
last element unchanged. Matrix products round/check each product before applying
the same tree to its row/column terms. Empty sums and empty inner products return
positive zero. No partial numerical result is published on failure.

Floating arithmetic uses integer bit algorithms to preserve the native rounding,
subnormal and NaN contracts. Binary64 uses 64-bit limbs and restoring division;
it requires neither native Metal double arithmetic nor 128-bit integer shader
operations. Explicit conversions cover all 100 source/destination numeric type
pairs with the native checked conversion and ties-to-even rounding semantics.
The typed shader profile requires the pinned MSL 2.3 toolchain. Previously
qualified binary32 shaders retain their existing profile.

## Composition and control

A typed numerical program can combine maps, conversions, matrix products,
reductions, reduced scalars feeding later maps, and scalar or tensor conditionals.
Helpers inline into a bounded program. Intermediate numerical values remain in
owned device buffers. Only the final scalar result or a conditional predicate
requires readback; numerical operations execute on the GPU. Explicit source
transfers govern final tensor access from CPU code.

A conditional schedules only the selected arm. Parameter extent contracts,
helper preconditions, local shape checks and numerical faults in an unselected
arm do not execute. Selected arms can return different tensor extents with the
same declared rank. Boolean comparisons and lazy Boolean expressions are
internal control values; public GPU task entries retain numeric scalar/tensor
interfaces. Recursive helpers, managed values and arbitrary GPU closures remain
outside this numerical source contract.

The compiler emits deduplicated typed shaders and a forward-only instruction
program. Instructions include map/reduce/matmul stages, branch/jump/alias,
complete-shape equality, axis extent checks and matrix compatibility. Small
maps bind projected resources directly. More than 29 contributing buffers use
Tier 2 argument buffers, including large joins of resident reduction scalars.
Logical source types remain part of the kernel identity even when packed storage
uses the same physical Metal word type.

## Ownership and failures

The runtime validates and copies the instruction program, capture schemas,
source indices, raw scalar words and retained buffer owners before scheduling.
Capture spans may be changed or released immediately after the call. Argument
storage, physical buffer padding, transient reduction levels and command
resources count against their corresponding budgets. Source values are released
after their last executed or skipped use.

A submitted command retains its inputs and unpublished result until completion
acknowledgement. Cancellation and deadlines wait for acknowledgement and prevent
successful observation. Device errors, checked arithmetic, shape failures and
resource exhaustion retain distinct task failures. Malformed programs reject
backward jumps, wrong kernel kinds, invalid indices and mismatched capture
schemas. Packed 8/16-bit buffers initialize complete output words; empty maps
still execute uniform scalar validation using a correctly sized owned backing.

The CPU compiler emits only reachable CPU worker exports in a mixed program.
GPU-only helpers do not consume CPU stack budgets. Original export ordinals are
preserved when unused slots are omitted, including mixed CPU/GPU applications.

## Qualification

The implementation is covered by the typed source suites in
[`native/metal-source`](../../native/metal-source): `NumericSourceTests.py`,
`NumericReductionTests.py`, `NumericMatmulTests.py`, `NumericScalarTests.py`,
`NumericConversionTests.py`, `NumericTier2Tests.py`, `NumericProgramTests.py` and
`NumericMatrixProgramTests.py` and `NumericMatrixControlTests.py`. Together they contain 630 source cases. The
compiler runs these with normal and address/undefined-sanitized builds. Device
suites exercise debug, optimized, address/undefined-sanitized and thread-sanitized
host programs with one, two and four workers. Debug executes the complete case
inventory. Additional modes retain all numeric types, with representative
conversion pairs, graph variants and empty matrix storage layouts. Independent
numerical proofs exercise all conversion pairs in each numerical proof mode.
Sanitizers instrument host code.

[`NumericTests.cpp`](../../native/metal/NumericTests.cpp) additionally injects
allocation/device failures, cancellation and deadlines during owned map programs
and multilevel resident reductions, checks argument and intermediate byte
budgets, validates malformed interfaces and verifies resource counters return
to baseline. Independent APInt/APFloat proofs qualify checked integer arithmetic,
binary64 and all conversion pairs. Installed acceptance builds all ten types in
four host modes from a relocated compiler and reruns moved applications after
removing the compiler bundle.

`NumericArtifactTests.py` audits actual debug SPIR-V from all nine application
suites for floating arithmetic, native binary64 types and 128-bit integer types.
Run it after the debug application suites with `--root` pointing to the source
build output directory and `--disassembler` to the pinned `spirv-dis`.

The [completion checklist](native-gpu-completion.md) and
[capability matrix](native-gpu-capabilities.md) track release qualification.
Custom shaders, external GPU libraries, performance optimization, CUDA,
distributed execution and public native embedding remain separate work.

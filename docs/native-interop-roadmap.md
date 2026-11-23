# Native C/C++ integration roadmap

C/C++ integration is a separate roadmap from the main native language release. It remains a project goal and is not required to finish the main roadmap. Existing LLVM output and private test interfaces establish an implementation foundation; they are not a supported public library ABI.

## Scope

Support embedding compiled Kalandra in native applications and calling explicitly declared native libraries. Keep object/library output, public ABI design, foreign imports and host runtime lifecycle in this roadmap.

## Public C calls and faults

An `export "C" fn` returns a 32-bit status by default. The caller supplies a final writable output pointer for a value result. Only success publishes a value, so the result can use every bit pattern. A failed call preserves the previous output and a subsequent call starts independently. Passing a null result pointer returns an invalid-argument status before evaluation. A unit result requires no pointer. The explicit spelling `export "C" checked fn` has the same ABI.

An `export "C" direct fn` exposes the declared scalar result directly. The compiler must prove that its native body cannot fault. An inconclusive proof is a build error.

Both declarations have the same Kalandra call syntax and result type inside Kalandra. The scalar implementation uses the compiler’s existing native value-and-fault representation. Bodies for which no recoverable entry can be generated are rejected. Opaque handles, buffer ownership, callbacks and runtime-backed exports need their own lifetime and qualification work.

## Milestones

1. **Object and library output.** Define explicit export selection and supported object, static-library and shared-library builds. Specify symbol naming, visibility, target/platform compatibility and linking requirements.
2. **Public interface contract.** Define C-compatible scalar, record and collection interfaces; generated headers; borrowed versus owned storage; errors; and ABI versioning. Do not expose private LLVM structs or scheduler layouts as the public contract.
3. **Host runtime lifecycle.** Specify initialization, shutdown, invocation isolation, CPU/GPU resources, thread use and synchronization. Define asynchronous completion, cancellation and ownership through final completion.
4. **Foreign calls.** Design explicit native declarations and safe wrappers for C APIs usable from C++. State which operations are unsafe, how external errors are represented, and how borrowed/retained memory is handled.
5. **Qualification and delivery.** Build independent C and C++ consumers, test lifetime/error/cancellation behavior under sanitizers, verify supported link combinations, and document a complete integration example.

Exporting a library does not automatically implement foreign imports or callbacks. Each boundary needs its own ownership and error rules. Initial platform scope and the public ABI must be reviewed before implementation; no release date or stable layout is implied here.

Progress on this roadmap is tracked separately from the [main native roadmap](roadmap.md#native-implementation-status).

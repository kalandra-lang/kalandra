# Native C/C++ integration roadmap

C/C++ integration is a separate roadmap from the main native language release. It remains a project goal and is not required to finish the main roadmap. Existing LLVM output and private test interfaces establish an implementation foundation; they are not a supported public library ABI.

## Scope

Support embedding compiled Kalandra in native applications and calling explicitly declared native libraries. Keep object/library output, public ABI design, foreign imports and host runtime lifecycle in this roadmap.

## Milestones

1. **Object and library output.** Define explicit export selection and supported object, static-library and shared-library builds. Specify symbol naming, visibility, target/platform compatibility and linking requirements.
2. **Public interface contract.** Define C-compatible scalar, record and collection interfaces; generated headers; borrowed versus owned storage; errors; and ABI versioning. Do not expose private LLVM structs or scheduler layouts as the public contract.
3. **Host runtime lifecycle.** Specify initialization, shutdown, invocation isolation, CPU/GPU resources, thread use and synchronization. Define asynchronous completion, cancellation and ownership through final completion.
4. **Foreign calls.** Design explicit native declarations and safe wrappers for C APIs usable from C++. State which operations are unsafe, how external errors are represented, and how borrowed/retained memory is handled.
5. **Qualification and delivery.** Build independent C and C++ consumers, test lifetime/error/cancellation behavior under sanitizers, verify supported link combinations, and document a complete integration example.

Exporting a library does not automatically implement foreign imports or callbacks. Each boundary needs its own ownership and error rules. Initial platform scope and the public ABI must be reviewed before implementation; no release date or stable layout is implied here.

Progress on this roadmap is tracked separately from the [main native roadmap](roadmap.md#native-implementation-status).

# Native C integration

Kalandra applications import native functions from their C headers. Native applications call Kalandra through a generated C header. The [modules and native interop guide](https://kalandra.dev/docs/modules) describes both interfaces. [Issue #334](https://github.com/kalandra-lang/kalandra/issues/334) tracks the first qualification.

## Calling a native library

A package declares the header and link dependency in its manifest. Kalandra reads declarations and attributes for the destination ABI. It uses those declarations to check buffer extents, UTF-8 byte counts, nonescaping borrows and opaque handle lifetimes. Ordinary C library code is responsible for honoring its declarations. A binding author can supply a small native adapter when the upstream API expresses its contract only in prose.

Function pointers to named direct exports support capture-free callbacks. An adapter can retain a callback with copied scalar captures through a subscription with an explicit release protocol. The adapter synchronizes delivery and detachment. The runtime pins admitted handle borrows until the native call returns.

The [window example](../native/examples/window) has a Kalandra entry point and event loop. Its adapters connect to AppKit on macOS, Win32 on Windows and X11 on Linux.

## Embedding Kalandra

Build a relocatable object, static library or shared library with a matching C header. An `export "C" fn` returns a 32-bit status. The host supplies a writable output pointer for a scalar result. A failed call preserves the previous output. The next call starts with independent fault state. A unit result requires no output pointer.

An `export "C" direct fn` exposes its scalar result directly. The compiler must prove the body can run without a fault or an invocation context. Keep the library loaded until all calls and callbacks into its code have returned.

The public export ABI covers scalars. A general embedding API for asynchronous tasks, GPU runtime ownership or arbitrary Kalandra-managed objects would require additional lifecycle contracts.

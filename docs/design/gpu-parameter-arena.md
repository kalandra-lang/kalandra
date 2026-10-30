# Invocation-local GPU parameter storage

Prepared hardware float32 progressions can place their small writable argument
blocks in one invocation-owned Metal buffer. Each block has a distinct
256-byte-aligned slice, including its fault word. CPU reads and GPU argument
bindings both use the slice offset. Numerical shader code is unchanged.

The preflight pass calculates the total padded size using checked arithmetic.
Padding must fit without consuming the previous 64 KiB safety headroom. If it
does not, the program retains separate argument buffers and the same command
progression policy. Streaming, unproved and otherwise ineligible programs also
retain their existing storage path. `Options::parameterArena = false` provides
an internal qualification control.

The backing is allocated lazily on the first parameter request and charged once
to the existing byte and Metal-buffer accounting. Slice objects retain that
backing; they do not reserve or release its bytes independently. Owners remain
live through GPU acknowledgement, including cancellation, deferred faults,
device errors, concurrent calls and runtime teardown. No page is cached across
invocations, no capture is shared between calls, and no output tensor aliases
argument storage.

This changes storage allocation, not the existing estimated-work cancellation
checkpoints, command sequence or shader arithmetic. Allocation failure still
reports an error and unwinds the invocation; a failed allocation is not hidden
by retrying through another path.

The diagnostic host profile uses the system Mach clock to compare submission,
GPU execution and callback entry. Apple's [GPU timestamp documentation](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime)
defines that common clock domain. Inclusive host timers overlap and must not be
summed as exclusive wall time. Three dispatch-worker completion prototypes
waited for genuine command completion and preserved work boundaries, but did
not provide a reliable speedup; they remain benchmark-only experiments.

# Native function calls

A source-to-source call uses an internal native convention: optional aggregate result storage, environment, source arguments, invocation, cancellation token and a word-sized execution mode. Scalars return directly; aggregates use caller-provided result storage, forwarded in tail position. Void results need no result slot. Ordinary internal calls do not enter/leave a runtime call slot, publish a recursion depth, return a success bit or poll again after a successful child return.

The checked entry remains the identity stored in function values and worker metadata. An internal resolver maps that identity to a body with the matching complete lowered signature. This keeps closure environments, task transport and provenance validation compatible. Resolver catalogs are finalized after task-owned callable entries are emitted, before whole-module optimization. Matching planned entries receive adapters from the internal convention to their checked C entry; signatures and code identities are validated together. Unknown or mismatched code still fails. The environment slot is present for named functions as well as closure thunks, allowing equal-signature mutual and indirect tail calls.

## Runtime boundaries and failure

A checked runtime entry stores its argument/result packet above a C++ catch boundary. The boundary admits the executing physical stack, calls the generated body, checks completion and balances its admission. Fatal source operations record the existing fault and coordinates. Internal fatal exits raise a private native abort marker. Inlined exits in the outermost callback return a boundary failure directly, avoiding needless exception unwinding. Generated source frames have unwind tables and retained frame pointers. The numerical fault suite exposed incomplete unwinding without those frame pointers, even though scalar recursion had passed. The callback returns a completion flag; the runtime also catches the private marker from deeper calls. A stopped callback stays stopped even if a host resets its cancellation token before final readiness is checked. Cancellation remains a stopped execution rather than a new fault. Source application errors continue using their existing typed representation.

Generated source frames have no C++ owners requiring destructors. The invocation ledger owns live backing storage, including failure paths, until task acknowledgement and settlement. Aggregate scratch packets of the same type share a slot within a function: each result is loaded into SSA immediately after its synchronous call, and the private ABI cannot retain the packet address. Normal releases and existing ownership rules remain in effect. This private control transfer does not add public exceptions or change the immutable source model.

## Recursion and physical stacks

There is no fixed source call-count limit. Eligible forwarded tail-return positions use LLVM `musttail`, including mutual calls, function values, aggregate results and void results. Register-only Darwin ARM64 signatures use `tailcc`, including differing parameter lists. Large Darwin ARM64 argument packets retain caller-pop native calls because the pinned backend miscompiles their callee-pop form in unoptimized builds; matching signatures can still use `musttail`. Other targets qualify their `tailcc` lowering in hosted tests. Aggregate result pointers are explicit; automatic conversion of a large return to `sret` would violate LLVM’s tail-call contract. Parameter replacement follows the ordinary native calling convention. A function with frame-local allocations conservatively retains ordinary calls because those allocations may escape to its callee. Runtime and foreign entry conventions retain their checked adapters.

Non-tail calls consume physical stack. macOS, Linux and Windows runtime entries discover the executing thread's bounds and reserve 128 KiB for fault handling and unwinding. Migrating tasks reacquire bounds on their next synchronous entry. Generated calls that can grow source frames check the stack pointer; fixed frames may reuse a dominating successful check. Resource exhaustion produces code 6, `StackExhaustionFault`, or the existing native task fault at a worker boundary. Finite stack memory remains a resource limit; it is not an arbitrary recursion counter.

On Win64, private scalar tail-call components use equally sized argument packets, including unused trailing slots when necessary. This avoids the pinned LLVM backend's unsupported negative tail-return adjustment under Win64 unwind tables. Private packet signatures are protected against later argument elimination; checked public C signatures remain unchanged. Aggregate/vector parameter paths use the C convention conservatively.

TSan builds reserve a 1 KiB diagnostic area in functions with growing native calls. Tiny frames otherwise permit enough active frames to exhaust TSan's trace restart buffer before physical stack admission. This instrumentation lets the existing stack guard run first; it adds no recursion counter and leaves race checks, function tracing and constant-space tail workers enabled.

Task-planned continuations and recursive ownership/transport helpers retain their separate checked runtime entries. They also use physical stack admission rather than a 128-entry cap. An active-path traversal guard rejects forged cyclic transport graphs with runtime fault 3, while accepting shared DAG children. Its thread-local chain is scoped to synchronous copies and popped on every return. No source value captures the chain. This change does not promise constant-space execution for every fallible, task-planned or ownership traversal path.

## Cancellation and optimization

On Darwin ARM64 (excluding ARM64e), scheduler-owned invocations can select the
hybrid native boundary automatically. The v4 checked entry captures a cleanup
return point, installs its state in reserved x28, and selects the clear worker
for pure computation. Helper returns still check status and the cancellation
token. Arithmetic and physical-stack checks remain. Source faults still use
the private C++ abort marker; the direct jump is only for cancellation.

Only compiler-marked resource-free source frames and their callback wrappers
occupy `__TEXT,__kalandra`. Its linker-defined limits include final machine-code
prologues/epilogues; runtime helpers and task-controller frames are excluded.
The signal handler examines PC before reading x28, uses lock-free loads, and
redirects normal signal return to a small assembly jump bridge. It never frees,
allocates, unwinds C++ frames or takes a scheduler lock. Landing returns a
stopped callback and balances admission. Scheduler settlement then releases
the invocation ledger under the existing descendant/external-borrow rules.

Every callback entry establishes its own boundary and reinstalls x28, even
when a foreign helper has borrowed that callee-saved register. Cancellation
therefore stops above C++ owners in the helper below the callback. Unbound and
cooperative callbacks also install a valid state with no interrupt token, so
a late signal cannot read a previous invocation's dead boundary. Compiler-owned
native code cannot retain stack argument/result packets after synchronous return.
Runtime/source composition uses separate object files without cross-runtime LTO;
foreign C++ frames must never be inlined into the certified source section.
Final call-site barriers prevent task/ownership helper bodies from being inlined
into source frames by downstream optimization, while preserving optimization
among task helpers outside that section and ordinary fast-ABI helper inlining.

Each capable task core owns an interrupt service that sleeps until a cancelled
execution lease exists, then retries targeted pthread interrupts at 100 us
intervals. The core mutex protects physical runner identities and record reuse.
Handlers receive no task-slot pointers: they inspect the current native boundary
and its current token. Execution leases finish before records can be recycled;
migrating invocations reacquire both physical stack bounds and interrupt capability.
Retries are not a real-time latency guarantee and cannot interrupt an arbitrary
blocking foreign helper. A submitted GPU borrow retains storage until completion.

`SIGUSR2` is private process-wide runtime infrastructure on this path. Initialization
claims it only if its disposition is default, and never replaces another owner.
After acquisition it remains owned through process exit; runtime modules cannot
be unloaded, nor can foreign code replace its disposition or block it inside an
active native lease. A worker already blocking it uses cooperative cancellation.
Existing signal owners, unsupported platforms and ARM64e also use that fallback.
This is a private standalone-runtime contract, not a new public embedding ABI.

ThreadSanitizer builds deliberately use cooperative cancellation. The sanitizer
defers asynchronous signal callbacks and supplies a copied context: editing it
does not redirect the live machine, and its saved boundary pointer can outlive
the original frame. This was exposed by a forced pure-compute cancellation
timeout; it is retained as a failed qualification attempt, not an async pass.
See [LLVM's interceptor implementation](https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/tsan/rtl/tsan_interceptors_posix.cpp).
Debug, optimized and ASan/UBSan builds qualify actual redirects separately.

The compiler specializes the internal call graph for clear, token-bound and observed execution. Clear progress checkpoints require no runtime state read. Token-bound checkpoints load the token with acquire ordering. Observed checkpoints retain the runtime probe/fault logic. Checked allocation, validation and other runtime-helper boundaries still inspect their result status. A bottom-up bounded-total proof permits omitting redundant progress checks only for finite, memory-free call DAGs with bounded work; recursive cycles and runtime effects cannot satisfy that proof.

Normal LLVM optimization runs on a private module clone. A control-flow transformation keeps pure, call-free base-case regions outside a non-tail recursive worker's physical frame. It handles branching return regions and preserves arithmetic order; it does not expand recursion to a fixed depth. Thin wrappers remain eligible for inlining and transfer to the nonleaf worker with `musttail`. Optimized functions must still satisfy the conservative 16 KiB stack model, including outgoing arguments and fixed allocations. Intrinsic metadata has no physical argument storage. If optimization exceeds the budget, the checked original module is retained. Final physical frame boundaries are sealed before downstream code generation. Task-controller frames retain their separately defined downstream optimization policy and trusted descriptor identities. Leaf-region cloning follows source block order so compiler instrumentation does not change emitted artifacts. After inlining, a tagged physical guard is removed only when its accepted continuation contains no frame-growing calls or allocations. Pure leaf loops and terminal fault paths can then remain outside recursive worker frames; surviving source calls keep their guards.

On AArch64, dynamic signed 64-bit remainder uses `SDIV`/`MSUB` after the source zero-divisor check. This directly preserves the language's zero remainder for `INT_MIN % -1`, avoiding a replacement-divisor select in every loop iteration. Other targets keep the portable checked lowering. This relies on [Arm's specified division overflow behavior](https://developer.arm.com/-/media/Files/pdf/graphics-and-multimedia/ARMv8_InstructionSetOverview.pdf); LLVM's ordinary [`srem` has undefined overflow behavior](https://www.llvm.org/docs/LangRef.html#srem-instruction). Floating-point arithmetic order and contraction policy are unchanged.

No fast-math, floating reassociation, source algorithm substitution or unchecked arithmetic mode is introduced. Strict numerical tests remain independent of benchmark checksums.

## Tests and historical evidence

`native-call-foundation-source` exercises million-step self, mutual and runtime-selected tail calls, captured closures, aggregate values, non-tail floating recurrence, arithmetic faults, cancellation and small physical stacks in debug, optimized, ASan/UBSan and TSan builds where supported. Existing numeric, closure, worker and ownership suites cover the surrounding language behavior. `native-checked-inlining` checks the internal convention and conservative fallbacks structurally. The scalar suite additionally checks every narrow integer and Bool value at a raw C boundary, immediately promoting the return rather than truncating it back to its source width. Checked C entries carry explicit signed/unsigned return-extension attributes; the internal convention retains its own signless value representation.

The preceding implementation and its 128-slot behavior are recorded in the historical [recursive status report](../../benchmarks/results/recursive-status-2026-10-24.md), [recursive register report](../../benchmarks/results/recursive-register-2026-10-24.md) and their linked qualification artifacts. Those historical poll counts, logical-depth failures and expansion phases describe the older compiler; they are not the current execution contract. The [final repeated-process source suite](../../benchmarks/results/call-foundation-2026-10-25.md) meets the Rust/C++ 10% target for all 18 measured unbound cases. This is a scoped result; cancellation-bound regressions and the remaining task/ownership work are retained in that report.

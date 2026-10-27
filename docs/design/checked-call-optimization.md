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
token. Arithmetic and physical-stack checks remain. Source call chains use the private C++ abort marker for faults; an outermost
callback can return failure directly when no source caller remains to unwind.
The direct jump is only for cancellation.

Only compiler-marked resource-free source frames and their callback wrappers
occupy `__TEXT,__kalandra`. Its linker-defined limits include final machine-code
prologues/epilogues; runtime helpers and task-controller frames are excluded.
The signal handler examines PC before reading x28, uses lock-free loads, and
redirects normal signal return to a small assembly jump bridge. It never frees,
allocates, unwinds C++ frames or takes a scheduler lock. Landing returns a
stopped callback and balances admission. Scheduler settlement then releases
the invocation ledger under the existing descendant/external-borrow rules.

Every v4 callback entry establishes its own boundary and reinstalls x28, even
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

## Native subgraphs inside CPU tasks

Ordinary synchronous helpers inside a task now use the same native lowering as
standalone CPU functions. The continuation planner imports their complete pure
call graph, including recursive helpers and function values, into a checked
native kernel. A spawned helper retains its outer task descriptor, parameter
transport, execution lease and result publication; its source calls do not need
a heap continuation frame at every recursive step. Expected-error/control-flow
callables and coordinating functions retain their checked continuation paths.
If a native subgraph exceeds physical-frame admission, compilation retries the
existing checked continuation representation instead of rejecting that source.

After graph inlining, the compiler can replace a small, acyclic closure
environment with local storage when every address use is a local load/store or
address calculation. Scalar replacement normally removes that storage entirely.
Returns, publication, pointer identity, foreign/other calls, loop allocations and
uncertain uses retain managed environments. The proof does not let a tail call
borrow the local address, so existing guaranteed tail transfers remain valid.

Eligible direct self-tail calls with rank-one numeric array/tensor state and a
scalar numeric/Bool result are lowered to a structured loop before bufferization.
The current form recognizes a single conditional with one direct recursive arm
and one terminal arm; other recursion retains its existing lowering. Terminal
work precedes the loop condition, while a continuing iteration produces the next
arguments. This preserves simultaneous argument replacement, immutable aliases
and changing-length views while allowing intermediate arrays to be released
between iterations. Tensor parameters carry an explicit read-only contract.
A simple parameter/literal comparison also permits the terminal case to bypass
loop ownership conversion. No source condition with allocations or other work
is duplicated by that shortcut. Native stack admission can fall back to the
previous lowering when the loop representation would exceed its budget.

The invocation allocator can retain one fully released numeric buffer of at most
256 payload bytes for exact-size reuse. It requires the original embedded owner,
no managed record/dependencies and an exclusive backing reference. Retained
readers and transported values are never recycled. Reuse still performs logical
allocation fault injection and reservation; counters and cumulative task byte
charges remain balanced through settlement. Constant live buffers therefore do
not imply an unlimited allocation quota under a fixed task byte budget.

`native-task-native-helpers` checks native task execution with 1/2/4 workers,
million-step closure recursion, immutable and aliased arrays, changing-length
views, every allocation fault, exact/rejected budgets and cancellation/deadlines.
Separate harnesses check bounded live buffers through 100,000 array steps,
divergence until cancellation, actual native redirects where supported, and
binary32/binary64 bit patterns including signed zeros, subnormals and NaN payloads.
All ten numeric types are exercised; software floating arithmetic is checked
separately. The [measurement report](../../benchmarks/results/task-native-optimization-2026-10-26.md)
records improvements, unsuccessful prototypes and remaining gaps.

## Tests and historical evidence

`native-call-foundation-source` exercises million-step self, mutual and runtime-selected tail calls, captured closures, aggregate values, non-tail floating recurrence, arithmetic faults, cancellation and small physical stacks in debug, optimized, ASan/UBSan and TSan builds where supported. Existing numeric, closure, worker and ownership suites cover the surrounding language behavior. `native-checked-inlining` checks the internal convention and conservative fallbacks structurally. The scalar suite additionally checks every narrow integer and Bool value at a raw C boundary, immediately promoting the return rather than truncating it back to its source width. Checked C entries carry explicit signed/unsigned return-extension attributes; the internal convention retains its own signless value representation.

The preceding implementation and its 128-slot behavior are recorded in the historical [recursive status report](../../benchmarks/results/recursive-status-2026-10-24.md), [recursive register report](../../benchmarks/results/recursive-register-2026-10-24.md) and their linked qualification artifacts. Those historical poll counts, logical-depth failures and expansion phases describe the older compiler; they are not the current execution contract. The [final repeated-process source suite](../../benchmarks/results/call-foundation-2026-10-25.md) meets the Rust/C++ 10% target for all 18 measured unbound cases. This is a scoped result; cancellation-bound regressions and the remaining task/ownership work are retained in that report.

## Range proofs, counted ownership and bounded host entries

Integer guard folding uses fresh LLVM value-range and dominator analyses. It
intersects facts already established on strict dominators and reevaluates
immutable integer expressions at the use. Future assumptions, sibling branches,
changing PHIs and memory reloads cannot justify a fold. Modular ranges preserve
wrapping behavior. Only a proved comparison is replaced; arithmetic failure
locations and evaluation order remain intact.

An eligible array-state self-tail call with an int64 parameter tested against
zero and decremented by one has a counted lowering for positive counts. Its first
iteration is peeled so a newly produced owned array enters the loop rather than
a borrowed input. Negative counts retain the general while lowering and checked
underflow. Other conditions retain that lowering too. Small static index/Bool
workspaces introduced by deallocation helpers can use native stack storage only
when all uses are nonescaping helper calls, accesses, casts or releases. They
are compiler control storage, not source values. Native frame admission still
falls back to the earlier lowering if necessary.

Promoting a local environment also removes its obsolete allocation-status
branch only in clear workers, with an exact context-field and slow-poll match.
Watched/cooperative workers retain their checks. Other effects or uncertain
pointer uses prevent the rewrite.

A second host-entry path handles scalar callbacks whose optimized computation is
call-free, allocation-free and provably bounded. It accepts only clear contexts or contexts with a bound
token at depth zero. It polls a bound token with acquire ordering
before and after work. Watched, faulted, settled, nested and already armed
contexts retain the full v4 runtime entry. Source faults store the same first
fault and return failure. There is no asynchronous abandonment of C++ frames in
this path; its code is outside the admitted native-source section.

The proof permits an acyclic body or one natural loop with one backedge, no
nested/irreducible cycles and a calculable exit count that dominates the latch.
The computation budget is 256 LLVM instructions, conservatively counting every
block on a path and every loop block per iteration. Admission has a separate
bounded projection budget of 128 nodes and constant entry/exit bookkeeping.
Projected counts read only initialized, unchanged scalar fields in the private
argument packet. Projection uses modular arithmetic without speculative
no-overflow assumptions. Unknown calls, allocations, atomics, volatile accesses,
unsupported intrinsics and wide division/remainder reject this path. The normal
interruptible entry remains available for all unproved work.

An empty invocation ledger now settles inline; owned backing, cached buffers and
nonzero environment/recursive budget reservations retain the full resource
cleanup. At that checkpoint construction remained a runtime entry and private
control flags occupied an aligned word. Compiler-accessed field offsets and the versioned C runtime
interfaces are unchanged; consumers rebuild with the matching private C++ header.

On production Darwin ARM64, native entry saves its recovery checkpoint in the
entry trampoline, including every ABI callee-saved general/FP register. A source
interrupt restores that checkpoint and returns failure through normal runtime
cleanup. Cooperative entry retains its smaller frame. ASan keeps its supported
sigsetjmp/siglongjmp recovery instead. Register-sentinel, nested-cleanup and late
signal tests qualify both the recovery and reuse paths.


Eligible bounded callbacks also have a guarded small-integer version. Up to eight
immutable int64 packet fields must lie in [0,65535] at entry; other values retain
the original implementation. This is a compiler policy, not a source type or
argument-count limit. LLVM range analysis can then narrow division/remainder to
a proved precision. Signed INT32_MIN / -1 and INT32_MIN % -1 cannot use a naive
signed int32 operation. Unsigned operations use the required bit precision, which
the target lowers through native hardware widths; source int64 values and
checked failures retain their semantics.

A nonnegative remainder recurrence needs an inductive proof: initial values lie
in [0,modulus), and its numerator remains nonnegative for every value in that
interval. Negative initial values, negative transforms, uncertain arithmetic and
wrapping ranges cannot establish that invariant. Source and sanitizer oracles
cover those rejection cases alongside wide initial values and negative divisors.

The bounded computation receives immutable value arguments through SSA, rather
than reloading their private packet fields after acquire cancellation polls.
Forwarding requires a dominating, unambiguous initializer with matching scalar
type and no overlapping packet write. The checked v4 bridge remains available.

Invocation mutation has one exclusive writer. Atomic live-count snapshots remain
available to diagnostics, using atomic loads/stores without an inter-writer RMW
transaction. They do not publish ledger or payload memory. Shared backing
reference counts retain their acquire/release protocol. Runtime helpers reuse
poll-mode health information to avoid repeated status/settlement/observer reads;
they retain acquire cancellation loads, fault priority and the full observed
poll path. Fault, settlement and observation always enter mode 2.


## Lazy invocation lifecycle and private layout revision

A fresh invocation initializes only its cancellation pointer, control tag/poll
mode and atomic live-count diagnostic. Fault/depth/stack state is initialized
on first managed entry or fault; the inline resource ledger is constructed on
first resource, observer or expected-error use. Both use tagged initialization,
not a heap allocation or a process-global current invocation. Explicit value
initialization (`Invocation{}`) also leaves deferred storage untouched.

The `ControlReady` and `Materialized` bits describe object lifetime, not language
execution modes. Materializing a ledger preserves any earlier fault, token,
observer and native-interruption capability. Empty settlement never touches an
inactive union member. Its control-only fast path remains idempotent. The live
counter stays an always-live atomic because diagnostic readers may sample it
concurrently with the owner's first allocation. Ledger mutation remains exclusive;
shared backing retains/releases retain their existing atomic synchronization.

Bounded entries inspect the control tag before reading a stored depth. Cold
entries have logical depth/status zero. Fault recording goes through the runtime
on the exceptional path so it cannot inspect an uninitialized fault slot. Tokens
are still checked with acquire ordering before and after admitted work. Generated
helper status checks recognize healthy armed mode 3 as well as mode 1, including
a null-token case; faults, observation and settlement still select mode 2 and the
full observed path. Returning from an armed boundary after explicit token
unbinding normalizes the context to unbound mode, so a following bounded entry
cannot inherit a token-bound mode with a null pointer.

The private compiler/runtime field layout changed. Rebuild C++ header consumers
and generated modules together. New modules retain a data relocation against
`kalandra_invocation_layout_v2`, so linking them to a runtime without that layout
fails without adding a runtime entry call. This is not a stable public C++ ABI or
backward-compatibility promise for older generated modules.

`KALANDRA_TEST_FAIL_ALLOC_AT` is immutable process test configuration, read once
at the first resource request. Allocation attempts and their failure counters
remain per invocation. `failAllocationAt(n)` overrides the process default for
that invocation, including zero to disable it. Changing the environment after
configuration has been read is unsupported; use the explicit per-invocation
probe for in-process changes. Scalar work does not resolve this configuration
or consume an allocation attempt.

Poisoned-storage tests cover both construction forms, first-fault preservation,
late observer/resource materialization, cancellation and repeated settlement.
The exact numerical corpus also begins in poisoned storage across all four
source build variants. [Measurements and qualification](../../benchmarks/results/invocation-lifecycle-2026-10-26.md)
retain the zero-round controls rather than replacing them with reused contexts.

### Omit task entry machinery for proven unbound code

Ordinary standalone execution starts with no cancellation token. This is distinct
from task execution and from OS process termination: the ability to kill an
executable does not require source cancellation polls. A sequential function
inside a cancellable task still inherits that task's execution contract.

After composing and optimizing the native call graph, the compiler can emit an
unbound entry alongside the general checked entry. A single execution-mode guard
selects it only for a healthy, unobserved context without a bound token. The
compiler specializes the callback to that mode and transitively inlines the
closed clear-body graph. Admission, native interruption setup, cancellation
polls and boundary depth bookkeeping are omitted from
the proven path. There is no per-call cancellation opt-out flag or token removal
inside a running cancellable task.

The proof rejects allocation, foreign and indirect calls, task operations,
remaining physical stack guards, local stack allocation, atomic/volatile
accesses and unsupported intrinsics. Packet reads must have dominating host
initialization; the callback cannot overwrite its context slot or write outside
its private result packet. Checked arithmetic remains intact: a fault may record
its original code/location and return boundary failure locally, with no execution
continuing after that fault. Unknown work uses the existing managed entry.
Unlike cancellable bounded entry, an unbound source loop needs no cancellation
latency budget. Token-bound, observed, faulted and settled contexts retain their
existing paths, including native stack safety and cleanup.

Existing bounded/range specializations run before the unbound fallback so short
loops retain their narrow arithmetic. Their side-effect-free argument guard
runs before reading invocation control/depth; packets outside that region can
reach the unbound eligibility guard without preparing managed control state.

### Separate checked entry frames from pure value returns

After the closed unbound graph proof, [LeafEntry.h](../../native/lib/LeafEntry.h)
can outline a checked scalar host entry's original body into an internal worker.
A call-free prefix can return its value without the packet frame. Before atomic,
volatile or external effects, foreign/resource calls or dynamic stack allocation,
it transfers to that worker with the original arguments. Private scratch writes
are eliminated by SROA; a surviving alloca or absence of a value-return path rejects
the transformation. Cancellation boundaries, task/native pure frames and special
code sections are excluded.

The required tail transfer preserves the original return and parameter ABI
attributes, including Boolean return extension. The original checked worker
retains its frame pointer and all admission/fault/cleanup behavior. The wrapper
can omit its frame pointer; target code generation still owns actual register
and stack requirements. No public callable ABI or cancellation opt-out is added.

The [isolated study](../../benchmarks/results/entry-costs-2026-10-27.md) records
both the zero-round gain and short-case regression, the ABI defect found by broad
source qualification, and the rejected compact backing experiment. At that stage, the scalar
context/entry ABI and array allocation/ownership parity targets remained open.
The ordinary ABI below addresses the recorded eight-case corpus.

The unbound host marker is installed before CFG simplification, which may delete
the originating basic block. The proof unit checks this optimizer-callback order.

### Prove an ordinary entry's resource and fault ABI

[OrdinaryEntry.h](../../native/lib/OrdinaryEntry.h) adds a private scalar
`_ordinary_v1` companion when the final closed graph has no task/foreign effects,
borrowed pointers, escaping resources, atomic/volatile operations or growing
source calls. The original checked entry remains available. The CLI selects the
companion for an eligible parameterless scalar main; emitted library IR retains
the original entries. Mixed CPU/Metal programs and unsupported graphs continue
through their existing paths.

A companion receives scalar arguments and a 24-byte fault output, without an
Invocation. Integer results with only their high bit set (true for Bool), and
any floating NaN, require consulting that output. Failure publishes the original
first code/line/column and returns that integer pattern or a quiet NaN. Successful
ambiguous values publish code zero and retain their exact value bits; other
successes leave the output untouched. Callers use `ordinaryFaulted` rather than
reading an uninitialized output. No valid numeric value is reserved. ABI extension
attributes survive; result range/NaN refinements from the checked failure
representation do not transfer. All ten numeric types and Bool are covered.

[OrdinaryPool.h](../../native/lib/OrdinaryPool.h) permits four aligned private
slots sized for the largest constant numeric allocation in an entry (at least
128 bytes per slot). Storage is capped at 8 KiB, and the complete entry must
pass the existing conservative 16 KiB native frame check, including SSA values,
fixed allocations and outgoing arguments. The final graph must prove all memory
private apart from the fault output. Unknown effects or pointer escape reject the entire companion.
If slots are exhausted or a release cannot be represented, the entry replays the
original checked graph with original scalar arguments in a fresh real Invocation.
Stack pointers never enter replay. Allocation-failure injection selects that heap
path at the first logical allocation, preserving failure order and location.
The original allocator, ownership ledger and Invocation layout are unchanged.

[OrdinaryBaseCase.h](../../native/lib/OrdinaryBaseCase.h) can version an ordinary
scalar entry when substituting zero for an integer argument removes every loop
and yields a body of at most 32 instructions and at most one third of the original
size. Specialization evaluates the whole body, retaining checks before the base
return. Other inputs tail-transfer to the original ordinary worker with matching
return/parameter attributes. Buffered entries are excluded. There are no workload
names or round-count ranges in this transformation.

Task/cancellable entries retain cancellation, physical-stack admission, ownership
and settlement. This private ABI accepts no cancellation token or caller resources;
it is compiler-selected ordinary execution, not an opt-out within a task.


### Ordinary countdown and IEEE value proofs

[OrdinaryLoops.h](../../native/lib/OrdinaryLoops.h) recognizes a two-block
countdown whose initial value is an entry argument, whose only backedge subtracts
exactly one, and whose switch exits at zero or reports signed minimum overflow.
A nonnegative initial count cannot reach signed minimum: zero exits before the
next subtraction. A guarded version removes only that unreachable minimum case.
Negative counts retain the original ordinary worker, including first-fault
location and ABI attributes. Task and buffered entries are excluded.

A separate conservative value proof removes unordered/ordered NaN classification
only when both operands cannot be NaN. It follows constants, integer-to-float
conversion, selected casts, and inductive phi/select graphs. A product with a
finite nonzero constant preserves absence of NaN; addition/subtraction requires
one finite operand and one non-NaN operand. Division by a finite nonzero
constant also preserves absence of NaN. Finite multiplication is proven only
for a finite input and a constant with magnitude at most one. Unknown floating
arguments, loads and unsupported operations remain unknown. Zero times infinity
and explicit NaN origins reject the proof. No fast-math flags, reassociation,
contraction, infinity exclusion or numeric representation restrictions are added.

The [broader ordinary study](../../benchmarks/results/broad-ordinary-2026-10-27.md)
records separate results for short scalar operations and arrays up to 128
elements, including remaining misses. Four buffer slots are necessary for some
nested immutable borrows: a three-slot prototype exhausted capacity on the second
map and replayed through the heap. That experiment is retained as a rejected
control rather than presented as a sustained local-buffer speedup.

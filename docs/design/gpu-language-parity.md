# CPU/device language parity qualification

Issue #279 is the completion boundary. PR #281's scalar checkpoints are a
foundation, not completion. Device execution means source predicates, arithmetic,
traversal and value construction execute on the selected GPU. Host scheduling,
resource admission and transfers must not masquerade as device computation.

## Implementation sequence

1. Extend checkpoint frames to plain-data records/enums: canonical field layout,
   zeroed inactive payloads, value copies, field projection and checked matching.
   Include nested values and nonrecursive helpers. Public aggregate inputs/results
   need a typed transport ABI; local-only aggregate support is an intermediate
   milestone, not full qualification.
2. Add invocation-owned device storage with stable handles, checked lengths and
   offsets, explicit read-only captures and exclusive local writes. Dynamic growth
   must checkpoint before resource admission, then resume device work. Define
   reclamation so repeated allocation in loops cannot accumulate unreachable
   backing indefinitely. No arbitrary fixed array capacity may replace the
   language's allocation/resource contract. Bounds and allocation errors discard
   partial results and preserve existing fault/cancellation ordering.
3. Lower array traversal, indexing, local/disjoint stores and mutating helper
   contracts onto that storage. Preserve alias identity for assignment; explicit
   `copy` owns independent storage. Reuse frontend sharing/exclusive-access proofs
   and reject overlapping writes rather than silently copying or weakening them.
4. Implement ordered eager `for`/`while` collection and `yield`, `generate`, array
   `map`, `copy`, `sorted` and `sort`. Include dynamic sizes, nested/filter/empty
   results, early exits, captures and distinct invocation state. Sequential
   invocation-local append is distinct from parallel compaction. Library algorithms
   must themselves checkpoint; one long opaque sort must not bypass cancellation.
5. Complete aggregate transport and owned aggregate/tensor state; specialize
   eligible function values and captures. Specify recursion independently: a
   checked invocation-owned call stack must respect memory budgets and checkpoint
   cancellation, not introduce an arbitrary source-call depth limit. Reject host
   services explicitly. Recursive owned types require an actual storage/lifetime
   representation, not host pointers embedded in device values.
6. Publish the CPU/device capability matrix and runnable user documentation;
   verify editor diagnostics and examples. Capabilities distinguish device
   execution, host orchestration, explicit target limits and unsupported features.
7. Qualify the complete corpus on physical Metal/Vulkan/CUDA, retain unfavorable
   benchmark results including transfer/allocation costs, and audit current-head
   build/package/relocation/sanitizer/cross-platform checks. Windows GPU hardware
   remains explicitly unqualified when unavailable.

## Required evidence

Each issue checkbox needs implementation and independent CPU/device oracle cases.
Cover nested traversal; assignment versus deep copy; exclusive writes and shared
reads; numerical policies; empty/filter ordering; enum layouts and matching;
owned captures; function specialization; recursive resource exhaustion; checked
bounds/overflow/zero steps/allocation errors; concurrent invocations; cancellation
before acknowledgement versus an already recorded fault; and zero retained
resources after completion, failure and cancellation. A passing compile-only job
does not prove execution. A local macOS 27 run does not prove hosted macOS 26.

Existing scalar qualification evidence is retained under ignored
`build/qualification-evidence/`. Its four cancelled hosted macOS jobs were runner
capacity failures, not executed tests. Retry that gate when capacity is available;
it must not prevent independent progress on the remaining language requirements.

No change to shared workgroup memory, atomics, barriers or launch geometry is
implied here; those kernel-authoring APIs belong to #265. Production numerical
implementations remain independent of vendor numerical libraries. Remote testing
uses only authorized project-local dependencies and removes its copies after
evidence is saved.

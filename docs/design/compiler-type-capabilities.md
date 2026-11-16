# Compiler type facts and capability checks

The compiler derives shared facts from its existing ValueType representation.
Public conformance continues to use explicit traits. Internal categories describe
storage and validation prerequisites; they do not create source-language
inheritance, implicit implementations or runtime dispatch.

## Separate questions

The model keeps four questions distinct:

1. What is the value's storage and semantic identity? Numeric family, scalar,
   array, tensor, bool, unit, record, function, wrappers and residence are facts.
2. Does a nominal value identify a canonical owned resource? File handles,
   buffers and iterator cursors retain their existing ownership contracts.
   Record spelling or matching fields never establishes canonical identity.
3. Does an operation accept these type facts in this compilation context?
   Host services, numeric operations and device storage have distinct gates.
4. Is this particular value/function eligible for transport or execution?
   Function bodies, captures, provenance, aggregate layouts and actual device
   support still require their existing bounded analyses and runtime admission.

An array can be an Iterable source without being a stateful Iterator cursor.
A custom Add implementation does not turn its Self type into a primitive number.
A String's host arithmetic/formatting support does not establish GPU text support.

## Computed facts

Facts are computed from the current type, rather than cached inside it. They
preserve array/tensor flags independently: arrays use a ranked representation,
while rank-zero tensors are distinct from scalars. Invalid combinations are not
normalized into accepted types. The numeric element field is ignored when the
semantic builtin is nonnumeric.

Completed host values exclude task/fallible wrappers and device residence.
A plain scalar additionally excludes collection storage and shape. Error-family
metadata remains an independent fact: existing validators intentionally differ
in whether they require its absence. Migration preserves those checks and their
diagnostic precedence instead of silently strengthening malformed-IR rejection.

Canonical resource facts use the module's standard-record identities. They do
not infer recursive ownership, copying permission, mutability or trait
conformance from storage shape.

## Operation and target context

Capability requests name the operation and the actual compilation target:
CPU, Metal, Vulkan or CUDA. Numeric arithmetic policy and observed device
requirements remain explicit. A compile-time type check can report a missing
device requirement; it cannot certify that a driver/device supports it.

Shared device storage predicates are prerequisites, not a universal GPU-safe
flag. Callable signatures still need target/capture provenance and eligible
method bodies. Record/enum contents still need bounded layout analysis. Host
resources and suspended iterator frames retain their host-only restrictions.
No rejected operation is silently redirected to the CPU.

Legacy numeric IR validation is a separate validation profile. Its narrower
pure-expression rules must not be confused with the richer shared GPU compiler.

## Migration and compatibility

Migrate repeated predicates through shared queries while retaining each
operation's argument count, nominal layout, shape, error and ownership checks.
Keep type identity, ValueType layout, public ABI, numerical behavior,
cancellation/fault precedence and resource budgets unchanged.

Type facts and capability decisions run in the compiler. Generated programs
gain no type-category tags, dynamic conformance tables or capability dispatch.

## Qualification

Use differential truth tables for migrated predicates, including malformed flag
combinations, empty and nonempty shapes, all ten numeric types and invalid
nominal identities. Check bool and unit semantic identity separately even where
they share physical storage. Exercise every supported target/policy combination
and distinguish static eligibility from device requirements and value analysis.

Run focused normal/sanitized type, frontend and backend checks during migration.
Source tests cover owned resources, iterators, arithmetic traits, captures,
expected errors and mixed GPU applications. Frozen baseline/candidate programs
must preserve diagnostics and generated code where behavior is unchanged.
Controlled performance checks retain load/stability failures and regressions.
Complete the hosted package/cross-target matrix before review; physical GPU
execution complements compile-only hosted GPU checks. Unsupported sanitizer
modes and known driver/loader findings remain explicit.

## Shared query interfaces

TypeCapabilities.h provides a non-owning TypeFacts view of a live ValueType.
The factory rejects temporary types, so a stored view cannot refer to a destroyed
argument. completedHostScalar preserves the legacy completed-value profile;
plainHostScalar additionally requires the absence of error-family metadata.
The frontend map-signature prerequisite is named separately from normalized IR
scalar validation, preserving parallel error channels and validation phases.

ownedResourceKind in Semantics.h supplies the canonical resource identity used
by isOwnedResource and its specific helpers. This fact does not certify that a
particular wrapped, aggregate or malformed value can be moved or copied.

numericTypeCapability receives the numeric type, CapabilityTarget,
ArithmeticMode and NumericCapabilityOperation. Storage is distinct from
arithmetic: Metal can transport f64 bits and perform a bit-selecting ReLU without
claiming hardware f64 arithmetic. The hardware-arithmetic rejection retains its
existing diagnostic. Accepted GPU requests carry requiresDeviceValidation;
hardware f64 arithmetic also records requiresNativeFloat64 where supported by
the target. Device float controls, kernel capabilities, shapes, operation
contracts and actual feature admission remain with their existing validators.

DeviceTypeStorage names the shared GPU layout prerequisites for scalar cells,
flat local arrays, inline records, record arrays, callables and tensors. The
aggregate planner, closure transport and checkpoint schemas continue to perform
bounded field/layout/provenance analyses after these structural queries. Bool
and unit can share a physical boolean cell while retaining different semantic
builtin identities.

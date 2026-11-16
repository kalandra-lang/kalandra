# Arithmetic traits and parameterized capabilities

Arithmetic capabilities are static relationships between types. `Add[Rhs]`,
`Sub[Rhs]`, `Mul[Rhs]`, `Div[Rhs]`, `Rem[Rhs]` and `Neg` belong to `std/ops`.
Each implementation chooses an associated `Output`. Binary operations need not
use matching operand types or return either operand type.

Operators evaluate operands once in source order and provide read access.
Implementations cannot implicitly acquire mutation or ownership. Required
methods are infallible; checked faults remain checked faults. Device eligibility
is determined by the specialized body, with no implicit host execution.

## Identity and coherence

A conformance consists of the canonical trait declaration, `Self`, and every
trait argument. Imported aliases do not create a new identity. Cache keys and
proofs must include that entire tuple.

Implementation overlap is a unification problem over the tuple. Repeated type
parameters constrain all their occurrences together. The occurs check rejects
infinite constructions. Bounds do not prove disjointness; unresolved or
non-injective patterns are treated conservatively. A parameter mentioned only
through `T.Item` is not determined by the implementation pattern.

The module defining the trait or an outer nominal operand type owns an
implementation. Merely mentioning a local type inside a foreign constructor
does not grant ownership.

## Associated types and generic proofs

Trait parameters and implementation parameters have separate lexical scopes.
`impl[T] Convert[String] for Box[T]` does not bind the implementation's `T` to
`String`.

`Trait[Self, Arguments...].Item` identifies one associated projection. `T.Item`
is shorthand only when one declared application supplies it. Distinct abstract
arguments stay distinct even if a particular call instantiates both as `i64`.

Compile-time projection identities retain the implementing generic term and all
argument terms. Forwarding substitutes every dependency before canonicalizing
the projection again. A known constructor may reduce an associated definition
only after its complete symbolic implementation tuple matches. An opaque type
parameter retains its public associated-type identity.

Concrete bound checking and caller-side abstract proof checking are separate.
A concrete implementation found during specialization does not establish a
missing bound in the caller's generic signature. Method parameters are checked
against the reconstructed abstract signature as well as their physical types.
These rules also apply to associated error channels and callable values.

## Lowering and component boundaries

Primitive numeric and tensor operators keep their existing typed IR and lowering.
Their source expressions do not load `std/ops`. Explicit trait calls obtain a
compiler-owned primitive conformance whose private helper uses that same IR.
Custom operators select only canonical standard traits, then emit ordinary
concrete function calls. A user trait with the same spelling cannot intercept
an operator. Imports of a type's implementation module supply its canonical
trait dependency transitively.

Continuation lowering can import synchronous functions into another function
namespace. Component copy adapters must preserve the original checked callable
identity rather than infer a projected ordinal from a return type. Private
source-callable aliases map referenced targets to the actual code, survive
component optimization, and are resolved before legacy direct native names.
This is compiler linkage metadata: callable payloads, ownership validation,
cancellation and runtime ABIs keep their existing contracts.

## Contributor checks

- `GenericsTests.cpp` checks trait applications, correlated overlap, projection
  ambiguity/provenance, associated errors and invalid implementations.
- `GenericsSourceTests.py` executes mixed operators, all numeric primitive/tensor
  trait operations, evaluation order, and mixed fallible/trait callbacks. Its
  target option exercises the actual selected backend.
- `GenericsCases.py` includes arithmetic in the portable installed-package and
  cross-target execution fixture.
- LSP tests cover generic metadata, standard imports, rename isolation and
  recovery before trait declarations. Compiler-owned text is immutable; it
  cannot capture user locals during a rename.
- Documentation fixtures check the authored examples and rendered source.

Performance qualification compares frozen primitive and explicit-function
controls with overloaded operators, using separately compiled native objects,
load monitoring, paired controls and independent replication. Preserve failed
controls and regressions. Timing runs must not overlap builds or correctness
checks. Windows GPU execution still requires physical qualification; unsupported
sanitizer modes and the Vulkan loader leak are not passing tests.

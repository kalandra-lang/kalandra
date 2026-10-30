# Kalandra core language proposal

Core syntax and immutable value behavior accepted for the design baseline — 2026-10-14. Numeric rules are accepted in the [numeric design](numbers.md); the memory programming model is recorded in [memory and allocation design](memory.md). The examples below describe proposed syntax and behavior for a fresh implementation. They are not supported programs for the released Kalandra 0.13 compiler.

Kalandra should make immutable programs easy to write and learn, with safe memory use and a path to native CPU, GPU, and distributed execution. Start with values, bindings, functions, and transformations. Choose the syntax and observable behavior before choosing the memory-management mechanism or implementing a backend.

## Direction already established

- The released implementation is a prototype and reference, not a compatibility contract for the next implementation.
- Keep the useful Kalandra concepts: functions, readable composition, parallel work, and remote calls.
- All ordinary language values are immutable. Changing a record, collection, or tensor produces a new logical value.
- Memory safety is a requirement. Immutability alone does not establish valid lifetimes or safe foreign access.
- Keep everyday code small and readable. Memory lifetimes and allocation strategy are handled by the compiler/runtime without source-level allocation scopes.
- Types and correctness checks belong to the language and compiler. Type decorators such as `@params`, `@returns`, and `@check` are not part of this proposed core.
- Numeric types distinguish signed integers, unsigned integers, and floating-point values: `int`, `uint`, `float`, and explicit-width types. There is no universal `Number` type.
- Function/call syntax, explicit anonymous parameters, pipelines, and immutable record derivation below are accepted as the core design baseline.
- Native execution, tensors, and GPU support inform the design from the beginning. Their syntax is a later design step.

The core below is the accepted starting point for the fresh language. Numeric widths and conversion details are accepted in the [numeric design](numbers.md). The separately selected memory model is described in [memory and allocation design](memory.md).

## Five representative programs

### 1. Bind and use a value

```kalandra
price = 12
quantity = 3
total = price * quantity
```

`total` is 36. Each `=` introduces a binding in its lexical scope. It does not overwrite storage. Unconstrained integer literals in this example infer `int`. Its default is int64, as specified by the accepted numeric design.

An annotation uses a colon:

```kalandra
price: int = 12
```

Names are available after their binding. A binding's name is not available in its own initializer. Binding statements evaluate in source order; expressions evaluate their operands and arguments left to right, once each.

Invalid:

```kalandra
price = 12
price = 15
```

Reject the second declaration: `price` is already bound in this scope. Suggest a new name when the author wants to describe another value.

### 2. Declare and call a function

```kalandra
fn double(value: int) -> int:
  value * 2

answer = double(21)
piped = 21 -> double
```

`answer` and `piped` are both 42. Indentation defines the body; its final expression provides the result. Parameters have explicit names and ordinary type annotations. The return annotation may be omitted when the compiler can infer it:

```kalandra
fn double(value: int):
  value * 2
```

Use one application syntax, `name(arguments)`, for ordinary calls. Keep `->` for composition. A pipeline inserts its left-hand value as the first argument:

```kalandra
fn scale(value: int, factor: int) -> int:
  value * factor

answer = 10 -> scale(3) -> double
```

This means `double(scale(10, 3))`, yielding 60. Evaluate each stage once, in order. The pipeline itself does not imply parallel execution.

Invalid:

```kalandra
answer = double("hello")
```

Reject the call: the parameter requires `int`, but the argument is `String`. No decorator is needed and there is no implicit string-to-number conversion.

### 3. Construct a record and derive another

The native [synchronous scalar-field record subset](records.md) now implements this form. Broader field types and execution integration remain separate work.

```kalandra
type Point:
  x: float
  y: float

origin = Point { x: 0.0, y: 0.0 }
moved = origin with { x: 10.0 }
```

`origin` remains `Point { x: 0.0, y: 0.0 }`. `moved` is `Point { x: 10.0, y: 0.0 }`. A record declaration introduces a named type. Fields are supplied by name, avoiding constructor-order surprises. `with` derives another value of the same type; omitted fields retain their values.

Construction requires each field exactly once. An update must name existing fields exactly once. Field values must have compatible types. Structural sharing is permitted, but observable alias mutation is not.

Invalid:

```kalandra
origin.x = 10
```

Reject the write: record fields are immutable. Suggest `moved = origin with { x: 10.0 }`.

Also reject a misspelled or duplicate update field:

```kalandra
moved = origin with { z: 10 }
movedAgain = origin with { x: 10, x: 20 }
```

The first expression names no field in `Point`; the second supplies `x` twice.

### 4. Transform an array

```kalandra
values = [1, 2, 3]
doubled = values -> map(double)
```

`values` remains `[1, 2, 3]`. `doubled` is `[2, 4, 6]`. Arrays are ordered and homogeneous; `[int]` denotes an array of `int`. Element count is runtime data, not part of this initial array type. The [tensor proposal](tensors.md) defines accepted ranked shape syntax separately from arrays.

For the core, `map(values, transform)` calls `transform` with exactly one argument: the element. It returns results in input order. Additional index or collection arguments are never secretly appended. This first definition is sequential; parallel mapping will be explicit in the execution proposal.

Use commas to separate elements and arguments. The same expression remains readable when elements become calls:

```kalandra
values = [double(1), double(2), double(3)]
```

Invalid:

```kalandra
values[0] = 99
```

Reject the write: array elements are immutable. Collection replacement operations are a later library decision; they must return another logical value.

### 5. Return a derived array and share the input

```kalandra
fn shifted(values: [int], offset: int) -> [int]:
  values -> map(fn(value): value + offset)

input = [1, 2, 3]
first = shifted(input, 10)
second = shifted(input, 20)
```

`input` remains `[1, 2, 3]`, `first` is `[11, 12, 13]`, and `second` is `[21, 22, 23]`. All three bindings remain usable. Passing an ordinary immutable value does not consume the binding or require an observable deep copy.

An anonymous function uses the same `fn` concept with an explicit parameter list. Here `map` supplies the expected parameter type, so `value` is inferred as `int`. `offset` is a lexical capture of the surrounding function. There are no implicit `@name` parameters, and nested functions do not guess which function owns a name.

The returned array remains valid after `shifted` finishes. The captured offset remains valid for as long as any corresponding closure needs it. The compiler/runtime must ensure these lifetimes; the author does not manually free either.

The two calls have no data conflict and are candidates for later parallel execution. This example does not specify scheduling, implicit parallelism, or task syntax.

Invalid:

```kalandra
fn shifted(values: [int], offset: int) -> [int]:
  values -> map(fn(value): value + missing)
```

Reject the unresolved name `missing`. Do not reinterpret it as another anonymous parameter.

## Core grammar

| Construct           | Accepted spelling                   | Reason                                               |
| ------------------- | ----------------------------------- | ---------------------------------------------------- |
| Binding             | `name = expression`                 | Retains familiar Kalandra spelling; statements only  |
| Annotated binding   | `name: Type = expression`           | Puts the type next to its name                       |
| Named function      | `fn name(parameter: Type) -> Type:` | Explicit argument boundaries and return type         |
| Anonymous function  | `fn(parameter): expression`         | Explicit parameters; types may come from context     |
| Ordinary call       | `name(argument, argument)`          | Removes ambiguity between application and arithmetic |
| Pipeline            | `value -> function(argument)`       | One rule: insert the value as the first argument     |
| Array               | `[element, element]`                | Expressions cannot blend into neighboring elements   |
| Record construction | `Type { field: value }`             | Named fields avoid positional constructor ambiguity  |
| Record derivation   | `value with { field: replacement }` | Clearly expresses a new value                        |

Parentheses and commas intentionally replace the prototype's whitespace application and array syntax. They establish explicit boundaries for nested calls, arithmetic arguments, and collection elements.

Application, field access, and indexing bind more tightly than arithmetic; arithmetic binds more tightly than pipelines. Thus `10 + 2 -> double` means `double(10 + 2)`. Parentheses may always make grouping explicit. A complete operator table comes with the grammar step; this draft introduces no additional operators.

`=` is never an expression, including inside arguments, conditions, or pipelines. Do not introduce the prototype's binding-through-`=>` mechanism into this core. This removes declaration lifting and surprising visibility from expressions.

A function body introduces a lexical scope. Nested scopes may shadow outer names; duplicate declarations in one scope are rejected. A closure resolves each captured name lexically and retains that binding, regardless of similarly named bindings elsewhere.

## Types and inference

The initial core has signed integer, unsigned integer, and floating-point types alongside `Bool`, `String`, homogeneous arrays, named records, and function types. There is no universal `Number` type.

| Family           | Convenient spelling | Explicit-width spellings for the numeric proposal |
| ---------------- | ------------------- | ------------------------------------------------- |
| Signed integer   | `int`               | `int8`, `int16`, `int32`, `int64`                 |
| Unsigned integer | `uint`              | `uint8`, `uint16`, `uint32`, `uint64`             |
| Floating point   | `float`             | `float32`, `float64`                              |

The accepted defaults are `int = int64`, `uint = uint64`, and `float = float64`. Prefer defining them as aliases of specified widths that do not change between CPU architectures or between local and remote execution. The explicit-width list is a starting proposal, not a limit on later types; 128-bit integers, float16, and bfloat16 need their own representation, arithmetic, and device-support rules.

Infer binding types, return types, and anonymous parameters when their context establishes a concrete type. Named function parameters have annotations. Missing information is an actionable diagnostic, not an automatic conversion to `Any`.

Accepted literal rules:

- An unconstrained whole-number literal infers `int`; a decimal or floating exponent literal infers `float`.
- A known expected numeric type can determine a literal's type directly. This is literal typing, not an implicit conversion between already typed values.
- Diagnose a literal outside the chosen type's range. Negative literals cannot initialize `uint`.
- Already typed integer, unsigned, and floating-point values do not mix silently. Explicit conversions get a separate syntax and safety proposal.

```kalandra
count = 3
ratio = 0.5
capacity: uint = 32
sample: float32 = 0.5
```

Here `count` is `int`, `ratio` is `float`, `capacity` is `uint`, and `sample` is `float32`. Rounding and range rules are specified in the numeric design.

Invalid:

```kalandra
capacity: uint = -1
small: uint8 = 256
mixed = count + ratio
```

The first value is negative, the second exceeds uint8's range, and the third mixes already typed integer and floating-point values without an explicit conversion.

An empty array needs a type annotation or a context that supplies its element type. For example, `values: [int] = []` is clear; an unconstrained `values = []` requires more information in this core.

The [numeric design](numbers.md) specifies accepted alias widths, checked arithmetic, conversions, division, and floating-point policies. These examples use representable values and make no broader numerical promise. Tensor element precision must be explicit rather than determined by whichever device executes a program.

Optional values, errors, generics, tensor types, and explicit resource types each get a later focused proposal.

## Automatic memory management

The [memory design](memory.md) selects automatic lifetime management and compiler-chosen allocation strategies. Values stay valid when passed, returned, shared, or captured. Temporary arrays need no special keyword: the compiler can eliminate them, release them after their final uses, or place proven temporary storage in internal regions.

An allocation optimization cannot invalidate an escaping value or impose new source-level restrictions. Safe code does not manually free or reset accessible memory. CPU tasks, device commands, and foreign operations retain storage until their use actually ends. Their completion/error contracts belong to the execution proposal.

## Which prototype flaws this addresses

| Existing concern                                                            | Proposed treatment                                                   |
| --------------------------------------------------------------------------- | -------------------------------------------------------------------- |
| Nested implicit anonymous parameters (#233)                                 | Explicit parameter lists and lexical captures                        |
| Bindings lifted out of expressions (LD-001, LD-003, LD-004, LD-006; LC-001) | Bindings are statements; expression evaluation stays in place        |
| Extra mapped arguments evaluated inconsistently (LC-002)                    | Ordinary argument evaluation rules; explicit mapping contract        |
| `@check` sees implicit index/collection arguments (LC-006)                  | `map` supplies exactly one declared element argument                 |
| Positional record construction and inherited field ordering                 | Named field construction; inheritance is not introduced here         |
| Reassignment and indexed/property writes (LC-004)                           | Immutable bindings and values; derive a new value explicitly         |
| Await-all ambiguity and async checking (LD-007, LC-005)                     | Resolve in the execution/type proposals, not with decorators         |
| Repeated comparison operands (LC-003)                                       | Define comparison chaining later; do not inherit repeated evaluation |

Membership, escaping, module cycles, event APIs, dynamic task identity, and other existing backlog entries still require their own decisions. A fresh implementation has no obligation to preserve their prototype behavior.

## Accepted baseline and next step

Review this draft through the five programs: are the spellings readable, is the result obvious, and can an author predict which values remain available? Then check the invalid cases: does each rejection have an explanation and a straightforward rewrite?

The core syntax and immutable transformations are accepted. Numeric families now replace the prototype's `Number`; default widths and arithmetic rules are accepted in the [numeric design](numbers.md). The [memory design](memory.md) now leaves allocation strategies to the compiler and removes allocation-scope syntax. The [errors and effects direction](errors.md) is accepted. [Resources and task lifetimes](execution.md) are accepted. [Variants and error payloads](variants.md) are accepted. [Tensor types and shapes](tensors.md) are accepted. Review [placement and transfers](placement.md) next.

Implementation follows the accepted core plus the required numeric and memory-safety decisions. Future conformance tests should assert the specified behavior and diagnostics rather than compare with generated prototype JavaScript.

The [native immutable array foundation](arrays.md) now implements numeric array literals, `[T]` annotations, value passing and retained worker results. [Sequential maps](array-map.md) support named and anonymous expression callbacks with lexical captures, including numeric, Bool, String and record elements. The examples above continue to define the accepted complete core direction.

The native [sequential array map](array-map.md) implements the anonymous expression callback and lexical capture examples above. [Named CPU function values](native-function-values.md) now support general bindings, parameters, returned named functions and runtime-selected calls. Returned captured closures and anonymous block bodies remain separate work.

[Explicit rank-one tensor construction from numeric arrays](arrays.md#explicit-tensor-construction-from-arrays) is now implemented through `tensor(values)`, including after a map and inside workers. [Runtime shape lists](reshape.md) now construct ranked tensors from flat arrays. [CPU tensor reshaping](tensor-reshape.md) and [explicit tensor-to-array construction](tensor-array.md) are now implemented, preserving numeric identity, logical row-major order and retained worker ownership.

[Native local function modules](modules.md) now support `export fn` and explicit relative named imports with aliases. Visibility is file-local by default; calls and callbacks resolve names in the declaring module. [Record type exports](type-exports.md) and [explicit re-exports](reexports.md) also preserve declaration identity across CPU modules. [Explicit local package mappings](local-packages.md) support offline library entry names; [Apple Metal function modules](native-metal-modules.md) share this loader within the existing numerical subset. Package installation/version resolution remains separate work; module cycles remain rejected.

The [proposed native String contract](strings.md) defines text encoding, exact equality, scalar length/indexing/slicing and managed lifetime requirements. Its implementation sequence begins with literal and value support; existing core String examples remain design examples until that slice is qualified.

[Native project manifests](native-projects.md) now record an explicit root source and local library entries for CPU builds, without automatic discovery or installation.

[Immutable CPU array slicing](array-slicing.md) selects checked half-open ranges for all existing array element types, including direct worker dispatch.

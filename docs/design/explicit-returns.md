# Explicit returns and primitive names

Primitive numeric types are spelled `i8`, `i16`, `i32`, `i64`, `u8`,
`u16`, `u32`, `u64`, `f32`, and `f64`. Boolean and single-value types
are `bool` and `unit`. Other named types retain PascalCase, including
`String`, `Tensor`, records, enums, and error families. The old numeric
spellings and the `int`, `uint`, and `float` aliases are removed.
Unannotated integer and floating literals continue to infer i64 and f64.

The only value of `unit` is `()`. A unit function can complete naturally
or execute bare `return`, equivalent to `return ()`.

Every function producing a non-unit result must return explicitly on
every completing path. Functions and anonymous functions share this rule.
The final expression of a function body is no longer an implicit return.
Paths which fail or never complete do not require a return.

```kalandra
fn clampLow(x: i64) -> i64:
  return if x > 10:
    x
  else:
    0
```

Blocks used as expressions retain their existing result rules. The
operand of return may be an if, match, parallel group, or loop expression.
Return exits the innermost lexical function, including when it appears
inside a loop or task group. Anonymous functions define new return
boundaries.

Returning preserves structured task settlement, read-access lifetimes,
owned-resource cleanup, checked faults, cancellation, and error precedence.
CPU, Metal, Vulkan, and CUDA functions follow the same language rules.

The migration covers source fixtures, generated Kalandra source, diagnostics,
formatting, editor tooling, documentation, and qualification harnesses.
Backend ABI identities, numerical precision, and C++/shader type names
retain their existing meanings. Entry-point output and process status are
the subject of a separate proposal.

## Lowering and ownership

The checker gives each function and catch handler a lexical return scope,
independent of the nearest source loop. Return values are checked against the
function's result context. Every exiting path must discharge its pending error
and child-observation obligations before later statements can affect those
obligations.

A single terminal return uses the existing function result directly. Early
returns use a constant-true control-flow boundary and transfers to its result
slot. This reuses the established CPU and device continuation machinery for
group settlement, failure precedence and ownership. Transfers release abandoned
array collectors and close the structured groups they leave.

Branches that return, fail or continue elsewhere have no value at their normal
join. The checker joins the success types of completing branches and retains
error channels from every branch. Executable-IR validation independently derives this
control-flow property. A literal-true loop without a break targeting that loop
also cannot finish normally.

The semantic identity of unit is distinct from bool. Physical value layouts use
a canonical zero bit or byte for unit, including function parameters and owned
array elements. Conditions require bool. Unit entry adapters check failures and
resource settlement before successful process completion.

Numeric source spellings are separate from storage labels in kernel resources
and runtime metadata. The public resolver accepts only the new source names.
Resource tooling resolves storage labels through a separate internal resolver.

CLI collection metadata distinguishes unit arrays from bool arrays even though
both use byte storage. Unit arrays display `()` elements and scalar unit entries
produce no output. The GPU wire representation continues to use the checked
boolean storage carrier; source types remain distinct.

Callable validation accepts the explicit return values of function boundaries
and ignores non-completing branches at expression joins. It continues to compare
the signatures of every branch that can actually produce a callable value.

Expected types constrain the success value and permit error propagation; they do
not introduce an error family by themselves. Match and conditional expressions
whose branches never complete infer their channels from the actual failing
branches. Loop results join the channels of their break and normal-completion
values. A return with a non-completing operand discharges that operand's error
identity, preserving replacement-error propagation without inventing a host
TaskError channel in otherwise device-compatible functions.

Invocation frames preserve a fallible callable's declared application-error
carrier while copying its success or error payload. Strict ordinary callable
transport remains separate. Recovery therefore retains captured backing even
when a factory returns an error containing a closure. Allocation-failure tests
check that unsuccessful copies publish no partial result and release backing.
This does not change cancellation or fault precedence.

Large generated GPU control closures use lossless run spelling for repeated
capture types in entry names. Type identity and ordering remain part of the name;
short signatures keep their existing spelling. This avoids exceeding CUDA's
existing artifact name bound without expanding resource limits.

Tail returns inside an already-returned conditional or match are normalized to
ordinary arm values before typing. Only final arm expressions are eligible;
normalization does not pass through operators, loops, calls, lambdas or catch
handlers. This preserves lexical exits and skipped work while keeping numerical
primitives visible to the GPU stage planner. The traversal is bounded by the
parsed expression graph, and unobserved-task checks still apply at the branch
and function boundaries.

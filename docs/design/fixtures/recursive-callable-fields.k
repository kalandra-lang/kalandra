enum Operations:
  End
  More:
    apply: fn(i64) -> i64
    next: Operations

fn make(offset: i64) -> Operations:
  return Operations.More { apply: fn(value): return value + offset, next: Operations.End }

fn main() -> i64:
  inner = make(20)
  outer = Operations.More { apply: fn(value): return value, next: inner }
  return match outer:
    Operations.End: 0
    Operations.More { next }:
      match next:
        Operations.End: 0
        Operations.More { apply }: apply(22)

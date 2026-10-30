enum Operations:
  End
  More:
    apply: fn(int) -> int
    next: Operations

fn make(offset: int) -> Operations:
  Operations.More { apply: fn(value): value + offset, next: Operations.End }

fn main() -> int:
  inner = make(20)
  outer = Operations.More { apply: fn(value): value, next: inner }
  match outer:
    Operations.End: 0
    Operations.More { next }:
      match next:
        Operations.End: 0
        Operations.More { apply }: apply(22)

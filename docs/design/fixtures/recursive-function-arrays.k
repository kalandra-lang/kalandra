enum Chain:
  End
  More:
    callbacks: [fn(i64) -> i64]
    next: Chain

fn make(offset: i64) -> Chain:
  add = fn(value: i64): return value + offset
  return Chain.More { callbacks: [add, add], next: Chain.End }

fn main() -> i64:
  root = Chain.More { callbacks: [], next: make(20) }
  return match root:
    Chain.End: 0
    Chain.More { next }:
      match next:
        Chain.End: 0
        Chain.More { callbacks }: callbacks[0](1) + callbacks[1](1)

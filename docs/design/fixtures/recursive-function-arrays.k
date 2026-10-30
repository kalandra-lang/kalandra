enum Chain:
  End
  More:
    callbacks: [fn(int) -> int]
    next: Chain

fn make(offset: int) -> Chain:
  add = fn(value: int): value + offset
  Chain.More { callbacks: [add, add], next: Chain.End }

fn main() -> int:
  root = Chain.More { callbacks: [], next: make(20) }
  match root:
    Chain.End: 0
    Chain.More { next }:
      match next:
        Chain.End: 0
        Chain.More { callbacks }: callbacks[0](1) + callbacks[1](1)

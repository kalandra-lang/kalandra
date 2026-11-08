enum List:
  Nil
  Cons:
    head: i64
    tail: List

fn increment(items: List) -> List ! TaskError:
  return match items:
    List.Nil: List.Nil
    List.Cons {head, tail}:
      rest = try parallel:
        try await spawn increment(tail)
      List.Cons {head: head + 1, tail: rest}

fn sum(items: List) -> i64:
  return match items:
    List.Nil: 0
    List.Cons {head, tail}: head + sum(tail)

fn main() -> i64 ! TaskError:
  original = List.Cons {head: 20, tail: List.Cons {head: 20, tail: List.Nil}}
  updated = try parallel:
    try await spawn increment(original)
  return sum(updated)

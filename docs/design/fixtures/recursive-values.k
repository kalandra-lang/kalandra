enum List:
  Nil
  Cons:
    head: int
    tail: List
fn sum(items: List) -> int:
  match items:
    List.Nil: 0
    List.Cons { head, tail }: head + sum(tail)

fn main() -> int:
  items = List.Cons {
    head: 10,
    tail: List.Cons { head: 20, tail: List.Nil }
  }
  sum(items)

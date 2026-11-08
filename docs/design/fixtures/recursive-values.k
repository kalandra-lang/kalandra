enum List:
  Nil
  Cons:
    head: i64
    tail: List
fn sum(items: List) -> i64:
  return match items:
    List.Nil: 0
    List.Cons { head, tail }: head + sum(tail)

fn main() -> i64:
  items = List.Cons {
    head: 10,
    tail: List.Cons { head: 20, tail: List.Nil }
  }
  return sum(items)

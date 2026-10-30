fn double(value: int) -> int:
  value * 2

fn triple(value: int) -> int:
  value * 3

fn apply(value: int, transform: fn(int) -> int) -> int:
  transform(value)

fn choose(flag: Bool) -> fn(int) -> int:
  if flag: double
  else: triple

export fn main() -> int:
  transform = choose(false)
  alias: fn(int) -> int = transform
  items = map([1, 2, 3], alias)
  apply(4, choose(true)) + items[2]

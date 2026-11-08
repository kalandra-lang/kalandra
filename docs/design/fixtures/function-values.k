fn double(value: i64) -> i64:
  return value * 2

fn triple(value: i64) -> i64:
  return value * 3

fn apply(value: i64, transform: fn(i64) -> i64) -> i64:
  return transform(value)

fn choose(flag: bool) -> fn(i64) -> i64:
  return if flag: double
  else: triple

export fn main() -> i64:
  transform = choose(false)
  alias: fn(i64) -> i64 = transform
  items = map([1, 2, 3], alias)
  return apply(4, choose(true)) + items[2]

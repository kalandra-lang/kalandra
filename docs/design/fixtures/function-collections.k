fn keep(values: [i64]) -> [i64]:
  return values

fn doubled(values: [i64]) -> [i64]:
  return map(values, fn(value): return value * 2)

fn choose(flag: bool) -> fn([i64]) -> [i64]:
  return if flag: doubled
  else: keep

fn apply(values: [i64], transform: fn([i64]) -> [i64]) -> [i64]:
  return transform(values)

export fn main() -> i64:
  transform = choose(true)
  values = apply([10, 11], transform)
  return values[0] + values[1]

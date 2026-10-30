fn keep(values: [int]) -> [int]:
  values

fn doubled(values: [int]) -> [int]:
  map(values, fn(value): value * 2)

fn choose(flag: Bool) -> fn([int]) -> [int]:
  if flag: doubled
  else: keep

fn apply(values: [int], transform: fn([int]) -> [int]) -> [int]:
  transform(values)

export fn main() -> int:
  transform = choose(true)
  values = apply([10, 11], transform)
  values[0] + values[1]

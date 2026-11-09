fn apply(value: i64, transform: fn(i64) -> i64) -> i64:
  return transform(value)

fn main() -> i64 ! TaskError:
  offsets = [20, 20]
  transform = fn(value: i64): return offsets[0] + offsets[1] + value
  return try parallel:
    work = spawn apply(2, transform)
    first = try await work
    second = try await work
    first + second - 42

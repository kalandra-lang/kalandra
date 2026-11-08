fn double(value: i64) -> i64:
  return value * 2

fn main() -> i64 ! TaskError:
  transform = double
  return try parallel:
    work = spawn transform(21)
    try await work

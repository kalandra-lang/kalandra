fn makeAdder(offset: i64) -> fn(i64) -> i64:
  values = [offset]
  return fn(value: i64): return values[0] + value

fn main() -> i64 ! TaskError:
  transform = try parallel:
    try await spawn makeAdder(40)
  return transform(2)

error BuildError:
  Invalid:
    text: String
    values: [i64]

fn makeAdder() -> (fn(i64) -> i64) ! BuildError:
  fail BuildError.Invalid {text: "é!", values: [20, 20]}

fn obtain() -> (fn(i64) -> i64) ! (BuildError | TaskError):
  return try parallel:
    try await spawn makeAdder()

fn main() -> i64:
  transform = obtain() catch fn(error):
    return match error:
      BuildError.Invalid {text, values}:
        fn(value: i64): return length(text) + values[0] + values[1] + value
      _: fn(value: i64): return 0
  return transform(0)

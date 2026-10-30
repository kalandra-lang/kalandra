error BuildError:
  Invalid:
    text: String
    values: [int]

fn makeAdder() -> (fn(int) -> int) ! BuildError:
  fail BuildError.Invalid {text: "é!", values: [20, 20]}

fn obtain() -> (fn(int) -> int) ! (BuildError | TaskError):
  try parallel:
    try await spawn makeAdder()

fn main() -> int:
  transform = obtain() catch fn(error):
    match error:
      BuildError.Invalid {text, values}:
        fn(value: int): length(text) + values[0] + values[1] + value
      _: fn(value: int): 0
  transform(0)

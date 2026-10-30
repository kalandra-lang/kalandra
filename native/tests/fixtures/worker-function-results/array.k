fn makeAdder(offset: int) -> fn(int) -> int:
  values = [offset]
  fn(value: int): values[0] + value

fn main() -> int ! TaskError:
  transform = try parallel:
    try await spawn makeAdder(40)
  transform(2)

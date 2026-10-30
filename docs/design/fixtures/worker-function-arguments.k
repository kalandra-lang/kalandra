fn apply(value: int, transform: fn(int) -> int) -> int:
  transform(value)

fn main() -> int ! TaskError:
  offsets = [20, 20]
  transform = fn(value: int): offsets[0] + offsets[1] + value
  try parallel:
    work = spawn apply(2, transform)
    first = try await work
    second = try await work
    first + second - 42

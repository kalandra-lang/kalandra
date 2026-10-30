fn double(value: int) -> int:
  value * 2

fn main() -> int ! TaskError:
  transform = double
  try parallel:
    work = spawn transform(21)
    try await work

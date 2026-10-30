fn double(value: float32) -> float32:
  value * 2

fn main() -> float32 ! TaskError:
  try parallel:
    first = spawn double(2)
    inner = try parallel:
      second = spawn double(3)
      try await second
    outer = try await first
    inner + outer

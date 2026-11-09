fn double(value: f32) -> f32:
  return value * 2

fn main() -> f32 ! TaskError:
  return try parallel:
    first = spawn double(2)
    inner = try parallel:
      second = spawn double(3)
      try await second
    outer = try await first
    inner + outer

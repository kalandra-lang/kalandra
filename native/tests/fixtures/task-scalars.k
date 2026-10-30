fn double(value: float32) -> float32:
  value * 2

fn main() -> float32 ! TaskError:
  try parallel:
    first = spawn double(2)
    alias = first
    second = spawn double(3)
    a = try await alias
    b = try await second
    again = try await first
    inline = try await spawn double(1)
    a + b + again + inline

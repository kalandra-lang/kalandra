fn run(f:fn(int)->int)->int ! TaskError:
  try parallel:
    try await spawn f(42)
fn main()->int:
  42

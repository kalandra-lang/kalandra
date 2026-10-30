fn consume(callbacks:[fn(int)->int])->int ! TaskError:
  try parallel:
    try await spawn length(callbacks)
fn main()->int:
  42

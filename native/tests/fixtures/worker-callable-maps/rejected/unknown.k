fn consume(callback:fn(int)->int)->int ! TaskError:
  try parallel:
    values=try await spawn map([42],callback)
    values[0]
fn main()->int:
  42

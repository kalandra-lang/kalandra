error E:
  Retry:
    callbacks:[fn(int)->int]
fn worker()->int ! E:
  42
fn main()->int ! (E | TaskError):
  try parallel:
    try await spawn worker()

error E:
  Retry:
    callback:fn(int)->int
fn worker(callback:fn(int)->int)->int ! E:
  fail E.Retry {callback:callback}
fn submit(callback:fn(int)->int)->int ! (E | TaskError):
  try parallel:
    try await spawn worker(callback)
fn main()->int:
  42

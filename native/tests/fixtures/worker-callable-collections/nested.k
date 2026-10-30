fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
fn worker()->int ! TaskError:
  callbacks=[make(40)]
  try parallel:
    values=try await spawn slice(callbacks,0,1)
    values[0](2)
fn main()->int ! TaskError:
  try parallel:
    try await spawn worker()

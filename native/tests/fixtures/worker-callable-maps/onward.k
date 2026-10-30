fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
fn main()->int ! TaskError:
  callbacks=try parallel:
    try await spawn map([40],make)
  try parallel:
    values=try await spawn map(callbacks,fn(callback):callback(2))
    values[0]

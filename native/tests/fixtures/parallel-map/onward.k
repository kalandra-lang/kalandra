fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
fn produce()->[fn(int)->int] ! TaskError:
  try parallelMap([40],make)
fn main()->int ! TaskError:
  callbacks=try parallel:
    try await spawn produce()
  values=try parallelMap(callbacks,fn(callback):callback(2))
  values[0]

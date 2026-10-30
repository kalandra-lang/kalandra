fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
fn main()->int ! TaskError:
  callbacks=[make(1),make(40)]
  values=try parallel:
    try await spawn slice(callbacks,1,2)
  values[0](2)

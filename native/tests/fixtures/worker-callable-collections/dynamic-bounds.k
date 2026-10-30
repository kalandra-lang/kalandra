fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
fn select(callbacks:[fn(int)->int],start:int,end:int)->[fn(int)->int] ! TaskError:
  try parallel:
    try await spawn slice(callbacks,start,end)
fn main()->int ! TaskError:
  selected=try select([make(0),make(40)],1,2)
  selected[0](2)

enum Choice:
  Empty
  Some:
    run:fn(int)->int
fn make(offset:int)->[fn(int)->int]:
  values=[offset]
  [fn(value):values[0]+value,fn(value):values[0]-value]
fn makeChoice(offset:int)->Choice:
  values=[offset]
  Choice.Some {run:fn(value:int):values[0]+value}
fn makeCaptured()->fn(int)->int:
  callbacks:[fn(int)->int]=[fn(value):value+40]
  fn(value:int):callbacks[0](value)
fn makeWords()->[fn(String)->int]:
  [fn(text):length(text)]
fn apply(callbacks:[fn(int)->int])->int:
  callbacks[0](2)
fn main()->int ! TaskError:
  extra=makeCaptured()
  extra(2)
  callbacks=try parallel:
    try await spawn make(21)
  callbacks[0](2)+callbacks[1](2)

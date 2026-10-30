fn make(offset:int)->[fn(int)->int]:
  values=[offset]
  [fn(value):values[0]+value,fn(value):values[0]-value]
fn main()->int ! TaskError:
  callbacks=try parallel:
    work=spawn make(21)
    first=try await work
    second=try await work
    first[0](2)+second[1](2)
    second
  callbacks[0](2)+callbacks[1](2)

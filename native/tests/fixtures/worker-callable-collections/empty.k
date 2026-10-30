fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
fn main()->int ! TaskError:
  callbacks:[fn(int)->int]=[]
  values=try parallel:
    try await spawn concat(callbacks,[make(40)])
  selected=try parallel:
    try await spawn slice(values,0,0)
  count=try parallel:
    try await spawn length(selected)
  count+values[0](2)

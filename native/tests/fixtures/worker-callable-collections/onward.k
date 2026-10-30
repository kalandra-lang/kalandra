fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
fn main()->int ! TaskError:
  original=try parallel:
    try await spawn map([40],make)
  selected=try parallel:
    try await spawn slice(original,0,1)
  combined=try parallel:
    try await spawn concat(selected,selected)
  result=try parallel:
    try await spawn map(combined,fn(callback):callback(1))
  result[0]+result[1]-40

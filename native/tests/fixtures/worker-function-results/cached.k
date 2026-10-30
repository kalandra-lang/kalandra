fn make(offset:int)->fn(int)->int:
  values=[offset]
  fn(value:int):values[0]+value
fn main()->int ! TaskError:
  selected=try parallel:
    work=spawn make(40)
    first=try await work
    second=try await work
    first(2)+second(2)
    second
  selected(2)

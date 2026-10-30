fn make(flag:Bool)->fn(int)->int:
  numbers=[40]
  text="hi"
  if flag:fn(value:int):numbers[0]+value
  else:fn(value:int):length(text)+38+value
fn main()->int ! TaskError:
  first=try parallel:
    try await spawn make(true)
  second=try parallel:
    try await spawn make(false)
  first(2)+second(2)-42

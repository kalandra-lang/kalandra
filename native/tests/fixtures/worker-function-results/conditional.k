fn make(flag:bool)->fn(i64)->i64:
  numbers=[40]
  text="hi"
  return if flag:fn(value:i64):return numbers[0]+value
  else:fn(value:i64):return length(text)+38+value
fn main()->i64 ! TaskError:
  first=try parallel:
    try await spawn make(true)
  second=try parallel:
    try await spawn make(false)
  return first(2)+second(2)-42

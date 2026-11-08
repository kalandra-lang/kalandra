type Box:
  text:String
  values:[i64]
fn make()->fn(i64)->i64:
  box=Box {text:"é🙂",values:[20,20]}
  return fn(value:i64):return length(box.text)+box.values[0]+box.values[1]+value
fn main()->i64 ! TaskError:
  f=try parallel:
    try await spawn make()
  return f(0)

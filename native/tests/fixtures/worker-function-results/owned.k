type Box:
  text:String
  values:[int]
fn make()->fn(int)->int:
  box=Box {text:"é🙂",values:[20,20]}
  fn(value:int):length(box.text)+box.values[0]+box.values[1]+value
fn main()->int ! TaskError:
  f=try parallel:
    try await spawn make()
  f(0)

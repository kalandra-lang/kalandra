error E:
  Bad:
    text:String
    values:[int]
fn make(flag:Bool)->(fn(int)->int) ! E:
  if flag:fail E.Bad {text:"é!",values:[20,20]}
  values=[40]
  fn(value:int):values[0]+value
fn run(flag:Bool)->(fn(int)->int) ! (E | TaskError):
  try parallel:
    try await spawn make(flag)
fn main()->int ! TaskError:
  f=run(true) catch fn(error):fn(value:int):42
  f(2)

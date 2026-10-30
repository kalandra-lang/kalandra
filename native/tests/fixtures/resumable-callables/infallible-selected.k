fn double(value:int)->int:
  value*2
fn run(value:int)->int:
  execute(value) catch fn(error):0
fn execute(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)
fn choose(flag:Bool)->fn(int)->int:
  if flag:run
  else:double
fn main()->int:
  first=choose(true)
  second=choose(false)
  first(10)+second(11)

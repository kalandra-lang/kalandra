fn double(value:int)->int:
  value*2
fn run(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)
fn ordinary(value:int)->int ! TaskError:
  value*2
fn choose(flag:Bool)->(fn(int)->int ! TaskError):
  if flag:run
  else:ordinary
fn main()->int ! TaskError:
  first=choose(true)
  second=choose(false)
  left=try first(10)
  right=try second(11)
  left+right

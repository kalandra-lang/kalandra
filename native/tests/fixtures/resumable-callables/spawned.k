fn double(value:int)->int:
  value*2
fn run(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)
fn main()->int ! TaskError:
  f=run
  try parallel:
    try await spawn f(21)

fn double(value:int)->int:
  value*2
fn coordinate(value:int)->int:
  run(value) catch fn(error):0
fn run(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)
fn main()->int ! TaskError:
  f=coordinate
  try parallel:
    try await spawn f(21)

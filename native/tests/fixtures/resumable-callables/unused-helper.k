fn apply(callback:fn(int)->int,value:int)->int:
  callback(value)
fn double(value:int)->int:
  value*2
fn run(value:int)->int:
  execute(value) catch fn(error):0
fn execute(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)
fn main()->int:
  callback=run
  callback(21)

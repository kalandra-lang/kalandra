fn double(value:int)->int:
  value*2
fn run(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)
fn generic(callback:fn(int)->int ! TaskError,value:int)->int ! TaskError:
  try callback(value)
fn main()->int ! TaskError:
  callback=run
  try callback(21)

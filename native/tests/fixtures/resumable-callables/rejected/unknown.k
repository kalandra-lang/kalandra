fn double(value:int)->int:
  value*2
fn run(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)
fn main(callback:fn(int)->int ! TaskError)->int ! TaskError:
  known=run
  try callback(21)

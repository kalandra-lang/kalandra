fn double(value:int)->int:
  value*2
fn run(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)
fn apply(f:fn(int)->int ! TaskError,value:int)->int ! TaskError:
  try f(value)
fn main()->int ! TaskError:
  try apply(run,21)

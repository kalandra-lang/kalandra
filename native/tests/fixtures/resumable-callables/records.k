type Action:
  f:fn(int)->int ! TaskError
fn double(value:int)->int:
  value*2
fn run(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)
fn worker(action:Action)->int ! TaskError:
  try action.f(21)
fn main()->int ! TaskError:
  action=Action {f:run}
  try parallel:
    try await spawn worker(action)

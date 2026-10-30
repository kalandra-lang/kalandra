enum Choice:
  Empty
  Some:
    run:fn(int)->int
fn make(flag:Bool)->Choice:
  values=[40]
  if flag:Choice.Some {run:fn(value:int):values[0]+value}
  else:Choice.Empty
fn apply(choice:Choice)->int:
  match choice:
    Choice.Empty:42
    Choice.Some {run}:run(2)
fn main()->int ! TaskError:
  choice=try parallel:
    try await spawn make(true)
  try parallel:
    try await spawn apply(choice)

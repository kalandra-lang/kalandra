enum Choice:
  Empty
  Some:
    run:fn(i64)->i64
fn make(flag:bool)->Choice:
  values=[40]
  return if flag:Choice.Some {run:fn(value:i64):return values[0]+value}
  else:Choice.Empty
fn apply(choice:Choice)->i64:
  return match choice:
    Choice.Empty:42
    Choice.Some {run}:run(2)
fn main()->i64 ! TaskError:
  choice=try parallel:
    try await spawn make(true)
  return try parallel:
    try await spawn apply(choice)

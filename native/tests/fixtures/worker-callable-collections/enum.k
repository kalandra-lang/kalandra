fn make(offset:i64)->fn(i64)->i64:
  numbers=[offset]
  return fn(value:i64):return numbers[0]+value
enum Choice:
  Empty
  Some:
    run:fn(i64)->i64
fn main()->i64 ! TaskError:
  first=[Choice.Empty]
  second=[Choice.Some {run:make(40)}]
  values=try parallel:
    try await spawn concat(first,second)
  return match values[1]:
    Choice.Empty:0
    Choice.Some {run}:run(2)

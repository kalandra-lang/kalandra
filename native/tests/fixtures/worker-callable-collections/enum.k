fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
enum Choice:
  Empty
  Some:
    run:fn(int)->int
fn main()->int ! TaskError:
  first=[Choice.Empty]
  second=[Choice.Some {run:make(40)}]
  values=try parallel:
    try await spawn concat(first,second)
  match values[1]:
    Choice.Empty:0
    Choice.Some {run}:run(2)

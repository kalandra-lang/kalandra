type Outcome:
  error:fn(int)->int
fn unsafe(error:fn(int)->int)->fn(int)->int:
  outcome=Outcome {error:error}
  fn(value:int):
    outcome.error(value)
fn consume(error:fn(int)->int)->int ! TaskError:
  callbacks=[unsafe(error)]
  good=[fn(value:int):value]
  try parallel:
    values=try await spawn concat(callbacks,good)
    length(values)
fn main()->int:
  42

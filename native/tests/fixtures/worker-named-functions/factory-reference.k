fn makeAdder(offset:int)->fn(int)->int:
  fn(value:int):value+offset
fn apply(value:int,maker:fn(int)->fn(int)->int)->int:
  transform=maker(40)
  transform(value)
fn main()->int ! TaskError:
  try parallel:
    try await spawn apply(2,makeAdder)

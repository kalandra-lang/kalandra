fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn forward(functions:[fn(int)->int])->[fn(int)->int]:
  functions
fn wrap(functions:[fn(int)->int])->fn(int)->int:
  fn(value:int):functions[0](value)
fn main()->int ! TaskError:
  functions=try parallel:
    try await spawn forward([double])
  try parallel:
    try await spawn apply(21,wrap(functions))

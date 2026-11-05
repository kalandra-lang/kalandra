fn entry(n:int)->int:
  cell=tensor([n],[])
  cell[]
fn main()->int ! TaskError:
  try parallel:
    try await spawn entry(4) on devices.gpu()

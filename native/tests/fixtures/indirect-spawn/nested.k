fn double(value:int)->int:
  value*2
fn run(f:fn(int)->int)->int ! TaskError:
  try parallel:
    a=spawn f(10)
    b=spawn f(11)
    (try await a)+(try await b)
fn main()->int ! TaskError:
  try parallel:
    try await spawn run(double)

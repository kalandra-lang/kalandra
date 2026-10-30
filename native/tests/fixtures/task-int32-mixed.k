fn mixed(integer:int32,real:float32) -> int32:
  changed = real + 0.5
  integer + 1
fn floating(value:float32) -> float32:
  value + 0.5
fn coordinator(integer:int32,real:float32) -> int32 ! TaskError:
  try parallel:
    a = spawn mixed(integer,real)
    b = spawn floating(real)
    x = try await a
    y = try await b
    x
fn main() -> int32 ! TaskError:
  try parallel:
    job = spawn coordinator(16777217,1.5)
    other = spawn floating(2.5)
    value = try await job
    real = try await other
    value

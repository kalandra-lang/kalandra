fn mixed(integer:i32,real:f32) -> i32:
  changed = real + 0.5
  return integer + 1
fn floating(value:f32) -> f32:
  return value + 0.5
fn coordinator(integer:i32,real:f32) -> i32 ! TaskError:
  return try parallel:
    a = spawn mixed(integer,real)
    b = spawn floating(real)
    x = try await a
    y = try await b
    x
fn main() -> i32 ! TaskError:
  return try parallel:
    job = spawn coordinator(16777217,1.5)
    other = spawn floating(2.5)
    value = try await job
    real = try await other
    value

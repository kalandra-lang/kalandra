fn entry(n:i64)->i64:
  cell=tensor([n],[])
  return cell[]
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn entry(4) on devices.gpu()

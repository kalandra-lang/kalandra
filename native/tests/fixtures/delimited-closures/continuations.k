type Box:
  fn:i64
fn sum(a:i64,b:i64)->i64:
  return a+b
fn main()->i64:
  box=Box {
    fn:
             20,
  }
  return sum(
       box.fn,
  22
  )

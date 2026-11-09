fn double(value:i64)->i64:
  return value*2
fn run(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn double(value)
fn other(value:i64)->i64 ! TaskError:
  return value*2
fn main()->i64 ! TaskError:
  callbacks=[run,other]
  first=try callbacks[0](10)
  second=try callbacks[1](11)
  return first+second

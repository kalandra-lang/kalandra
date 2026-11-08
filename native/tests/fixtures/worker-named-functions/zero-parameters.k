fn answer()->i64:
  return 42
fn apply(transform:fn()->i64)->i64:
  return transform()
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn apply(answer)

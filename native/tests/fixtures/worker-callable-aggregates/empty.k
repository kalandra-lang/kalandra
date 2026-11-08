fn make()->[fn(i64)->i64]:
  return []
fn main()->i64 ! TaskError:
  callbacks=try parallel:
    try await spawn make()
  return length(callbacks)+42

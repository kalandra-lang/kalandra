fn make()->[fn(int)->int]:
  []
fn main()->int ! TaskError:
  callbacks=try parallel:
    try await spawn make()
  length(callbacks)+42

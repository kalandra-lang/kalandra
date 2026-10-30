fn answer()->int:
  42
fn apply(transform:fn()->int)->int:
  transform()
fn main()->int ! TaskError:
  try parallel:
    try await spawn apply(answer)

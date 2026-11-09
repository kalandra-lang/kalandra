fn keep(values:[i64])->[i64]:
  return values
fn apply(values:[i64],transform:fn([i64])->[i64])->[i64]:
  return transform(values)
fn main()->i64 ! TaskError:
  values=try parallel:
    work=spawn apply([20,22],keep)
    first=try await work
    second=try await work
    [first[0],second[1]]
  return values[0]+values[1]

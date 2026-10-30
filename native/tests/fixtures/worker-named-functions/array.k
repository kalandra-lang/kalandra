fn keep(values:[int])->[int]:
  values
fn apply(values:[int],transform:fn([int])->[int])->[int]:
  transform(values)
fn main()->int ! TaskError:
  values=try parallel:
    work=spawn apply([20,22],keep)
    first=try await work
    second=try await work
    [first[0],second[1]]
  values[0]+values[1]

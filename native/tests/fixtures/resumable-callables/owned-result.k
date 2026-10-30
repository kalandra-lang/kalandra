fn append(values:[int])->[int]:
  concat(values,[2])
fn run(values:[int])->[int] ! TaskError:
  try parallel:
    try await spawn append(values)
fn main()->int ! TaskError:
  callback=run
  values=try callback([20,20])
  values[0]+values[1]+values[2]

fn shift(values:Tensor[float32,?])->Tensor[float32,?]:
  values+1
fn run(values:Tensor[float32,?])->Tensor[float32,?] ! TaskError:
  try parallel:
    try await spawn shift(values)
fn main()->float32 ! TaskError:
  callback=run
  input:Tensor[float32,2]=tensor([20,20])
  values=try callback(input)
  reduceSum(values)

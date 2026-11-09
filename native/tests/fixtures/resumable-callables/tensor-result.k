fn shift(values:Tensor[f32,?])->Tensor[f32,?]:
  return values+1
fn run(values:Tensor[f32,?])->Tensor[f32,?] ! TaskError:
  return try parallel:
    try await spawn shift(values)
fn main()->f32 ! TaskError:
  callback=run
  input:Tensor[f32,2]=tensor([20,20])
  values=try callback(input)
  return reduceSum(values)

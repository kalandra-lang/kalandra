fn operation(values:Tensor[f64,?])->Tensor[f64,?]:
  return values
fn main()->Tensor[f64,3] ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    values:Tensor[f64,3]=tensor([1,2,3])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident) on gpu
    try await transfer(answer,devices.cpu())

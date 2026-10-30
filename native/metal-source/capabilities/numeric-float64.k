fn operation(values:Tensor[float64,?])->Tensor[float64,?]:
  values
fn main()->Tensor[float64,3] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    values:Tensor[float64,3]=tensor([1,2,3])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident) on gpu
    try await transfer(answer,devices.cpu())

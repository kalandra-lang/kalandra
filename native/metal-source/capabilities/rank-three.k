fn operation(values:Tensor[float32,?,?,?])->Tensor[float32,?,?,?]:
  values
fn main()->Tensor[float32,1,1,3] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    values:Tensor[float32,1,1,3]=tensor([[[1,2,3]]])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident) on gpu
    try await transfer(answer,devices.cpu())

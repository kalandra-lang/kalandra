fn operation(values:Tensor[f32,?,?,?])->Tensor[f32,?,?,?]:
  return values
fn main()->Tensor[f32,1,1,3] ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    values:Tensor[f32,1,1,3]=tensor([[[1,2,3]]])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident) on gpu
    try await transfer(answer,devices.cpu())

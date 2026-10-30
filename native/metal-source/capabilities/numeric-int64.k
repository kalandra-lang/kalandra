fn operation(values:Tensor[int64,?])->Tensor[int64,?]:
  values
fn main()->Tensor[int64,3] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    values:Tensor[int64,3]=tensor([1,2,3])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident) on gpu
    try await transfer(answer,devices.cpu())

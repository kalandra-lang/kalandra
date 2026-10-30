fn operation(values:Tensor[int8,?])->Tensor[int8,?]:
  values
fn main()->Tensor[int8,3] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    values:Tensor[int8,3]=tensor([1,2,3])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident) on gpu
    try await transfer(answer,devices.cpu())

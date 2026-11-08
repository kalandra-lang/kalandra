fn operation(values:Tensor[i32,?])->Tensor[i32,?]:
  return values
fn main()->Tensor[i32,3] ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    values:Tensor[i32,3]=tensor([1,2,3])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident) on gpu
    try await transfer(answer,devices.cpu())

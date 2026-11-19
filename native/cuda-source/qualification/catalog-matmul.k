fn product(a:Tensor[f32,?,?],b:Tensor[f32,?,?])->Tensor[f32,?,?]:
  return matmul(a+1,b)

fn main()->Tensor[f32,?,?] ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    left:Tensor[f32,2,2]=tensor([[1,2],[3,4]])
    right:Tensor[f32,2,2]=tensor([[5,6],[7,8]])
    a=try await transfer(left,gpu)
    b=try await transfer(right,gpu)
    result=try await spawn product(a,b) on gpu
    try await transfer(result,devices.cpu())

fn compute(a:Tensor[f32,?,?],b:Tensor[f32,?,?])->Tensor[f32,?,?]:
  first=matmul(a,b)
  total=reduceSum(first)
  return matmul(first/total,b)
fn main()->Tensor[f32,?,?] ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    a:Tensor[f32,2,2]=tensor([[1,0],[0,1]])
    x=try await transfer(a,gpu)
    work=try await spawn compute(x,x) on gpu
    try await transfer(work,devices.cpu())

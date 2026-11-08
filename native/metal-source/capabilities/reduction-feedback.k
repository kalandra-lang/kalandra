fn compute(a:Tensor[f32,?,?],b:Tensor[f32,?,?])->f32:
  first=matmul(a,b)
  total=reduceSum(first)
  second=matmul(first/total,b)
  return reduceSum(second)
fn main()->f32 ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    a:Tensor[f32,2,2]=tensor([[1,0],[0,1]])
    x=try await transfer(a,gpu)
    try await spawn compute(x,x) on gpu

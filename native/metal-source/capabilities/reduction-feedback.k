fn compute(a:Tensor[float32,?,?],b:Tensor[float32,?,?])->float32:
  first=matmul(a,b)
  total=reduceSum(first)
  second=matmul(first/total,b)
  reduceSum(second)
fn main()->float32 ! TaskError:
  try parallel:
    gpu=devices.gpu()
    a:Tensor[float32,2,2]=tensor([[1,0],[0,1]])
    x=try await transfer(a,gpu)
    try await spawn compute(x,x) on gpu

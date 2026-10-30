fn compute(a:Tensor[float32,?,?],b:Tensor[float32,?,?])->Tensor[float32,?,?]:
  first=matmul(a,b)
  total=reduceSum(first)
  matmul(first/total,b)
fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    a:Tensor[float32,2,2]=tensor([[1,0],[0,1]])
    x=try await transfer(a,gpu)
    work=try await spawn compute(x,x) on gpu
    try await transfer(work,devices.cpu())

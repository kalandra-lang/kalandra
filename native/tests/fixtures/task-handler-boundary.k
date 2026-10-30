fn total() -> float32 ! TaskError:
  try parallel:
    work = spawn compute()
    try await work

fn compute() -> float32:
  data: Tensor[float32, 2] = tensor([1, 2])
  reduceSum(data)

fn main() -> float32:
  result = total()
  forwarded = result catch fn(error):
    try total()
    99
  recovered = forwarded catch fn(error): 40
  recovered + 2

fn compute() -> float32:
  data: Tensor[float32, 2] = tensor([1, 2])
  reduceSum(data)

fn main() -> float32:
  result = parallel:
    work = spawn compute()
    completed = await work
    first = completed catch fn(error): 99
    second = await work catch fn(error): 88
    first + second
  result catch fn(error): 42

fn compute() -> f32:
  data: Tensor[f32, 2] = tensor([1, 2])
  return reduceSum(data)

fn main() -> f32:
  result = parallel:
    work = spawn compute()
    completed = await work
    first = completed catch fn(error): return 99
    second = await work catch fn(error): return 88
    first + second
  return result catch fn(error): return 42

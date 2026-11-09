from "./kernels/public.k" import compute as calculate
from "./kernels/arithmetic.k" import offset
from "./public.k" import execute as run

fn helper(value: f32) -> f32:
  return value + 100

fn main() -> f32 ! TaskError:
  return try parallel:
    values: Tensor[f32, 3] = tensor([1, 2, 3])
    local = calculate(values, values)
    work = spawn run(values)
    result = try await work
    reduceSum(local + result) + offset(0) + helper(1)

from "./kernels/public.k" import compute as calculate
from "./kernels/arithmetic.k" import offset
from "./public.k" import execute as run

fn helper(value: float32) -> float32:
  value + 100

fn main() -> float32 ! TaskError:
  try parallel:
    values: Tensor[float32, 3] = tensor([1, 2, 3])
    local = calculate(values, values)
    work = spawn run(values)
    result = try await work
    reduceSum(local + result) + offset(0) + helper(1)

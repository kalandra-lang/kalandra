fn success() -> float32 ! TaskError:
  9

fn recover(a: Tensor[float32, ?]) -> float32:
  success() catch fn(error):
    exact: Tensor[float32, 2] = a
    reduceSum(exact)

fn main() -> float32:
  a: Tensor[float32, 3] = tensor([1, 2, 3])
  recover(a)

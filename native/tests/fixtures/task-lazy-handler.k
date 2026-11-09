fn success() -> f32 ! TaskError:
  return 9

fn recover(a: Tensor[f32, ?]) -> f32:
  return success() catch fn(error):
    exact: Tensor[f32, 2] = a
    return reduceSum(exact)

fn main() -> f32:
  a: Tensor[f32, 3] = tensor([1, 2, 3])
  return recover(a)

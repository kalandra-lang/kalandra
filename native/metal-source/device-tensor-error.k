error TileError:
  Invalid:
    tile: Tensor[int16,2,2]

fn calculate(n: int) -> int ! TileError:
  tile: Tensor[int16,2,2] = tensor([[1,2],[3,4]])
  if n == 0:
    fail TileError.Invalid { tile: tile }
  0

fn main() -> int ! TaskError:
  try parallel:
    bad = spawn calculate(0) on devices.gpu()
    await bad catch fn(error):
      match error:
        TileError.Invalid { tile }: int(reduceSum(tile))
        _: 0

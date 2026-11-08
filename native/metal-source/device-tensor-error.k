error TileError:
  Invalid:
    tile: Tensor[i16,2,2]

fn calculate(n: i64) -> i64 ! TileError:
  tile: Tensor[i16,2,2] = tensor([[1,2],[3,4]])
  if n == 0:
    fail TileError.Invalid { tile: tile }
  return 0

fn main() -> i64 ! TaskError:
  return try parallel:
    bad = spawn calculate(0) on devices.gpu()
    await bad catch fn(error):
      return match error:
        TileError.Invalid { tile }: i64(reduceSum(tile))
        _: 0

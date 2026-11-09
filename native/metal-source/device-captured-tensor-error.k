type Input:
  tile: Tensor[i16,2,2]

error TileError:
  Invalid:
    tile: Tensor[i16,2,2]

fn rejectInput(input: Input) -> i64 ! TileError:
  fail TileError.Invalid { tile: input.tile }

fn main() -> i64 ! TaskError:
  tile: Tensor[i16,2,2] = tensor([[1,2],[3,4]])
  input = Input { tile: tile }
  return try parallel:
    result = await spawn rejectInput(input) on devices.gpu()
    result catch fn(error):
      return match error:
        TileError.Invalid { tile }: i64(reduceSum(tile))
        _: 0

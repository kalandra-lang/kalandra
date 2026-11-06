type Input:
  tile: Tensor[int16,2,2]

error TileError:
  Invalid:
    tile: Tensor[int16,2,2]

fn rejectInput(input: Input) -> int ! TileError:
  fail TileError.Invalid { tile: input.tile }

fn main() -> int ! TaskError:
  tile: Tensor[int16,2,2] = tensor([[1,2],[3,4]])
  input = Input { tile: tile }
  try parallel:
    result = await spawn rejectInput(input) on devices.gpu()
    result catch fn(error):
      match error:
        TileError.Invalid { tile }: int(reduceSum(tile))
        _: 0

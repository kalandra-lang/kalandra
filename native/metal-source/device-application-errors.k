error SampleError:
  Invalid:
    values: [int]

error OtherError:
  Empty

fn sample(n: int) -> int ! SampleError:
  if n == 0 or n == 2:
    fail SampleError.Invalid { values: [7, 11] }
  5

fn descend(n: int) -> int ! (SampleError | OtherError):
  if n == 1:
    fail OtherError.Empty
  if n > 2:
    try descend(n - 1)
  else:
    try sample(n)

fn work(n:int)->int:
  descend(n) catch fn(error):
    match error:
      SampleError.Invalid { values }: values[0] + values[1]
      OtherError.Empty: 23

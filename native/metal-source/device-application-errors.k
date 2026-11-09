error SampleError:
  Invalid:
    values: [i64]

error OtherError:
  Empty

fn sample(n: i64) -> i64 ! SampleError:
  if n == 0 or n == 2:
    fail SampleError.Invalid { values: [7, 11] }
  return 5

fn descend(n: i64) -> i64 ! (SampleError | OtherError):
  if n == 1:
    fail OtherError.Empty
  return if n > 2:
    try descend(n - 1)
  else:
    try sample(n)

fn work(n:i64)->i64:
  return descend(n) catch fn(error):
    return match error:
      SampleError.Invalid { values }: values[0] + values[1]
      OtherError.Empty: 23

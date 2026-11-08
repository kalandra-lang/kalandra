fn main()->i64 ! TaskError:
  offset:i64=10
  return try parallel:
    task=spawn map([10,12],fn(value):
      total=value+offset
      return total
    )
    values=try await task
    values[0]+values[1]

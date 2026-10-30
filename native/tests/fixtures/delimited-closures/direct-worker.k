fn main()->int ! TaskError:
  offset:int=10
  try parallel:
    task=spawn map([10,12],fn(value):
      total=value+offset
      total
    )
    values=try await task
    values[0]+values[1]

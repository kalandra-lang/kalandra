fn main()->i64:
  values=map([20,22],fn(value): # begin body

    # ignore this indentation
    total=value+1

    return total-1 # result
  # boundary comment
  )
  return values[0]+values[1]

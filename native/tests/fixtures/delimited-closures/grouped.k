fn main()->i64:
  return (fn(value:i64):
    total=value+1
    return total*2
  )(20)

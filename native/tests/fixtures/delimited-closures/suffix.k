fn main()->i64:
  return (fn(value:i64):
    adjusted=value+1
    return adjusted
  )(20)*2

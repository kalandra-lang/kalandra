fn main() -> i64 ! TaskError:
  values = [1]
  values = [2]
  callbacks = try parallelMap(generate(17, fn(index): return index), fn(offset): return fn(value: i64): return value + offset)
  index = 0
  total = values[0]
  while index < length(callbacks):
    total = total + callbacks[index](1)
    index = index + 1
  return total

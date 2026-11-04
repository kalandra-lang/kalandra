fn main() -> int ! TaskError:
  values = [1]
  values = [2]
  callbacks = try parallelMap(generate(17, fn(index): index), fn(offset): fn(value: int): value + offset)
  index = 0
  total = values[0]
  while index < length(callbacks):
    total = total + callbacks[index](1)
    index = index + 1
  total

fn add(a:int32,b:int32) -> int32:
  a + b
fn subtract(a:int32,b:int32) -> int32:
  a - b
fn multiply(a:int32,b:int32) -> int32:
  a * b
fn divide(a:int32,b:int32) -> int32:
  a / b
fn remainder(a:int32,b:int32) -> int32:
  a % b
fn negate(a:int32) -> int32:
  -a
fn main() -> int32 ! TaskError:
  try parallel:
    first = spawn add(2147483646,1)
    alias = first
    second = spawn subtract(-2147483647,1)
    third = spawn multiply(-1073741824,2)
    fourth = spawn divide(-2147483648,1)
    fifth = spawn remainder(-2147483648,-1)
    sixth = spawn negate(-2147483647)
    seventh = spawn remainder(-5,2)
    eighth = spawn divide(5,-2)
    a = try await first
    b = try await second
    c = try await third
    d = try await fourth
    e = try await fifth
    f = try await sixth
    g = try await seventh
    h = try await eighth
    again = try await alias
    (a + b) + (c - d) + e + (f - again) + g + h

fn selectWideUnsigned(wideUnsigned:Tensor[u64,?],wideSigned:Tensor[i64,?],narrowUnsigned:Tensor[u32,?],narrowSigned:Tensor[i32,?],real:Tensor[f32,?],signedOffset:i64,unsignedOffset:u64,precise:Tensor[f64,?],offset:f64) -> Tensor[u64,?]:
  exact = precise * offset
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  return exactUnsigned
fn selectWideSigned(wideUnsigned:Tensor[u64,?],wideSigned:Tensor[i64,?],narrowUnsigned:Tensor[u32,?],narrowSigned:Tensor[i32,?],real:Tensor[f32,?],signedOffset:i64,unsignedOffset:u64,precise:Tensor[f64,?],offset:f64) -> Tensor[i64,?]:
  exact = precise * offset
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  return exactSigned
fn selectNarrowUnsigned(wideUnsigned:Tensor[u64,?],wideSigned:Tensor[i64,?],narrowUnsigned:Tensor[u32,?],narrowSigned:Tensor[i32,?],real:Tensor[f32,?],signedOffset:i64,unsignedOffset:u64,precise:Tensor[f64,?],offset:f64) -> Tensor[u32,?]:
  exact = precise * offset
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  return narrowU
fn selectNarrowSigned(wideUnsigned:Tensor[u64,?],wideSigned:Tensor[i64,?],narrowUnsigned:Tensor[u32,?],narrowSigned:Tensor[i32,?],real:Tensor[f32,?],signedOffset:i64,unsignedOffset:u64,precise:Tensor[f64,?],offset:f64) -> Tensor[i32,?]:
  exact = precise * offset
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  return narrowI
fn selectReal(wideUnsigned:Tensor[u64,?],wideSigned:Tensor[i64,?],narrowUnsigned:Tensor[u32,?],narrowSigned:Tensor[i32,?],real:Tensor[f32,?],signedOffset:i64,unsignedOffset:u64,precise:Tensor[f64,?],offset:f64) -> Tensor[f32,?]:
  exact = precise * offset
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  return floating
fn selectPrecise(wideUnsigned:Tensor[u64,?],wideSigned:Tensor[i64,?],narrowUnsigned:Tensor[u32,?],narrowSigned:Tensor[i32,?],real:Tensor[f32,?],signedOffset:i64,unsignedOffset:u64,precise:Tensor[f64,?],offset:f64) -> Tensor[f64,?]:
  exact = precise * offset
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  return exact
fn probe() -> Tensor[f64,?] ! TaskError:
  wideUnsigned:Tensor[u64,4] = tensor([0,18446744073709551615,9223372036854775808,9007199254740993])
  wideSigned:Tensor[i64,4] = tensor([-9223372036854775808,9223372036854775807,9007199254740993,-5])
  narrowUnsigned:Tensor[u32,4] = tensor([0,4294967295,2147483648,16777217])
  narrowSigned:Tensor[i32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[f32,4] = tensor([1.5,2.5,3.5,4.5])
  signedOffset:i64 = -9223372036854775808
  unsignedOffset:u64 = 18446744073709551615
  precise:Tensor[f64,4] = tensor([-0.0,1.0000000000000002,4.9406564584124654e-324,1.7976931348623157e308])
  offset:f64 = 1
  return try parallel:
    preciseWork = spawn selectPrecise(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset,precise,offset)
    wideUnsignedWork = spawn selectWideUnsigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset,precise,offset)
    wideSignedWork = spawn selectWideSigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset,precise,offset)
    narrowUnsignedWork = spawn selectNarrowUnsigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset,precise,offset)
    narrowSignedWork = spawn selectNarrowSigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset,precise,offset)
    realWork = spawn selectReal(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset,precise,offset)
    wideUnsignedResult = try await wideUnsignedWork
    wideSignedResult = try await wideSignedWork
    narrowUnsignedResult = try await narrowUnsignedWork
    narrowSignedResult = try await narrowSignedWork
    realResult = try await realWork
    try await preciseWork
fn main() -> i64:
  return 0

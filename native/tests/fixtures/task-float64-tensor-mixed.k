fn selectWideUnsigned(wideUnsigned:Tensor[uint64,?],wideSigned:Tensor[int64,?],narrowUnsigned:Tensor[uint32,?],narrowSigned:Tensor[int32,?],real:Tensor[float32,?],signedOffset:int64,unsignedOffset:uint64,precise:Tensor[float,?],offset:float) -> Tensor[uint64,?]:
  exact = precise * offset
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  exactUnsigned
fn selectWideSigned(wideUnsigned:Tensor[uint64,?],wideSigned:Tensor[int64,?],narrowUnsigned:Tensor[uint32,?],narrowSigned:Tensor[int32,?],real:Tensor[float32,?],signedOffset:int64,unsignedOffset:uint64,precise:Tensor[float,?],offset:float) -> Tensor[int64,?]:
  exact = precise * offset
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  exactSigned
fn selectNarrowUnsigned(wideUnsigned:Tensor[uint64,?],wideSigned:Tensor[int64,?],narrowUnsigned:Tensor[uint32,?],narrowSigned:Tensor[int32,?],real:Tensor[float32,?],signedOffset:int64,unsignedOffset:uint64,precise:Tensor[float,?],offset:float) -> Tensor[uint32,?]:
  exact = precise * offset
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  narrowU
fn selectNarrowSigned(wideUnsigned:Tensor[uint64,?],wideSigned:Tensor[int64,?],narrowUnsigned:Tensor[uint32,?],narrowSigned:Tensor[int32,?],real:Tensor[float32,?],signedOffset:int64,unsignedOffset:uint64,precise:Tensor[float,?],offset:float) -> Tensor[int32,?]:
  exact = precise * offset
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  narrowI
fn selectReal(wideUnsigned:Tensor[uint64,?],wideSigned:Tensor[int64,?],narrowUnsigned:Tensor[uint32,?],narrowSigned:Tensor[int32,?],real:Tensor[float32,?],signedOffset:int64,unsignedOffset:uint64,precise:Tensor[float,?],offset:float) -> Tensor[float32,?]:
  exact = precise * offset
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  floating
fn selectPrecise(wideUnsigned:Tensor[uint64,?],wideSigned:Tensor[int64,?],narrowUnsigned:Tensor[uint32,?],narrowSigned:Tensor[int32,?],real:Tensor[float32,?],signedOffset:int64,unsignedOffset:uint64,precise:Tensor[float,?],offset:float) -> Tensor[float,?]:
  exact = precise * offset
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  exact
fn probe() -> Tensor[float,?] ! TaskError:
  wideUnsigned:Tensor[uint64,4] = tensor([0,18446744073709551615,9223372036854775808,9007199254740993])
  wideSigned:Tensor[int64,4] = tensor([-9223372036854775808,9223372036854775807,9007199254740993,-5])
  narrowUnsigned:Tensor[uint32,4] = tensor([0,4294967295,2147483648,16777217])
  narrowSigned:Tensor[int32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[float32,4] = tensor([1.5,2.5,3.5,4.5])
  signedOffset:int64 = -9223372036854775808
  unsignedOffset:uint64 = 18446744073709551615
  precise:Tensor[float,4] = tensor([-0.0,1.0000000000000002,4.9406564584124654e-324,1.7976931348623157e308])
  offset:float = 1
  try parallel:
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
fn main() -> int64:
  0

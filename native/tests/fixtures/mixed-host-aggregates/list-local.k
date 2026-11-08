enum List:
  End
  Next:
    value:i64
    tail:List
fn head(list:List)->i64:
  return match list:
    List.End:0
    List.Next {value,tail}:value
fn firstTwo(list:List)->i64:
  return match list:
    List.End:0
    List.Next {value,tail}:value+head(tail)
fn build()->i64:
  tail=List.Next {value:7,tail:List.End}
  list=List.Next {value:11,tail:tail}
  return firstTwo(list)
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn build() on devices.gpu()

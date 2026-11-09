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
fn main()->i64 ! TaskError:
  tail=List.Next {value:7,tail:List.End}
  list=List.Next {value:11,tail:tail}
  return try parallel:
    try await spawn firstTwo(list) on devices.gpu()

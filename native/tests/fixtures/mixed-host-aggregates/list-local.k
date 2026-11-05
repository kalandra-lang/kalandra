enum List:
  End
  Next:
    value:int
    tail:List
fn head(list:List)->int:
  match list:
    List.End:0
    List.Next {value,tail}:value
fn firstTwo(list:List)->int:
  match list:
    List.End:0
    List.Next {value,tail}:value+head(tail)
fn build()->int:
  tail=List.Next {value:7,tail:List.End}
  list=List.Next {value:11,tail:tail}
  firstTwo(list)
fn main()->int ! TaskError:
  try parallel:
    try await spawn build() on devices.gpu()

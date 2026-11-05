enum List:
  End
  Next:
    value:int
    tail:List
fn make(n:int)->List:
  tail=List.Next {value:7,tail:List.End}
  List.Next {value:n,tail:tail}
fn head(list:List)->int:
  match list:
    List.End:0
    List.Next {value,tail}:value
fn score(list:List)->int:
  match list:
    List.End:0
    List.Next {value,tail}:value+head(tail)
fn main()->int ! TaskError:
  try parallel:
    list=try await spawn make(11) on devices.gpu()
    score(list)

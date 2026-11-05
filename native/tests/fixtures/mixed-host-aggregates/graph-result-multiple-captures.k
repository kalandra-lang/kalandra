enum Tree:
  End
  Node:
    left:[int]
    right:[int]
    tail:Tree
fn convert(start:int,first:Tree,middle:int,second:Tree,end:int)->Tree:
  if start*100+middle*10+end==123:
    match first:
      Tree.End:Tree.End
      Tree.Node {left,right,tail}:
        match second:
          Tree.End:Tree.End
          Tree.Node {left:other,right:alias,tail:unused}:
            Tree.Node {left:left,right:alias,tail:first}
  else:
    Tree.End
fn inspect(tree:Tree)->int:
  match tree:
    Tree.End:0
    Tree.Node {left,right,tail}:
      match tail:
        Tree.End:0
        Tree.Node {left:original,right:other,tail:unused}:
          left[0]*100+right[0]*10+original[0]+other[0]
fn main()->int ! TaskError:
  values=[1,2,3]
  separate=[1,2,3]
  first=Tree.Node {left:values,right:separate,tail:Tree.End}
  second=Tree.Node {left:separate,right:values,tail:Tree.End}
  try parallel:
    returned=try await spawn convert(1,first,2,second,3) on devices.gpu()
    values[0]=9
    inspect(returned)

enum Tree:
  End
  Branch:
    value:int
    left:Tree
    right:Tree
fn head(node:Tree)->int:
  match node:
    Tree.End:0
    Tree.Branch {value,left,right}:value
fn total(node:Tree)->int:
  match node:
    Tree.End:0
    Tree.Branch {value,left,right}:value+head(left)+head(right)
fn build(n:int)->int:
  leaf=Tree.Branch {value:7,left:Tree.End,right:Tree.End}
  root=Tree.Branch {value:11,left:leaf,right:leaf}
  sum=0
  i=0
  while i<n:
    next=Tree.Branch {value:i,left:root,right:leaf}
    sum=sum+total(next)
    i=i+1
  sum+total(root)
fn main()->int ! TaskError:
  try parallel:
    try await spawn build(300) on devices.gpu()

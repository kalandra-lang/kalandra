enum Node:
  End
  Branch:
    value:int
    children:[Node]
fn score(node:Node)->int:
  match node:
    Node.End:1
    Node.Branch {value,children}:value+length(children)
fn device(node:Node, index:int)->int:
  match node:
    Node.End:0
    Node.Branch {value,children}:value+score(children[index])+score(children[0])
fn make()->Node:
  leaf=Node.Branch {value:7,children:[Node.End]}
  values=generate(500,fn(i):leaf)
  Node.Branch {value:11,children:values}
fn main()->int ! TaskError:
  root=make()
  try parallel:
    try await spawn device(root,499) on devices.gpu()

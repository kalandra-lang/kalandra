enum Node:
  End
  Branch:
    value:int
    children:[Node]
fn score(node:Node)->int:
  match node:
    Node.End:1
    Node.Branch {value,children}:value+length(children)
fn work(n:int)->int:
  children=[Node.End,Node.Branch {value:5,children:[Node.End]}]
  branch=Node.Branch {value:7,children:children}
  original=[branch,Node.End,branch]
  view=slice(original,1,3)
  joined=concat(view,original)
  total=0
  for i in range(n):
    unused=[branch,Node.Branch {value:i,children:joined}]
    total=total+score(unused[1])
  total+score(joined[0])+score(joined[1])+score(joined[4])
fn main()->int ! TaskError:
  try parallel:
    try await spawn work(300) on devices.gpu()

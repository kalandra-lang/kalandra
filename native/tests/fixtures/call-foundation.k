fn count(n:int,a:int)->int:
  if n==0:a
  else:count(n-1,a+1)
fn even(n:int)->Bool:
  if n==0:true
  else:odd(n-1,false)
fn odd(n:int,unused:Bool)->Bool:
  if n==0:false
  else:even(n-1)
fn jump(n:int,a:int)->int:
  if n==0:a
  else:
    next=if n%2==0:jump
    else:other
    next(n-1,a+1)
fn other(n:int,a:int)->int:
  if n==0:a
  else:
    next=if n%2==0:other
    else:jump
    next(n-1,a+1)
fn walk(n:int,a:int,f:fn(int)->int)->int:
  if n==0:a
  else:walk(n-1,f(a),f)
fn make(offset:int)->fn(int)->int:
  fn(value:int):value+offset
fn series(n:int)->float64:
  if n==0:1.0
  else:series(n-1)*1.000000001+0.25
fn faultTail(n:int)->int:
  if n==0:1/(n-n)
  else:faultTail(n-1)
fn faultDeep(n:int)->int:
  if n==0:1/(n-n)
  else:faultDeep(n-1)+1
type Pair:
  left:int
  right:int
  pad0:int
  pad1:int
  pad2:int
  pad3:int
  pad4:int
  pad5:int
fn recordTail(n:int,a:int)->Pair:
  if n==0:Pair {left:a,right:42,pad0:0,pad1:1,pad2:2,pad3:3,pad4:4,pad5:5}
  else:recordTail(n-1,a+1)
fn pair(n:int)->Pair:
  recordTail(n,0)
fn wide(n:int,a:int,b:int,c:int,d:int,e:int,f:int,g:int,h:int,i:int)->int:
  if n==0:a+b+c+d+e+f+g+h+i
  else:wide(n-1,b,c,d,e,f,g,h,i,a+1)
fn run(kind:int,n:int)->int:
  if kind==0:count(n,0)
  else:
    if kind==1:
      if even(n):1
      else:0
    else:
      if kind==2:jump(n,0)
      else:
        if kind==3:walk(n,0,make(3))
        else:
          if kind==4:int(series(n))
          else:
            if kind==5:faultTail(n)
            else:
              if kind==6:faultDeep(n)
              else:
                if kind==8:wide(n,0,1,2,3,4,5,6,7,8)
                else:
                  value=pair(n)
                  next=pair(n+1)
                  value.left+next.right
fn main()->int:
  run(0,1000000)

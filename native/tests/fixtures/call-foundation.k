fn count(n:i64,a:i64)->i64:
  return if n==0:a
  else:count(n-1,a+1)
fn even(n:i64)->bool:
  return if n==0:true
  else:odd(n-1,false)
fn odd(n:i64,unused:bool)->bool:
  return if n==0:false
  else:even(n-1)
fn jump(n:i64,a:i64)->i64:
  return if n==0:a
  else:
    next=if n%2==0:jump
    else:other
    next(n-1,a+1)
fn other(n:i64,a:i64)->i64:
  return if n==0:a
  else:
    next=if n%2==0:other
    else:jump
    next(n-1,a+1)
fn walk(n:i64,a:i64,f:fn(i64)->i64)->i64:
  return if n==0:a
  else:walk(n-1,f(a),f)
fn make(offset:i64)->fn(i64)->i64:
  return fn(value:i64):return value+offset
fn series(n:i64)->f64:
  return if n==0:1.0
  else:series(n-1)*1.000000001+0.25
fn faultTail(n:i64)->i64:
  return if n==0:1/(n-n)
  else:faultTail(n-1)
fn faultDeep(n:i64)->i64:
  return if n==0:1/(n-n)
  else:faultDeep(n-1)+1
type Pair:
  left:i64
  right:i64
  pad0:i64
  pad1:i64
  pad2:i64
  pad3:i64
  pad4:i64
  pad5:i64
fn recordTail(n:i64,a:i64)->Pair:
  return if n==0:Pair {left:a,right:42,pad0:0,pad1:1,pad2:2,pad3:3,pad4:4,pad5:5}
  else:recordTail(n-1,a+1)
fn pair(n:i64)->Pair:
  return recordTail(n,0)
fn wide(n:i64,a:i64,b:i64,c:i64,d:i64,e:i64,f:i64,g:i64,h:i64,i:i64)->i64:
  return if n==0:a+b+c+d+e+f+g+h+i
  else:wide(n-1,b,c,d,e,f,g,h,i,a+1)
fn run(kind:i64,n:i64)->i64:
  return if kind==0:count(n,0)
  else:
    if kind==1:
      if even(n):1
      else:0
    else:
      if kind==2:jump(n,0)
      else:
        if kind==3:walk(n,0,make(3))
        else:
          if kind==4:i64(series(n))
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
fn main()->i64:
  return run(0,1000000)

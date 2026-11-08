type Input:
  kind:i64
  seed:i64
  rounds:i64
type Output:
  value:i64
fn transform(offset:i64)->fn(i64)->i64:
  return fn(value:i64): return (value*13+offset)%1000003
fn walk(f:fn(i64)->i64,value:i64,rounds:i64)->i64:
  return if rounds == 0:value
  else:walk(f,f(value),rounds-1)
fn arrays(values:[i64],rounds:i64)->i64:
  return if rounds == 0:
    values[0]+values[1]+values[2]+values[3]+values[4]+values[5]+values[6]+values[7]
  else:
    next=map(values,fn(value:i64): return (value*13+7)%1000003)
    arrays(next,rounds-1)
fn swap(left:[i64],right:[i64],rounds:i64)->i64:
  return if rounds == 0:left[0]+right[0]
  else:swap(right,map(left,fn(value:i64): return value+1),rounds-1)
fn trim(values:[i64],rounds:i64)->i64:
  return if rounds != 0:trim(slice(values,1,length(values)),rounds-1)
  else:values[0]
fn worker(input:Input)->Output:
  value=if input.kind == 0:
    walk(transform(input.seed+7),input.seed,input.rounds)
  else:
    seed=input.seed
    original=[seed,seed+1,seed+2,seed+3,seed+4,seed+5,seed+6,seed+7]
    result=if input.kind == 1:arrays(original,input.rounds)
    else:
      if input.kind == 2:swap(original,original,input.rounds)
      else:trim(original,input.rounds%8)
    result+original[0]
  return Output {value:value}
fn main()->i64 ! TaskError:
  return try parallel:
    output=try await spawn worker(Input {kind:0,seed:17,rounds:100000})
    output.value

fn arrayRounds(values:[i64],rounds:i64)->f64:
  return if rounds == 0:
    f64(values[0]+values[1]+values[2]+values[3]+values[4]+values[5]+values[6]+values[7])
  else:
    next=map(values,fn(value:i64): return (value*13+7)%1000003)
    arrayRounds(next,rounds-1)

fn arrays(seed:i64,rounds:i64)->f64:
  return arrayRounds([seed,seed+1,seed+2,seed+3,seed+4,seed+5,seed+6,seed+7],rounds)

fn makeTransform(offset:i64)->fn(i64)->i64:
  return fn(value): return (value*13+offset)%1000003

fn applyRounds(transform:fn(i64)->i64,value:i64,rounds:i64)->f64:
  return if rounds == 0:
    f64(value)
  else:
    applyRounds(transform,transform(value),rounds-1)

fn closure(seed:i64,rounds:i64)->f64:
  return applyRounds(makeTransform(seed+7),seed,rounds)

fn main()->f64:
  return arrays(17,16)

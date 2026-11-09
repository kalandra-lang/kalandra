fn walk0(value:i64,rounds:i64)->f64:
  return if rounds==0:f64(value)
  else:walk0((value*13+7)%1000003,rounds-1)
fn probe0(seed:i64,rounds:i64)->f64:
  return walk0(seed,rounds)
fn walk1(value:i64,rounds:i64)->f64:
  return if rounds==0:f64(value)
  else:walk1((value*13+7)%1000003,rounds-1)
fn probe1(seed:i64,rounds:i64)->f64:
  return walk1(-1,rounds)
fn walk2(value:i64,rounds:i64)->f64:
  return if rounds==0:f64(value)
  else:walk2((value*(-13)+7)%1000003,rounds-1)
fn probe2(seed:i64,rounds:i64)->f64:
  return walk2(seed,rounds)
fn walk3(value:i64,rounds:i64)->f64:
  return if rounds==0:f64(value)
  else:walk3((value*13-65536)%1000003,rounds-1)
fn probe3(seed:i64,rounds:i64)->f64:
  return walk3(seed,rounds)
fn walk4(value:i64,rounds:i64)->f64:
  return if rounds==0:f64(value)
  else:walk4((value*13+7)%1000003,rounds-1)
fn probe4(seed:i64,rounds:i64)->f64:
  return walk4(seed*65536,rounds)
fn walk5(value:i64,rounds:i64)->f64:
  return if rounds==0:f64(value)
  else:walk5((value*13+7)%(-1000003),rounds-1)
fn probe5(seed:i64,rounds:i64)->f64:
  return walk5(seed,rounds)
fn walk6(value:i64,rounds:i64)->f64:
  return if rounds==0:f64(value)
  else:walk6((value*9223372036854775807+7)%1000003,rounds-1)
fn probe6(seed:i64,rounds:i64)->f64:
  return walk6(seed,rounds)
fn main()->f64:
  return probe0(17,16)

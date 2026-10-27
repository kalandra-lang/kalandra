#[cfg(array)]
fn recurrence(values: Vec<i64>, rounds: i64) -> f64 {
    if rounds==0 {return values.iter().sum::<i64>() as f64;}
    let next=values.iter().map(|x|(x*13+7)%1000003).collect();
    recurrence(next,rounds-1)
}
#[cfg(array)]
#[unsafe(no_mangle)]
pub extern "C" fn broad_entry(seed:i64,rounds:i64)->f64 {
    recurrence(vec![ARRAY_VALUES],rounds)
}
#[cfg(any(affine,polynomial))]
fn recurrence(x:f64,offset:f64,rounds:i64)->f64 {
    if rounds==0{return x;}
    #[cfg(affine)] let next=x*0.5+offset;
    #[cfg(polynomial)] let next=x*1.015625+offset;
    recurrence(next,offset,rounds-1)
}
#[cfg(any(affine,polynomial))]
#[unsafe(no_mangle)]
pub extern "C" fn broad_entry(seed:i64,rounds:i64)->f64 {
    #[cfg(affine)] let (x,offset)=(seed as f64*0.25,seed as f64*0.0625);
    #[cfg(polynomial)] let (x,offset)=(seed as f64*0.015625,seed as f64*0.00390625);
    recurrence(x,offset,rounds)
}
#[cfg(euclid)]
fn gcd(a:i64,b:i64)->i64 {if b==0 {a}else{gcd(b,a%b)}}
#[cfg(euclid)]
fn recurrence(x:i64,seed:i64,rounds:i64)->f64 {
    if rounds==0{return x as f64;}
    recurrence(gcd(x*13+7,seed*7+11)+seed,seed,rounds-1)
}
#[cfg(euclid)]
#[unsafe(no_mangle)]
pub extern "C" fn broad_entry(seed:i64,rounds:i64)->f64 {recurrence(seed,seed,rounds)}

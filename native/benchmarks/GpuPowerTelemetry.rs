use std::time::{SystemTime,UNIX_EPOCH};
fn stamp()->u128 {SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos()}
fn main()->Result<(),Box<dyn std::error::Error>> {
 let samples=std::env::args().nth(1).unwrap_or("120".into()).parse::<u32>()?;
 let mut sampler=macmon::Sampler::new()?;
 for _ in 0..samples {
  let start=stamp();let m=sampler.get_metrics(1000)?;let end=stamp();
  println!("{}",serde_json::json!({"start_ns":start,"end_ns":end,"gpu_freq_mhz":m.gpu_freq_mhz,"gpu_active_ratio":m.gpu_active_ratio,"gpu_power_w":m.gpu_power,"gpu_temp_c":m.temp.gpu_temp_avg}));
 }
 Ok(())
}

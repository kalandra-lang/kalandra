type Job:
  numbers:[fn(int)->int]
  strings:[fn(String)->int]
fn apply(job:Job)->int:
  job.numbers[0](2)+job.strings[0]("hi")
fn main()->int ! TaskError:
  values=[38]
  job=Job {numbers:[fn(value):values[0]+value],strings:[fn(value):length(value)]}
  try parallel:
    try await spawn apply(job)

# Bias-free single-head unmasked pre-RMSNorm encoder block, unit RMS gain.
# This is a complete block, not a tokenizer, KV cache, or generation engine.
fn block(x:Tensor[float32,?,?],wq:Tensor[float32,?,?],wk:Tensor[float32,?,?],wv:Tensor[float32,?,?],wo:Tensor[float32,?,?],up:Tensor[float32,?,?],down:Tensor[float32,?,?],epsilon:float32,scale:float32)->Tensor[float32,?,?]:
  norm=rmsNormRows(x,epsilon)
  q=matmul(norm,wq)
  k=matmul(norm,wk)
  v=matmul(norm,wv)
  probabilities=softmaxRows(matmul(q,transpose(k))*scale)
  residual=x+matmul(matmul(probabilities,v),wo)
  feed=rmsNormRows(residual,epsilon)
  residual+matmul(relu(matmul(feed,up)),down)

fn main()->int64:
  0

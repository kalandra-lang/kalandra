"""Independent float32 operation/tree oracle and float64 forward-error checks.

No BLAS is used for the strict oracle. Every product and adjacent pair is
rounded separately; complete output matrices are checked, including timed sizes.
"""
import functools
import numpy as np

def inputs(m,d,pattern):
    def make(shape,stream):
        z=np.arange(np.prod(shape),dtype=np.uint32)+np.uint32((0x9e3779b9*(stream+1))&0xffffffff)
        z=(z^(z>>16))*np.uint32(0x85ebca6b)
        z=(z^(z>>13))*np.uint32(0xc2b2ae35)
        z=z^(z>>16)
        denominator=(127 if stream else 29) if pattern else (128 if stream else 32)
        return ((z%17).astype(np.int32)-8).astype(np.float32).reshape(shape)/np.float32(denominator)
    return make((m,d),0),make((d,4*d),1),make((4*d,d),2)

def tree(values,axis=-1):
    values=np.moveaxis(np.asarray(values,dtype=np.float32),axis,-1).copy()
    while values.shape[-1]>1:
        n=values.shape[-1]
        paired=values[...,:n-1:2]+values[...,1:n:2]
        values=np.concatenate((paired,values[...,-1:]),axis=-1) if n&1 else paired
    return values[...,0]

def strict_matmul(a,b):
    out=np.empty((a.shape[0],b.shape[1]),np.float32)
    for row in range(0,a.shape[0],4):
        for col in range(0,b.shape[1],16):
            terms=a[row:row+4,:,None]*b[None,:,col:col+16]
            out[row:row+4,col:col+16]=tree(terms,axis=1)
    return out

@functools.lru_cache(maxsize=1)
def oracle(m,d,pattern):
    x,up,down=inputs(m,d,pattern)
    hidden=np.maximum(strict_matmul(x,up),np.float32(0))
    cells=strict_matmul(hidden,down)+x
    scalar=float(tree(cells.reshape(-1)))
    # BLAS may use FMA/reassociation. Standard gamma bounds propagate the first
    # GEMM error through 1-Lipschitz ReLU and the second GEMM/residual addition.
    # float64 oracle's own smaller error is covered by doubling these bounds.
    a,b,c=(v.astype(np.float64) for v in (x,up,down))
    high_hidden=np.maximum(a@b,0)
    high=high_hidden@c+a
    u=2.0**-24
    gamma=lambda k:k*u/(1-k*u)
    e1=gamma(2*d)*(np.abs(a)@np.abs(b))
    bound=2*(e1@np.abs(c)+gamma(8*d+2)*((np.abs(high_hidden)+e1)@np.abs(c)+np.abs(a)))
    return cells,scalar,high,bound

def assess(m,d,pattern,row,strict):
    want,scalar,high,bound=oracle(m,d,pattern)
    actual=np.asarray(row['cells'],np.float32).reshape(m,d)
    assert np.isfinite(actual).all() and np.isfinite(row['result'])
    error=np.abs(actual.astype(np.float64)-high)
    assert np.all(error<=bound), (m,d,pattern,float(np.max(error-bound)))
    same=bool(np.array_equal(actual.view(np.uint32),want.view(np.uint32)))
    if strict:
        assert same, (m,d,pattern,'strict cell bits disagree',np.argwhere(actual.view(np.uint32)!=want.view(np.uint32))[:4].tolist())
        assert np.float32(row['result']).view(np.uint32)==np.float32(scalar).view(np.uint32)
    # The scalar is also validated against this backend's complete output cells.
    assert np.float32(row['result']).view(np.uint32)==tree(actual.reshape(-1)).view(np.uint32)
    return dict(cells=m*d,strict_bit_match=same,max_absolute_error=float(error.max()),
                max_error_bound=float(bound.max()),max_bound_fraction=float(np.max(error/np.maximum(bound,1e-300))),
                expected=row['result'])

def self_test():
    # Independently check vectorized product/tree semantics using scalar Python
    # binary64 (exact for each float32 product/pair), rounded at every operation.
    x,up,_=inputs(3,7,1)
    def scalar_sum(v):
        v=[np.float32(z) for z in v]
        while len(v)>1:
            v=[np.float32(float(v[i])+float(v[i+1])) for i in range(0,len(v)-1,2)]+(v[-1:] if len(v)&1 else [])
        return v[0]
    a=strict_matmul(x,up)
    for r in range(3):
        for col in range(28):
            expected=scalar_sum([np.float32(float(x[r,k])*float(up[k,col])) for k in range(7)])
            assert a[r,col].view(np.uint32)==expected.view(np.uint32)
    cells,scalar,_,_=oracle(3,7,1)
    row=dict(cells=cells.reshape(-1).tolist(),result=scalar)
    assess(3,7,1,row,True)
    row['cells'][0]+=1
    try:assess(3,7,1,row,True)
    except AssertionError:return dict(scalar_product_cells=84,corruption_rejected=True)
    raise AssertionError('corrupted cell accepted')

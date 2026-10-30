"""Explicit OpenBLAS ctypes bindings and idiomatic NumPy, with queried thread control."""
import ctypes as c,json,os,sys,time
import numpy as np
DTYPE=sys.argv[1];NUMPY=sys.argv[2]=='numpy'
kind,n,iterations,rounds,threads,pattern,dump=map(int,sys.argv[3:])
assert DTYPE in ('float32','float64') and 0<=kind<=2 and 0<n<=4194304 and (dump or iterations>0 and rounds>0)
lib=c.CDLL('/opt/homebrew/opt/openblas/lib/libopenblas.dylib')
lib.openblas_set_num_threads(threads);assert lib.openblas_get_num_threads()==threads
# Prove that this NumPy installation resolves BLAS to the controlled library.
handle=c.CDLL(np._core._multiarray_umath.__file__)
assert c.cast(handle.openblas_get_num_threads,c.c_void_p).value==c.cast(lib.openblas_get_num_threads,c.c_void_p).value
F=c.c_float if DTYPE=='float32' else c.c_double;P=c.POINTER(F);prefix='s' if DTYPE=='float32' else 'd'
dot=getattr(lib,'cblas_'+prefix+'dot');dot.argtypes=[c.c_int,P,c.c_int,P,c.c_int];dot.restype=F
copy=getattr(lib,'cblas_'+prefix+'copy');copy.argtypes=[c.c_int,P,c.c_int,P,c.c_int]
axpy=getattr(lib,'cblas_'+prefix+'axpy');axpy.argtypes=[c.c_int,F,P,c.c_int,P,c.c_int]
gemm=getattr(lib,'cblas_'+prefix+'gemm');gemm.argtypes=[c.c_int]*6+[F,P,c.c_int,P,c.c_int,F,P,c.c_int]
count=n*n if kind==2 else n
i=np.arange(count,dtype=np.int64)
a=((i*37)%101-50).astype(DTYPE)/np.array(37,dtype=DTYPE) if pattern else (i%17-8).astype(DTYPE)/8
b=((i*43)%101-50).astype(DTYPE)/np.array(29,dtype=DTYPE) if pattern else (i%13-6).astype(DTYPE)/8
del i
ones=None if NUMPY else np.ones(count,dtype=DTYPE)
def pointer(v):return v.ctypes.data_as(P)
def kernel():
    if kind==0:
        return float(np.dot(a,b) if NUMPY else dot(n,pointer(a),1,pointer(b),1)),None
    if NUMPY:
        out=np.empty_like(a)
        if kind==1:
            np.multiply(a,3,out=out);np.add(out,b,out=out)
        else:np.matmul(a.reshape(n,n),b.reshape(n,n),out=out.reshape(n,n))
        return float(np.sum(out,dtype=DTYPE)),out
    out=np.empty_like(a)
    if kind==1:
        copy(n,pointer(b),1,pointer(out),1);axpy(n,3,pointer(a),1,pointer(out),1)
    else:gemm(101,111,111,n,n,n,1,pointer(a),n,pointer(b),n,0,pointer(out),n)
    return float(dot(count,pointer(out),1,pointer(ones),1)),out
if dump:
    result,cells=kernel();print(json.dumps(dict(result=result,threads=threads,cells=[] if cells is None else cells.tolist())))
else:
    for r in range(-2,rounds):
        checksum=0.0;start=time.perf_counter_ns()
        for _ in range(iterations):
            value,out=kernel();checksum+=value;del out
        elapsed=time.perf_counter_ns()-start
        print(json.dumps(dict(round=r,ns=elapsed,checksum=checksum,threads=threads)))
i=np.arange(count,dtype=np.int64)
expected_a=((i*37)%101-50).astype(DTYPE)/np.array(37,dtype=DTYPE) if pattern else (i%17-8).astype(DTYPE)/8
expected_b=((i*43)%101-50).astype(DTYPE)/np.array(29,dtype=DTYPE) if pattern else (i%13-6).astype(DTYPE)/8
assert np.array_equal(a,expected_a) and np.array_equal(b,expected_b)

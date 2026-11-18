"""Compile the standalone native reference with installed NVRTC, without host headers."""
import argparse
import ctypes as c
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('source',type=Path)
p.add_argument('output',type=Path)
p.add_argument('--library',default='/usr/local/cuda/lib64/libnvrtc.so')
a=p.parse_args()
l=c.CDLL(a.library)
def check(code):
    if code:raise RuntimeError('NVRTC result '+str(code))
program=c.c_void_p()
l.nvrtcCreateProgram.argtypes=[c.POINTER(c.c_void_p),c.c_char_p,c.c_char_p,c.c_int,c.c_void_p,c.c_void_p]
l.nvrtcCompileProgram.argtypes=[c.c_void_p,c.c_int,c.POINTER(c.c_char_p)]
for name in ('nvrtcGetProgramLogSize','nvrtcGetPTXSize'):
    getattr(l,name).argtypes=[c.c_void_p,c.POINTER(c.c_size_t)]
for name in ('nvrtcGetProgramLog','nvrtcGetPTX'):
    getattr(l,name).argtypes=[c.c_void_p,c.c_void_p]
l.nvrtcDestroyProgram.argtypes=[c.POINTER(c.c_void_p)]
check(l.nvrtcCreateProgram(c.byref(program),a.source.read_bytes(),b'reference.cu',0,None,None))
try:
    options=[b'--gpu-architecture=compute_120',b'--fmad=false',b'--prec-div=true',b'--std=c++17']
    code=l.nvrtcCompileProgram(program,len(options),(c.c_char_p*len(options))(*options))
    size=c.c_size_t();check(l.nvrtcGetProgramLogSize(program,c.byref(size)))
    log=c.create_string_buffer(size.value);check(l.nvrtcGetProgramLog(program,log));print(log.value.decode())
    check(code)
    check(l.nvrtcGetPTXSize(program,c.byref(size)))
    output=c.create_string_buffer(size.value);check(l.nvrtcGetPTX(program,output))
    a.output.write_bytes(output.value)
finally:check(l.nvrtcDestroyProgram(c.byref(program)))

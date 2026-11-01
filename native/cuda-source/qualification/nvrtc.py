"""Project-local device-only NVRTC compiler helper. Installs nothing."""
import ctypes as c
import hashlib
import json
from pathlib import Path

def compile_ptx(source,output_path,chip='80',fmad=False,library='/usr/local/cuda/lib64/libnvrtc.so.13',extra_options=()):
    # NVRTC dlopens its matching builtins by SONAME. An explicitly supplied
    # redistributable SDK is not necessarily on the process loader search path.
    # Preload the exact adjacent version without changing LD_LIBRARY_PATH or
    # accidentally mixing this NVRTC with ambient toolkit installations.
    library_path=Path(library).resolve()
    lib=c.CDLL(str(library_path));ptr=c.c_void_p
    lib.nvrtcVersion.argtypes=[c.POINTER(c.c_int),c.POINTER(c.c_int)]
    lib.nvrtcVersion.restype=c.c_int
    major=c.c_int();minor=c.c_int()
    if lib.nvrtcVersion(c.byref(major),c.byref(minor)):
        raise RuntimeError('Cannot query NVRTC version')
    builtins=library_path.parent/f'libnvrtc-builtins.so.{major.value}.{minor.value}'
    if not builtins.is_file():
        raise RuntimeError('Matching NVRTC builtins are missing: '+str(builtins))
    builtins_library=c.CDLL(str(builtins))

    signatures={'nvrtcCreateProgram':[c.POINTER(ptr),c.c_char_p,c.c_char_p,c.c_int,ptr,ptr],
     'nvrtcCompileProgram':[ptr,c.c_int,ptr], 'nvrtcGetProgramLogSize':[ptr,c.POINTER(c.c_size_t)],
     'nvrtcGetProgramLog':[ptr,ptr], 'nvrtcGetPTXSize':[ptr,c.POINTER(c.c_size_t)],
     'nvrtcGetPTX':[ptr,ptr], 'nvrtcDestroyProgram':[c.POINTER(ptr)]}
    for name,args in signatures.items():fn=getattr(lib,name);fn.argtypes=args;fn.restype=c.c_int
    program=ptr()
    def check(status):
     if status:raise RuntimeError('NVRTC error '+str(status))
    check(lib.nvrtcCreateProgram(c.byref(program),source,b'kalandra-device.cu',0,None,None))
    try:
     options=[f'--gpu-architecture=compute_{chip}'.encode(),b'--std=c++17',b'--fmad=true' if fmad else b'--fmad=false',b'--ftz=false']
     options.extend(v.encode() for v in extra_options)
     arguments=(c.c_char_p*len(options))(*options)
     status=lib.nvrtcCompileProgram(program,len(options),arguments)
     size=c.c_size_t();check(lib.nvrtcGetProgramLogSize(program,c.byref(size)))
     log=c.create_string_buffer(size.value);check(lib.nvrtcGetProgramLog(program,log))
     output_path.parent.mkdir(parents=True,exist_ok=True);output_path.with_suffix('.compile.log').write_bytes(log.value)
     if status:raise RuntimeError(log.value.decode())
     check(lib.nvrtcGetPTXSize(program,c.byref(size)))
     output=c.create_string_buffer(size.value);check(lib.nvrtcGetPTX(program,output))
     ptx=output.value
     # C++ headers contain an unused __kind_tf32 type tag. Reject actual
     # reduced-precision instructions, not that non-arithmetic symbol.
     assert all(op not in ptx for op in (b'.tf32',b'.f16',b'.bf16',b'mma.sync',b'wgmma.',b'tcgen05.',b'.extern .func'))
     output_path.write_bytes(ptx)
     output_path.with_suffix('.json').write_text(json.dumps({'options':[v.decode() for v in options],
        'sourceSHA256':hashlib.sha256(source).hexdigest(),'ptxSHA256':hashlib.sha256(ptx).hexdigest(),
        'gpuExecution':False},indent=2)+'\n')
     return {'sourceSHA256':hashlib.sha256(source).hexdigest(),'ptxSHA256':hashlib.sha256(ptx).hexdigest()}
    finally:check(lib.nvrtcDestroyProgram(c.byref(program)))

if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--chip',choices=('80','120'),default='80')
    p.add_argument('--library',default='/usr/local/cuda/lib64/libnvrtc.so.13')
    a=p.parse_args()
    compile_ptx(a.source.read_bytes(),a.output,a.chip,library=a.library)

"""Extract the compiler's prepared program without reconstructing its graph."""
import re

def gpu_source(block):
    """Place the canonical block on Metal; its dataflow stays compiler generated."""
    assert block.count('fn main()')==1
    return block.split('fn main()')[0]+'''fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    cpu=devices.cpu()
    x:Tensor[float32,2,2]=tensor([[1,2],[3,4]])
    w:Tensor[float32,2,2]=tensor([[1,0],[0,1]])
    up:Tensor[float32,2,8]=tensor([1,0,1,0,1,0,1,0,0,1,0,1,0,1,0,1],[2,8])
    down:Tensor[float32,8,2]=tensor([1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1],[8,2])
    a=try await transfer(x,gpu)
    b=try await transfer(w,gpu)
    c=try await transfer(up,gpu)
    d=try await transfer(down,gpu)
    y=try await spawn block(a,b,b,b,b,c,d,0.00001,0.70710677) on gpu
    try await transfer(y,cpu)
'''

def header(host,prepared=True):
    loads=re.findall(r'auto (kernel\d+)=(r\.load[^;]+);if\(!kernel\d+\)\{[^\n]+',host)
    assert loads
    scalars=host[host.index('auto scalars0='):host.index('auto target=',host.index('auto scalars0='))]
    code=host[host.index('auto operands0='):host.index('auto prepared=',host.index('auto operands0='))]
    slots=list(dict.fromkeys(re.findall(r'f\.v\d+',scalars)))
    assert 1<=len(slots)<=2
    for old,new in zip(slots,('epsilon','scale')):scalars=scalars.replace(old,new)
    match=re.search(r'prepareNumericProgram\(7u,code,(\d+)u,kalandra::NumericType::Float32,false\)',host);assert match
    captures=re.search(r'executeNumericProgram\(task,[^\n]+?,std::array<std::uint64_t,\d+>\{([^}]*)\}\)',host);assert captures
    if not prepared:
        text='struct LegacyProgram {std::array<m::Kernel,'+str(len(loads))+'> kernels;\nLegacyProgram(m::Runtime&r,std::string directory){auto target=r.device();ffn::check(bool(target));\n'
        for name,call in loads:text+=f'auto {name}='+call.replace('f.directory','directory')+f';ffn::check(bool({name}));\n'
        text+='kernels={'+','.join('*'+name for name,_ in loads)+'};}\n'
        text+='TaskHandle launch(m::Runtime&r,TaskCore&core,GroupHandle group,const std::array<m::Buffer,7>&inputs,float epsilon,float scale){\n'
        for i in range(len(loads)):text+=f'auto*kernel{i}=&kernels[{i}];\n'
        return text+scalars+code+f'return r.numericProgram(core,group,inputs,code,{match[1]}u,kalandra::NumericType::Float32,false);\n}}\n}};\n'
    text='struct Program {m::NumericExecutable executable; double preparationNanoseconds=0; double pipelineNanoseconds=0;\nProgram(m::Runtime&r,std::string directory){auto target=r.device();ffn::check(bool(target));auto pipelineStart=std::chrono::steady_clock::now();\n'
    for name,call in loads:text+=f'auto {name}='+call.replace('f.directory','directory')+f';ffn::check(bool({name}));\n'
    # Captures are placeholders at preparation, rebound on every invocation.
    text+=scalars.replace('epsilon','0.0f').replace('scale','0.0f')+code
    text+='pipelineNanoseconds=ffn::elapsed(pipelineStart);auto preparationStart=std::chrono::steady_clock::now();\n'
    text+=f'auto prepared=r.prepareNumericProgram(7,code,{match[1]}u,kalandra::NumericType::Float32,false);ffn::check(bool(prepared));executable=*prepared;preparationNanoseconds=ffn::elapsed(preparationStart);}}\n'
    text+='TaskHandle launch(m::Runtime&r,TaskCore&core,GroupHandle group,const std::array<m::Buffer,7>&inputs,float epsilon,float scale){\n'+scalars
    count=len([v for v in captures[1].split(',') if v])
    text+=f'return r.executeNumericProgram(core,group,executable,inputs,std::array<std::uint64_t,{count}>{{'+captures[1]+'});\n}\n'
    text+='std::expected<m::Buffer,Failure> launch(m::Runtime&r,ClosedTaskRunner&runner,const std::array<m::Buffer,7>&inputs,float epsilon,float scale){\n'+scalars
    text+=f'return r.executeNumericProgram(runner,executable,inputs,std::array<std::uint64_t,{count}>{{'+captures[1]+'});\n}\n};\n'
    return text

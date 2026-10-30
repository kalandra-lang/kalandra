"""Combine two-column reuse with smaller strict reduction panels.

Retains the exact balanced tree by combining adjacent 32-leaf subtrees, including
all outer carry roots through K=4096. Copied shaders only; production unchanged.
"""
from pathlib import Path
base=Path(__file__).with_name('wide_grid_screen.py');body=base.read_text()
body=body.replace('sources=[Path(__file__),header',"sources=[Path(__file__),Path(__file__).with_name('wide_grid_leaf32_screen.py'),header")
needle=" (b/'study.metal').write_text(source)";assert body.count(needle)==1
patch=''' helper=function(source,'inline float2 kalandra_hardware_eight_pair32(')
 source=source.replace(helper,helper.replace('row*64','row*32'))
 kernel=function(source,'kernel void kalandra_matmul_hardware_pair32(')
 changed=kernel.replace('av[1024],bv[2048]','av[512],bv[1024]').replace('blocks=k/64','blocks=k/32').replace('first=block*64','first=block*32')
 changed=changed.replace('at<1024;','at<512;',1).replace('at/64','at/32').replace('at%64','at%32').replace('at<2048;','at<1024;')
 changed=re.sub(r'      float2 r[4-7]=[^;]+;\\n','',changed)
 changed=changed.replace('((r0+r1)+(r2+r3))+((r4+r5)+(r6+r7))','(r0+r1)+(r2+r3)')
 changed=changed.replace('s6=0.0f;','s6=0.0f,s7=0.0f;').replace('else s6=s5+value;','else { value=s5+value; if(!(block&64u)) s6=value; else s7=s6+value; }')
 changed=changed.replace('output.values[row*n+col]=value.x;', 'if(blocks&128u) value=used?s7+value:s7;\\n    output.values[row*n+col]=value.x;')
 assert changed!=kernel and 'r7=' not in changed
 source=source.replace(kernel,changed)
'''
body=body.replace(needle,patch+needle)
exec(compile(body,str(base),'exec'),dict(__file__=str(base),__name__='__main__'))

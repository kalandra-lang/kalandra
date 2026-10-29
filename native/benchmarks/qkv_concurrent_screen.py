"""Control whether concurrent ordinary dispatches recover the batched Q/K/V gain.

Reuse the exact qualification and timing protocol from qkv_dispatch_screen.py,
with a copied driver. No production shaders or runtime are changed.
"""
from pathlib import Path

base=Path(__file__).with_name('qkv_dispatch_screen.py')
text=base.read_text()
needle="header=p.ROOT/'native/metal-source/HardwareShaders.h';driver=Path(__file__).with_name('QkvDispatchStudy.mm')"
replacement="""header=p.ROOT/'native/metal-source/HardwareShaders.h'
original_driver=Path(__file__).with_name('QkvDispatchStudy.mm')
driver=build/'ConcurrentDriver.mm'
driver_text=original_driver.read_text()
old='else {auto e=[cb computeCommandEncoder];if(mode=="current")encode(e,0,out[0],true);else for(unsigned p=0;p<3;++p)encode(e,p,out[p],false);[e endEncoding];}'
new='else {auto e=[cb computeCommandEncoderWithDispatchType:mode=="current"?MTLDispatchTypeConcurrent:MTLDispatchTypeSerial];for(unsigned p=0;p<3;++p)encode(e,p,out[p],false);[e endEncoding];}'
assert driver_text.count(old)==1
candidate_text=driver_text.replace(old,new)
if driver.exists(): assert driver.read_text()==candidate_text
else: driver.write_text(candidate_text)
"""
assert text.count(needle)==1;text=text.replace(needle,replacement)
needle='sources=[Path(__file__),driver,header,'
assert text.count(needle)==1
text=text.replace(needle,"sources=[Path(__file__),Path(__file__).with_name('qkv_dispatch_screen.py'),original_driver,driver,header,")
# Retain this wrapper's distinct contract in the evidence, not the base module's.
start=text.index('import argparse')
exec(compile(text[start:],str(base),'exec'),globals())

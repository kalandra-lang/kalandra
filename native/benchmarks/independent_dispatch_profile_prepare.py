"""Build optional diagnostics for the frozen copied-runtime experiment."""
import argparse,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--build',type=Path,required=True);a=parser.parse_args();b=a.build.resolve();fair=Path(__file__).parent/'fair'
source=(fair/'TransformerClosedMetal.cpp').read_text();needle='m::Options options;';assert source.count(needle)==1
profile=source.replace(needle,needle+' options.profile = true;')
arena='#include <cstdlib>\n'+profile.replace(needle+' options.profile = true;',needle+' options.profile = true; options.parameterArena = std::getenv("KALANDRA_PROBE_NO_ARENA") == nullptr;')
for name,binary,text in [('Profile.cpp','profile',profile),('ProfileArena.cpp','profile-arena',arena)]:
 path=b/name
 if path.exists():assert path.read_text()==text
 else:path.write_text(text)
 if (b/binary).exists():continue  # Never overwrite frozen evidence.
 p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0','-I'+str(b),'-I'+str(p.ROOT/'native/metal'),'-I'+str(fair),path,b/'metal.o',p.ROOT/'build/native/native/libkalandra-task-core-optimized.a','-framework','Foundation','-framework','Metal','-o',b/binary])
print('Profile sources verified; missing diagnostic binaries built')

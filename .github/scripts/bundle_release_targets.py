"""Insert all five CPU runtime datasets before signing/notarization."""
import os
from pathlib import Path
import shutil
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'native/packaging'))
from RuntimeBundles import assemble
from collect_native_platforms import collect

collect(rehearsal=not os.environ['GITHUB_REF'].startswith('refs/tags/'))
assets=Path('release-assets')
with tempfile.TemporaryDirectory(prefix='bundled release ') as temporary:
    output=Path(temporary)
    assemble(assets.resolve(),output)
    for file in output.iterdir():
        if file.is_file() and file.name.startswith('kalandra-native-'):
            shutil.copyfile(file,assets/file.name)

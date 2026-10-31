"""Remove private SDK input copies and the ephemeral receiver key."""
import os
from pathlib import Path
import shutil

folder = Path('private-sdk')
if folder.exists(): shutil.rmtree(folder)
Path(os.environ['RUNNER_TEMP'], 'sdk-private.pem').unlink(missing_ok=True)

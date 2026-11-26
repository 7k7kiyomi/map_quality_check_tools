"""Install the verified road topology build with a recoverable backup."""
from datetime import datetime
from pathlib import Path
import hashlib
import json
import os
import shutil

root = Path(__file__).resolve().parents[2]
candidate = root / 'plugin_map_quality_check_tools_road_topology_fix.dll'
manifest = json.loads((root / 'tmp/road_topology_build/manifest.json').read_text(encoding='utf-8'))
target = Path('E:/LTZK/plugins/plugin_map_quality_check_tools.dll')
def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()
assert candidate.is_file() and target.is_file()
assert digest(candidate) == manifest['dll_sha256']
for name, sha in manifest['sources'].items():
    assert digest(root / name) == sha, name
data = candidate.read_bytes()
assert '[ROAD_TOPOLOGY]'.encode('utf-16le') in data
assert 'mActionimagemap'.encode('utf-16le') in data
assert b'SE_ImageMapCheckDialog' in data
# Windows will reject the replacement if the destination is still locked.
stamp = datetime.now().strftime('%Y%m%d_%H%M%S')
backup = target.with_name(target.name + '.before_road_topology_' + stamp + '.bak')
stage = target.with_name(target.name + '.road_topology_install.tmp')
assert not backup.exists() and not stage.exists()
old_hash = digest(target)
shutil.copy2(str(target), str(backup))
assert digest(backup) == old_hash
shutil.copy2(str(candidate), str(stage))
assert digest(stage) == manifest['dll_sha256']
os.replace(str(stage), str(target))
assert digest(target) == manifest['dll_sha256']
print(json.dumps({'installed': str(target), 'sha256': digest(target),
                  'backup': str(backup), 'backup_sha256': old_hash}, ensure_ascii=True))

"""Link separately named release DLL from the exact objects used by regression."""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess

root=Path(__file__).resolve().parents[2]
os.chdir(str(root))
out=root/'tmp/road_topology_build'
names=['ReadShp','ReadShp_interface','QualityCheckConfig','map_check_algorithms','map_check_task_manager']
sources=[root/'ui_class'/(name+'.cpp') for name in names]+[root/'ui_class/road_topology_network.h']
for name in names:
    assert (out/(name+'.obj')).stat().st_mtime>=(root/'ui_class'/(name+'.cpp')).stat().st_mtime
for name in ['ReadShp','ReadShp_interface']:
    assert (out/(name+'.obj')).stat().st_mtime>=(root/'ui_class/road_topology_network.h').stat().st_mtime
fingerprints={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
link=(root/'tmp/full_plugin_build/link_full.rsp').read_text()
link=re.sub(r'/OUT:"[^"]+"','/OUT:"plugin_map_quality_check_tools_road_topology_fix.dll"',link)
link=re.sub(r'/IMPLIB:"[^"]+"','/IMPLIB:"tmp/road_topology_build/plugin.lib"',link)
link=re.sub(r'/PDB:"[^"]+"','/PDB:"tmp/road_topology_build/plugin.pdb"',link)
for name in names:
    old='"tmp\\full_plugin_build\\'+name+'.obj"'
    assert old in link
    link=link.replace(old,'"tmp/road_topology_build/'+name+'.obj"')
(out/'plugin_link.rsp').write_text(link)
template=(root/'tmp/full_plugin_build/compile_release.bat').read_text()
(out/'plugin_build.bat').write_text(template.split('@cl ',1)[0]+'@link @tmp\\road_topology_build\\plugin_link.rsp\n@exit /b %errorlevel%\n')
subprocess.check_call(['cmd','/c',str(out/'plugin_build.bat')])
assert fingerprints=={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
dll=root/'plugin_map_quality_check_tools_road_topology_fix.dll'
manifest={'sources':fingerprints,'dll_sha256':hashlib.sha256(dll.read_bytes()).hexdigest(),'dll_bytes':dll.stat().st_size,'installed':False,
    'build_variant':'Existing local test configuration: MAP_QUALITY_DISABLE_IMAGE_QA; use normal project configuration for full production release.'}
(out/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({'dll':str(dll),'bytes':dll.stat().st_size,'sha256':manifest['dll_sha256']},indent=2))

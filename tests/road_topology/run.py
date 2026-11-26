"""Compile changed production sources and run the real QGIS regression executable."""
import os
from pathlib import Path
import re
import subprocess
import sys

root=Path(__file__).resolve().parents[2]
os.chdir(str(root))
out=root/'tmp/road_topology_build'
out.mkdir(exist_ok=True)
template=(root/'tmp/full_plugin_build/compile_release.bat').read_text()
command=template.split('@cl ',1)[1].splitlines()[0]
command=command.replace('/MP4 ','').replace('/DQT_NO_CAST_TO_ASCII ','')
command=command.split('@tmp\\full_plugin_build\\release.rsp')[0]
names=['ReadShp','ReadShp_interface','QualityCheckConfig','map_check_algorithms','map_check_task_manager']
sources=['ui_class\\'+name+'.cpp' for name in names]+['tests\\road_topology\\test.cpp']
headers=list((root/'ui_class').glob('road_topology*.h'))
newest_header=max([p.stat().st_mtime for p in headers] or [0])
sources=[p for p in sources if not (out/(Path(p.replace('\\','/')).stem+'.obj')).exists()
    or max((root/p.replace('\\','/')).stat().st_mtime,newest_header if Path(p.replace('\\','/')).stem in ['ReadShp','ReadShp_interface'] else 0)>(out/(Path(p.replace('\\','/')).stem+'.obj')).stat().st_mtime]
command+=' '.join(sources)+' /MP4 /Fo:tmp\\road_topology_build\\'
link=(root/'tmp/full_plugin_build/link_full.rsp').read_text()
link=link.replace('/DLL ','')
link=re.sub(r'/OUT:"[^"]+"','/OUT:"tmp/road_topology_build/test.exe"',link)
link=re.sub(r'/IMPLIB:"[^"]+"','',link)
for name in names:
    old='"tmp\\full_plugin_build\\'+name+'.obj"'
    assert old in link,name+' missing from link inputs'
    link=link.replace(old,'"tmp/road_topology_build/'+name+'.obj"')
link+='\n"tmp/road_topology_build/test.obj"\n'
(out/'link.rsp').write_text(link)
batch=template.split('@cl ',1)[0]+(('@cl '+command+'\n@if errorlevel 1 exit /b 1\n') if sources else '')
batch+='@link @tmp\\road_topology_build\\link.rsp\n@exit /b %errorlevel%\n'
(out/'build.bat').write_text(batch)
if '--no-build' not in sys.argv:
    subprocess.check_call(['cmd','/c',str(out/'build.bat')])
env=os.environ.copy()
env['PATH']=';'.join([r'E:\LTZK\bin',r'E:\LTT',r'E:\LTZK\QGIS_32815\bin',r'E:\LTZK\OSGeo4W_32815\bin',r'E:\LTZK\OSGeo4W_32815\apps\Qt5\bin',r'E:\LTZK\plugins',env['PATH']])
env['QT_QPA_PLATFORM']='offscreen'
env['PROJ_LIB']=r'E:\LTZK\OSGeo4W_32815\share\proj'
env['GDAL_DATA']=r'E:\LTZK\OSGeo4W_32815\share\gdal'
log=out/('actual_replay.log' if '--actual' in sys.argv else 'regression.log')
with log.open('wb') as stream:
    process=subprocess.Popen([str(out/'test.exe')]+sys.argv[1:],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    for line in process.stdout:
        stream.write(line);stream.flush();sys.stdout.buffer.write(line);sys.stdout.buffer.flush()
    raise SystemExit(process.wait())

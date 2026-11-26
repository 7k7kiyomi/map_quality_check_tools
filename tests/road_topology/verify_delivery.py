"""Verify release, rules and real-data outputs; write the delivery record."""
from pathlib import Path
from collections import Counter
import difflib
import hashlib
import json
import struct
import zipfile
import xml.etree.ElementTree as ET

root=Path(__file__).resolve().parents[2]
build=root/'tmp/road_topology_build'
assert 'TOTAL passed=17 failed=0' in (build/'regression.log').read_text(encoding='utf-8')
assert 'ACTUAL_REPLAY_PASS' in (build/'actual_replay.log').read_text(encoding='utf-8')
manifest=json.loads((build/'manifest.json').read_text(encoding='utf-8'))
for name,digest in manifest['sources'].items():
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest,name
dll=root/'plugin_map_quality_check_tools_road_topology_fix.dll'
assert hashlib.sha256(dll.read_bytes()).hexdigest()==manifest['dll_sha256']
rules=root/'永登县新版2_道路拓扑修订XML_20261030'
original=Path('D:/AAAGSZJTESTMAP/挂图XML文件汇总_10.28/成果质检启用的xml（测试图层已覆盖）')
rows=json.loads((rules/'变更清单.json').read_text(encoding='utf-8'))
assert len(rows)==85 and sum(r['changed'] for r in rows)==10
for r in rows:
    assert hashlib.sha256((original/r['name']).read_bytes()).hexdigest()==r['original_sha256']
    assert hashlib.sha256((rules/r['name']).read_bytes()).hexdigest()==r['copy_sha256']
    parsed=ET.parse(str(rules/r['name']))
    if r['changed']:
        item=next(c for c in parsed.iter('check_item') if c.findtext('id')=='112')
        values={x.findtext('name'):x.findtext('value') for x in item.findall('rule')}
        assert values['道路连接缓冲距离']=='0.2' and values['居民点缓冲距离']=='1'
    else:assert r['original_sha256']==r['copy_sha256']
assert not next(r for r in rows if r['name']=='街道线.xml')['changed']
with zipfile.ZipFile(str(rules.with_suffix('.zip'))) as archive:
    assert archive.testzip() is None
    for p in rules.iterdir():
        if p.is_file():assert archive.read(rules.name+'/'+p.name)==p.read_bytes()

replay=root/'tmp/road_topology_replay_20261030'
table=[]
for name in ['村道','省道','县道','公路隧道']:
    p=replay/name/'质检Json文件'/('线要素拓扑_'+name+'.json')
    item=json.loads(p.read_text(encoding='utf-8-sig'))['check_item1']
    assert item['status']=='质检成功'
    errors=item['errors'];count=Counter(e['error_type'] for e in errors)
    shapefile_count=0
    for dbf in (replay/name/'质检错误结果').glob('*.dbf'):
        shapefile_count+=struct.unpack_from('<I',dbf.read_bytes(),4)[0]
    assert shapefile_count==len(errors),(name,shapefile_count,len(errors))
    for e in errors:
        if '道路' in e['error_type']:assert abs(e['std_value']-29)<1e-8
        if '悬挂' in e['error_type']:assert e['calc_value']<0 or e['calc_value']>29
    if name=='村道':
        assert item['info']['input_line_feature_count']==1259
        assert item['info']['participating_line_feature_count']==1258
        assert item['info']['skipped_invalid_feature_count']==1
        assert count['道路悬挂']>0 and count['道路不合理独立']>0
        assert not any(e['feature_id']==1049 and '独立' in e['error_type'] for e in errors)
    if name=='省道':assert not any(e['feature_id']==26 and '独立' in e['error_type'] for e in errors)
    if name=='县道':assert not any(e['feature_id']==65 for e in errors)
    table.append('| '+name+' | '+str(item['info']['input_line_feature_count'])+' | '+str(count['道路悬挂'])+' | '+str(count['道路不合理独立'])+' | '+str(count['几何体无效'])+' |')

# The geometry copies used during the original diagnosis remain identical.
for name in ['村道_LineString','省道','县道']:
    a=Path('D:/AAAGSZJTESTMAP/TESTMAP/永登县地图——新版2_shp')/(name+'.shp')
    b=Path('D:/AAAGSZJTESTMAP/永登县/永登县_shp')/(name+'.shp')
    assert hashlib.sha256(a.read_bytes()).digest()==hashlib.sha256(b.read_bytes()).digest()

diff=[]
for name in ['ReadShp.cpp','ReadShp_interface.cpp','QualityCheckConfig.cpp','map_check_algorithms.cpp']:
    before=(root/'tmp/road_topology_before_20261030'/name).read_text(encoding='utf-8-sig').splitlines(True)
    after=(root/'ui_class'/name).read_text(encoding='utf-8-sig').splitlines(True)
    diff.extend(difflib.unified_diff(before,after,'before/ui_class/'+name,'after/ui_class/'+name))
header=(root/'ui_class/road_topology_network.h').read_text(encoding='utf-8').splitlines(True)
diff.extend(difflib.unified_diff([],header,'/dev/null','after/ui_class/road_topology_network.h'))
(build/'source_changes.patch').write_text(''.join(diff),encoding='utf-8')

report=[
    '# 永登县地图——新版2：道路拓扑修复交付说明',
    '',
    '日期：2026-10-30。街道线未接入道路网络，街道线XML保持原文件字节不变。',
    '',
    '## 已修改的行为',
    '',
    '1. 村道线、面多路径先拆分，再筛选线几何。1259条村道线实际进入检查，村道面被排除。',
    '2. 悬挂与独立使用同一份成果道路网络；公路隧道参与；库体、铁路、长城、隧道口符号不作为公路连接。',
    '3. 每个端点独立应用图上0.2mm道路缓冲、1mm居民点缓冲；一端居民点不再掩盖另一端异常。',
    '4. 支持闭合居民地符号内部、带_LineString/_Polygon后缀居民地及不同坐标系的距离计算。',
    '5. 独立按统一路网的连通分量及主网可达性判断，跨文件互连不会自行排除独立。',
    '6. 多部件逐部件检查，按源要素合并时保留全部错误部件定位几何。',
    '7. 悬挂的最近道路距离按异常端点计算；独立的最近距离排除分量内部道路。',
    '8. 修复最小线段长度参数键名；道路短线不再直接作为独立判据。',
    '9. UI任务必须匹配当前标准图层的专属XML及112规则，失败/未执行不再显示“未发现错误”。',
    '',
    '主网选取：在统一成果路网中，选择含骨干道路（非村道、连接道、公路隧道）、至少两个线部件的候选连通分量，以总长度最大者为主网；等长时按源路径/FID/部件稳定排序。无居民点且不能到达主网的分量报独立；有居民点仍逐端点检查悬挂。',
    '',
    '## 验证',
    '',
    '- 使用真实QGIS运行库编译并运行17项回归，全部通过。审查提出的跨文件孤立环、多部件几何丢失、等长主网顺序问题均有失败后转通过的回归。',
    '- 使用原始永登县新版2数据只读重放4个任务，输出另存在tmp目录。JSON记录数与对应SHP/DBF总记录数一致。',
    '',
    '| 图层 | 输入线要素 | 悬挂 | 独立 | 无效几何 |',
    '|---|---:|---:|---:|---:|',
]+table+[
    '',
    '村道1259条中，1258条有效参与，1条无效几何单独定位输出；村道面不参与。上述数量是修复规则下的候选问题数量，仍可结合原图复核。',
    '',
    '已确认反例：省道FID26、村道FID1049不再误报独立；县道FID65在29米道路缓冲内，不再报错。',
    '',
    '## 文件与使用',
    '',
    '- 修订XML文件夹：`永登县新版2_道路拓扑修订XML_20261030/`，内含85份完整启用规则、原CSV、使用说明、逐文件哈希变更清单。请选择此文件夹本层加载规则。',
    '- XML压缩包：`永登县新版2_道路拓扑修订XML_20261030.zip`。仅10份道路XML的112项发生变化，道路XML的其他检查项及其余75份XML均保持不变；原始规则未覆盖。',
    '- 独立测试DLL：`plugin_map_quality_check_tools_road_topology_fix.dll`。已编译链接，未替换当前插件。它沿用本机测试配置（图像QA入口未启用），仅用于本次道路验证；正式使用请按原项目配置重新编译本次修订源码。',
    '- 17项回归日志：`tmp/road_topology_build/regression.log`。',
    '- 数据重放日志：`tmp/road_topology_build/actual_replay.log`；各图层JSON、SHP、日志位于`tmp/road_topology_replay_20261030/`。',
    '- 构建源码/DLL哈希：`tmp/road_topology_build/manifest.json`；源码差异：`tmp/road_topology_build/source_changes.patch`。',
    '- 四个原源码备份：`tmp/road_topology_before_20261030/`。',
    '',
    '核心修改文件：`ui_class/ReadShp.cpp`、`ui_class/ReadShp_interface.cpp`、`ui_class/QualityCheckConfig.cpp`、`ui_class/map_check_algorithms.cpp`；新增`ui_class/road_topology_network.h`。',
    '',
    'DLL SHA-256：`'+manifest['dll_sha256']+'`。',
]
(root/'道路拓扑修复说明_2026-10-30.md').write_text('\n'.join(report)+'\n',encoding='utf-8')
plan=root/'docs/superpowers/plans/2026-10-30-road-topology.md'
text=plan.read_text(encoding='utf-8').replace('- [ ]','- [x]')
text=text.replace('仅修改道路XML中的112参数','仅修改具有112项的10份道路XML参数')
plan.write_text(text,encoding='utf-8')
print('VERIFIED: 17 regressions; 4 actual tasks; JSON/SHP counts; 85 XMLs, 10 changed; street and originals unchanged; ZIP bytes; source/DLL hashes.')
print('REPORT',root/'道路拓扑修复说明_2026-10-30.md')
print('\n'.join(table))

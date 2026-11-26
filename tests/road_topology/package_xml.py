"""Copy the enabled rule set; change road item 112 only; verify and ZIP it."""
from pathlib import Path
import hashlib
import json
import re
import shutil
import zipfile
import xml.etree.ElementTree as ET

root=Path(__file__).resolve().parents[2]
source=Path('D:/AAAGSZJTESTMAP/挂图XML文件汇总_10.28/成果质检启用的xml（测试图层已覆盖）')
target=root/'永登县新版2_道路拓扑修订XML_20261030'
target.mkdir(exist_ok=True)
roads={'高速公路','快速路','国道','省道','县道','乡道','专用公路','连接道','村道','公路隧道'}
changes=[]
manifest=[]
for p in sorted(source.glob('*.xml')):
    original=p.read_bytes()
    text=original.decode('utf-8-sig')
    changed=False
    if p.stem in roads:
        sections=list(re.finditer(r'<check_item>.*?</check_item>',text,re.S))
        match=next((m for m in sections if re.search(r'<id>\s*112\s*</id>',m.group())),None)
        if match:
            block=match.group()
            for name,value in [('悬挂线容差','0.2'),('孤立距离','0.2'),('连通性容差','0.2'),('网络缓冲距离','0.2')]:
                pattern=r'(<rule>\s*<name>'+re.escape(name)+r'</name>.*?<value>)[^<]*(</value>)'
                block,n=re.subn(pattern,lambda m:m.group(1)+value+m.group(2),block,flags=re.S)
                assert n==1,(p.name,name,n)
            related='高速公路、快速路、国道、省道、县道、乡道、专用道、连接道、村道、公路隧道、自然村、行政村、乡镇级驻地、县级驻地、街区面'
            block,n=re.subn(r'(<rule>\s*<name>关联图层</name>.*?<value>)[^<]*(</value>)',lambda m:m.group(1)+related+m.group(2),block,flags=re.S)
            assert n==1
            newline='\r\n' if '\r\n' in text else '\n'
            additions=''
            for name,value,description in [('道路连接缓冲距离','0.2','成果道路端点与其他成果道路的图上允许间隙，公路隧道参与构网'),('居民点缓冲距离','1','对每个端点独立判断合理居民点终止，不豁免另一个端点')]:
                additions+=newline+'            <rule>'+newline+'                <name>'+name+'</name>'+newline+'                <description>'+description+'</description>'+newline+'                <value>'+value+'</value>'+newline+'                <unit>毫米</unit>'+newline+'            </rule>'+newline
            block=block.replace('</check_item>',additions+'        </check_item>')
            text=text[:match.start()]+block+text[match.end():]
            # Other check items must be semantically identical.
            old=ET.fromstring(original);new=ET.fromstring(text)
            old_items=[ET.tostring(x,encoding='unicode') for x in old.iter('check_item') if x.findtext('id')!='112']
            new_items=[ET.tostring(x,encoding='unicode') for x in new.iter('check_item') if x.findtext('id')!='112']
            assert old_items==new_items,p.name
            changed=True;changes.append(p.name)
    data=(b'\xef\xbb\xbf' if original.startswith(b'\xef\xbb\xbf') else b'')+text.encode('utf-8') if changed else original
    (target/p.name).write_bytes(data)
    manifest.append({'name':p.name,'changed':changed,'original_sha256':hashlib.sha256(original).hexdigest(),'copy_sha256':hashlib.sha256(data).hexdigest()})
assert set(Path(n).stem for n in changes)==roads,(changes,roads)
csv=source.parent/'图层对应关系配置表_UTF8.csv'
shutil.copy2(str(csv),str(target/csv.name))
(target/'变更清单.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
instructions=(
    '永登县地图——新版2 道路拓扑修订规则\n\n'
    '本文件夹包含完整启用XML规则集及原CSV。请选择此文件夹本层加载XML。\n'
    '仅修改以下10个道路XML的112检查项：\n')+'\n'.join(changes)+(
    '\n\n道路连接缓冲：图上0.2毫米；居民点缓冲：图上1毫米。\n'
    '1:145000时分别为29米和145米。\n'
    '街道线.xml及铁路、水系等其他XML保持原文件字节不变。\n'
    '道路XML内其他检查项保持不变；在建高速公路原无112项，不增加检查项。\n'
    '原始XML未被覆盖。需配合本次源码重新编译的正式插件使用。\n'
    '另附测试DLL仅用于本次道路验证，沿用本机测试配置，未启用图像QA入口。\n'
    '库体道路不作为成果道路连接；公路隧道参与；隧道口符号不作为道路。\n'
    '独立分量依据统一成果路网的主网可达性判定，居民点豁免仍逐端点执行。\n')
(target/'使用说明.txt').write_text(instructions,encoding='utf-8-sig')
archive=target.with_suffix('.zip')
with zipfile.ZipFile(str(archive),'w',zipfile.ZIP_DEFLATED) as z:
    for p in sorted(target.iterdir()):
        if p.is_file():z.write(str(p),target.name+'/'+p.name)
with zipfile.ZipFile(str(archive)) as z:assert z.testzip() is None
for row in manifest:
    assert hashlib.sha256((source/row['name']).read_bytes()).hexdigest()==row['original_sha256']
    if not row['changed']:assert row['original_sha256']==row['copy_sha256']
print(json.dumps({'folder':str(target),'zip':str(archive),'xml_count':len(manifest),'modified':changes,'street_unchanged':next(x for x in manifest if x['name']=='街道线.xml')['changed']==False},ensure_ascii=False,indent=2))

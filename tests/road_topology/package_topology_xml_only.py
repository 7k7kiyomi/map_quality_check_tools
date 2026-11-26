"""Package only the ten revised road topology XML files."""
from pathlib import Path
import hashlib
import json
import re
import zipfile
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
source = root / '永登县新版2_道路拓扑修订XML_20261030'
target = root / '永登县新版2_仅线要素拓扑修订XML_20261030'
manifest = json.loads((source / '变更清单.json').read_text(encoding='utf-8'))
revised = [row for row in manifest if row['changed']]
expected = {name + '.xml' for name in (
    '高速公路', '快速路', '国道', '省道', '县道',
    '乡道', '专用公路', '连接道', '村道', '公路隧道')}
assert {row['name'] for row in revised} == expected
target.mkdir(exist_ok=True)
assert not set(p.name for p in target.iterdir()) - expected
for row in revised:
    data = (source / row['name']).read_bytes()
    assert hashlib.sha256(data).hexdigest() == row['copy_sha256']
    assert row['copy_sha256'] != row['original_sha256']
    items = [x for x in ET.fromstring(data).iter('check_item')
             if x.findtext('id') == '112']
    assert len(items) == 1
    rules = {x.findtext('name'): x.findtext('value')
             for x in items[0].iter('rule')}
    assert rules['道路连接缓冲距离'] == '0.2'
    assert rules['居民点缓冲距离'] == '1'
    text = data.decode('utf-8-sig')
    blocks = list(re.finditer(r'<check_item>.*?</check_item>', text, re.S))
    assert len(blocks) == len(list(ET.fromstring(data).iter('check_item')))
    for block in reversed(blocks):
        item = ET.fromstring(block.group())
        if item.findtext('id') != '112':
            text = text[:block.start()] + text[block.end():]
    text = re.sub(r'\s*<!--\s*质检项\d+：.*?-->', '', text, flags=re.S)
    output = (b'\xef\xbb\xbf' if data.startswith(b'\xef\xbb\xbf') else b'') + text.encode('utf-8')
    parsed = ET.fromstring(output)
    remaining = list(parsed.iter('check_item'))
    assert len(remaining) == 1 and remaining[0].findtext('id') == '112'
    def signature(node):
        return (node.tag, node.attrib, (node.text or '').strip(),
                [signature(child) for child in node])
    assert signature(remaining[0]) == signature(items[0])
    (target / row['name']).write_bytes(output)
assert {p.name for p in target.iterdir()} == expected
archive = target.with_suffix('.zip')
with zipfile.ZipFile(str(archive), 'w', zipfile.ZIP_DEFLATED) as z:
    for p in sorted(target.iterdir()):
        z.write(str(p), target.name + '/' + p.name)
with zipfile.ZipFile(str(archive)) as z:
    assert z.testzip() is None
    assert len(z.namelist()) == 10
    for p in target.iterdir():
        assert z.read(target.name + '/' + p.name) == p.read_bytes()
print(json.dumps({'folder': str(target), 'zip': str(archive),
                  'verified_xml_count': 10, 'files': sorted(expected)},
                 ensure_ascii=False, indent=2))

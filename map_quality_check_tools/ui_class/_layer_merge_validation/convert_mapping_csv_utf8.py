from pathlib import Path
import csv
import hashlib
import io
import os

path = Path(r"D:\Desktop\成果质检规则文件_挂图11.11\图层对应关系配置表_UTF8.csv")
original = path.read_bytes()
text = original.decode("gbk")
rows = list(csv.reader(io.StringIO(text)))
expected = ["标准图层", "gdb图层", "实际图层", "实际注记图层", "对应注记层", "图例字段"]
assert rows[0] == expected and len(rows) == 138
assert all(len(row) == 6 for row in rows)
encoded = text.encode("utf-8")
assert not encoded.startswith(b"\xef\xbb\xbf")
temporary = path.with_name(path.name + ".utf8.tmp")
temporary.write_bytes(encoded)
assert temporary.read_bytes().decode("utf-8").splitlines() == text.splitlines()
os.replace(temporary, path)
print(f"rows={len(rows)} bytes={len(encoded)} sha256={hashlib.sha256(encoded).hexdigest()}")

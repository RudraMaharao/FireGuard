"""Verify preserved sensor headers and the 56-column serial training contract."""
from pathlib import Path
import hashlib,json,re,ast
root=Path(__file__).resolve().parents[1]
for name,expected in json.loads((root/'tests/brain_sha256.json').read_text()).items():
    assert hashlib.sha256((root/'firmware/main/brain'/name).read_bytes()).hexdigest()==expected,name
s=(root/'firmware/main/sensors.cpp').read_text()
header=s[s.index('#define CSV_HEADER'):s.index('void logRow(')]
fields=''.join(re.findall(r'"([^"\n]*)"',header)).split(',')
ref=(root/'training/sensor/serial_capture.py').read_text()
# Capture the literal parenthesized header tuple from the original capture tool.
assert len(fields)==56, len(fields)
expected=next(ast.literal_eval(n.value) for n in ast.parse(ref).body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=="HEADER" for t in n.targets))
assert ",".join(fields)==expected,"CSV order differs from original capture script"
assert 'unused_mq7' in fields and 'dev7' not in fields
assert '#include "brain/model_a.h"' not in s and 'TwoSensorModel modelA' in s
assert 'fu.w[CH_CG] = 0;' in s
print('Archived scoring headers unchanged; revision-2 serial schema has 56 positions with explicit unavailable CO fields.')

# Expand storage only into unassigned flash on the verified 16 MB board.
parts={}
for row in (root/'firmware/partitions.csv').read_text().splitlines():
    if not row.strip() or row.lstrip().startswith('#'):continue
    cols=[v.strip() for v in row.split(',')]
    size=int(cols[4][:-1])*1024 if cols[4].endswith('K') else int(cols[4],0)
    parts[cols[0]]=(int(cols[3],0),size)
assert parts['nvs']==(0x9000,24*1024)
assert parts['phy_init']==(0xf000,4096)
assert parts['factory']==(0x10000,0x480000)
assert parts['storage']==(0x490000,0x370000)
assert parts['fg_config']==(0x800000,64*1024)
spans=sorted((start,start+size,name) for name,(start,size) in parts.items())
assert all(a[1]<=b[0] for a,b in zip(spans,spans[1:]))
assert spans[-1][1]<=16*1024*1024
print('Dedicated 64 KiB settings partition is non-overlapping; every legacy partition retains its offset/size.')

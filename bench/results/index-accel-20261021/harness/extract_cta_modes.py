#!/usr/bin/env python3
import argparse
import csv
import json
from pathlib import Path
import re
import sqlite3

parser=argparse.ArgumentParser()
parser.add_argument('database',type=Path)
parser.add_argument('--out',type=Path,required=True)
args=parser.parse_args()
db=sqlite3.connect(args.database)
db.row_factory=sqlite3.Row
tables={row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
names={row['id']:row['value'] for row in db.execute('SELECT id,value FROM StringIds')}
kernel_table=next((name for name in ('CUPTI_ACTIVITY_KIND_KERNEL','CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL') if name in tables),None)
if not kernel_table or 'NVTX_EVENTS' not in tables:
    raise RuntimeError('required kernel/NVTX tables missing')
kernels=[dict(row) for row in db.execute(f'SELECT * FROM {kernel_table}')]
rows=[]
for raw in db.execute('SELECT * FROM NVTX_EVENTS'):
    event=dict(raw)
    text=event.get('text') or names.get(event.get('textId'),'')
    match=re.fullmatch(r'itopk=(\d+)/(single|batch)',text)
    if not match or event.get('end') is None:
        continue
    captured=[]
    for kernel in kernels:
        if event['start'] <= kernel['start'] and kernel['end'] <= event['end']:
            name=next((names[kernel[key]] for key in ('demangledName','name','shortName','mangledName')
                       if key in kernel and kernel[key] in names),'unknown')
            if 'cagra' in name.lower():
                captured.append(name)
    modes=set()
    for name in captured:
        low=name.lower()
        if 'single_cta' in low: modes.add('single_cta')
        if 'multi_cta' in low: modes.add('multi_cta')
        if 'multi_kernel' in low: modes.add('multi_kernel')
    rows.append({'itopk_size':int(match[1]),'path':match[2],
                 'observed_modes':sorted(modes) or ['unresolved'],
                 'cagra_kernel_count':len(captured),'kernel_names':sorted(set(captured))})
if len(rows)!=8:
    raise RuntimeError(f'expected eight labelled probe ranges, got {len(rows)}')
rows.sort(key=lambda row:(row['itopk_size'],row['path']))
args.out.mkdir(parents=True,exist_ok=True)
(args.out/'cta-modes.json').write_text(json.dumps(rows,indent=2)+'\n')
with (args.out/'cta-modes.csv').open('w',newline='') as stream:
    writer=csv.DictWriter(stream,fieldnames=['itopk_size','path','observed_modes','cagra_kernel_count'])
    writer.writeheader()
    writer.writerows({k:v for k,v in row.items() if k!='kernel_names'} for row in rows)
print(json.dumps(rows,indent=2))

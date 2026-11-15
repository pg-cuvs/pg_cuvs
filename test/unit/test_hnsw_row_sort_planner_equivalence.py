#!/usr/bin/env python3
"""Compare the patch-0006 (row-sort consumer) planner text with the canonical source; source only."""
import argparse
import hashlib
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('--canonical-header',type=Path,required=True);p.add_argument('--row-sort-src',type=Path,required=True);a=p.parse_args()
old=a.canonical_header.read_bytes();new=(a.row_sort_src/'neighbors/detail/cagra/cagra_build.cuh').read_bytes()
assert hashlib.sha256(old).hexdigest()=='87239d05766dda6f99d17cbfbc75faa9858e976fc0aa80f679f63dc4a6297c8e'
assert hashlib.sha256(new).hexdigest()=='38722f03f72efeb5a92e92ea30ebe9d87ec9b9b1afc2ca392a07de4b645c95bc'

def function(data,anchor):
 start=data.index(anchor);brace=data.index(b'{',start);depth=1;i=brace+1
 while depth:
  if data[i:i+1]==b'{':depth+=1
  if data[i:i+1]==b'}':depth-=1
  i+=1
 return data[start:i]

for name,expected in (
 (b'bool ace_check_use_disk_mode(', '58d46970192320e8cfe2465d8db5fc77536dd81a7e8dc86c400cde488d707f6b'),
 (b'void ace_validate_disk_mode_partitions(', '7b6b455f45cd64915dcb92ef6ec402eed4aac56c2d224f31656a93c183f649bb'),
):
 b=function(old,name);assert b==function(new,name) and hashlib.sha256(b).hexdigest()==expected
 print(name.decode()+': identical source body')
for name,expected in (
 ('ace_memory_guard.hpp','dc2ccb2d6920bf81d55e614181e3ceff8e7e0394eae56d0506fbb302190cf555'),
 ('consumer_row_sort_plan.hpp','07b7b724fce554cec74ee1cb779e24ced31867981f33e6afda4f2c17d98cac49'),
):
 assert hashlib.sha256((a.row_sort_src/'neighbors/detail/cagra'/name).read_bytes()).hexdigest()==expected
print('row-sort consumer planner/source dependencies: PASS (source only)')

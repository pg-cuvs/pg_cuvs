#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
# SPDX-License-Identifier: PostgreSQL
"""Execute the SDK's real upper dispatch with builder spies and kill policy mutants."""
from __future__ import annotations
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import resource
from test_upper_level_copy import COPY_MISMATCH_EXIT

ROOT=pathlib.Path(__file__).resolve().parents[3]
DETAIL=pathlib.Path("cpp/src/neighbors/detail")

def function(text: str, anchor: str) -> str:
    start=text.index(anchor);body=text.index("{",start);depth=0
    for i in range(body,len(text)):
        if text[i]=="{":depth+=1
        elif text[i]=="}":
            depth-=1
            if depth==0:return text[start:i+1]
    raise ValueError("unterminated SDK function")

def assets(source: pathlib.Path) -> None:
    folder=ROOT/"infra/cuvs-patches";m=json.loads((folder/"patchset-manifest.json").read_text())
    for entry in m["patches"]:
        assert hashlib.sha256((folder/entry["path"]).read_bytes()).hexdigest()==entry["sha256"]
    for path,entry in m["source_files"].items():
        assert hashlib.sha256((source/path).read_bytes()).hexdigest()==entry["final_sha256"],path
    recipe=(folder/"build-patched-cuvs.sh").read_text();readme=(folder/"README.md").read_text()
    assert 'for candidate_level in 0 1 2 3 4' in recipe
    assert 'source_state_matches 4' in recipe
    for entry in m["patches"]:assert entry["sha256"] in recipe and entry["sha256"] in readme
    marker=m["runtime_markers"]["upper_precision"]
    assert marker["symbol"] in recipe and marker["value"] in recipe and marker["value"] in readme
    cpp=(source/"cpp/src/neighbors/hnsw.cpp").read_text()
    assert marker["symbol"] in cpp and marker["value"] in cpp
    # Bind the spy defaults to the pinned shape constructor rather than assuming a dtype.
    pq=(source/"cpp/include/cuvs/neighbors/ivf_pq.hpp").read_text()
    for field in ["coarse_search_dtype", "internal_distance_dtype", "lut_dtype"]:
        import re
        assert re.search(r"search_params\."+field+r"\s*=\s*CUDA_R_16F",pq)
    pin=(ROOT/"scripts/verify-hnsw-sdk-plan.py").read_text()
    assert 'patchset-manifest.json' in pin and 'final_sha256' in pin

def main() -> int:
    p=argparse.ArgumentParser();p.add_argument("--source",required=True,type=pathlib.Path);p.add_argument("--compiler",default="clang++")
    p.add_argument("--hnswlib-header",required=True,type=pathlib.Path);p.add_argument("--mutations",action="store_true")
    p.add_argument("--functional-only",action="store_true");p.add_argument("--policy-header",type=pathlib.Path)
    p.add_argument("--output",type=pathlib.Path);a=p.parse_args();source=a.source.resolve()
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    commit=subprocess.check_output(["git","-C",str(source),"rev-parse","HEAD"],text=True).strip()
    assert commit=="60b19a0f62ba0bd716de349382828e43be6e2daa"
    if not a.functional_only:assets(source)
    h=(source/DETAIL/"hnsw.hpp").read_text()
    dispatch=function(h,"template <typename T>\nvoid all_neighbors_graph(")
    policy=(a.policy_header or source/DETAIL/"hnsw_precision.hpp").read_text()
    product=function((ROOT/"src/cuvs_hnsw_build_common.h").read_text(),"static inline int cuvs_hnsw_ivfpq_needs_fp32(")
    template=pathlib.Path(__file__).with_name("upper_precision_cpu.cpp.in").read_text()
    results=[]
    with tempfile.TemporaryDirectory(prefix="upper-precision-") as temp:
        d=pathlib.Path(temp)
        def copy_check(mutant=None):
            output=d/("copy-"+(mutant or "baseline")+".json")
            command=[sys.executable,str(pathlib.Path(__file__).with_name("test_upper_level_copy.py")),"--source",str(source),"--compiler",a.compiler,"--hnswlib-header",str(a.hnswlib_header),"--output",str(output)]
            if mutant:command.extend(["--mutant",mutant])
            run=subprocess.run(command,capture_output=True,text=True)
            detail=json.loads(output.read_text()) if output.is_file() else {"phase":"no-result"}
            log=run.stdout+run.stderr
            expected=COPY_MISMATCH_EXIT if mutant else 0
            if not (detail.get("phase")=="runtime" and detail.get("compile_exit")==0 and detail.get("runtime_exit")==expected and run.returncode==expected):
                if a.output:a.output.write_text(json.dumps({"passed":False,"copy_check":detail,"exit":run.returncode,"diagnostic":log},indent=2)+"\n")
                raise RuntimeError(f"copy {mutant or 'baseline'} failed: expected runtime exit {expected}; got {detail}; {log}")
            print(log,end="")
            return run.returncode,log,detail
        # Validate the unmodified gather before any mutation can count as detected.
        _,_,copy_baseline=copy_check()
        print("GREEN copy baseline runtime exit 0")
        def execute(label,body,helper):
            (d/"policy.hpp").write_text(helper)
            text=template.replace("__POLICY_HEADER__","policy.hpp").replace("__SDK_ALL_NEIGHBORS_GRAPH__",body).replace("__PRODUCT_PRECISION_POLICY__",product)
            (d/"probe.cpp").write_text(text)
            command=[a.compiler,"-std=c++20","-O0","-I"+str(source/DETAIL/"cagra"),str(d/"probe.cpp"),"-o",str(d/"probe")]
            built=subprocess.run(command,capture_output=True,text=True)
            if built.returncode:return "compile",built.returncode,built.stdout+built.stderr
            run=subprocess.run([str(d/"probe")],capture_output=True,text=True)
            return "runtime",run.returncode,run.stdout+run.stderr
        phase,rc,log=execute("original",dispatch,policy);print(log,end="")
        if phase!="runtime":raise RuntimeError("precision baseline compilation failed: "+log)
        if rc:return rc if rc>0 else 1
        if a.mutations:
            changes=[
                ("L2-factor",dispatch,policy.replace("4.0 * max_sq_norm","max_sq_norm",1)),
                ("threshold-equality",dispatch,policy.replace("< 65504.0","<= 65504.0",1)),
                ("threshold-too-large",dispatch,policy.replace("65504.0","65536.0",1)),
                ("nonfinite-zero",dispatch,policy.replace("return std::numeric_limits<double>::infinity()","return 0.0",1)),
                ("norm-minimum",dispatch,policy.replace("std::max(maximum, squared)","std::min(maximum, squared)",1)),
                ("first-row-only",dispatch,policy.replace("i < rows","i < 1",1)),
                ("force-safe",dispatch.replace("upper_precision::needs_fp32(max_sq, is_l2)","false",1),policy),
                ("force-fp32",dispatch.replace("upper_precision::needs_fp32(max_sq, is_l2)","true",1),policy),
                ("wrong-L2-metric",dispatch.replace("metric == cuvs::distance::DistanceType::L2Expanded","metric == cuvs::distance::DistanceType::InnerProduct",1),policy),
                ("NN-scans-input",dispatch.replace("  const size_t degree = neighbors.extent(1);","  const size_t degree = neighbors.extent(1);\n  volatile double unexpected = upper_precision::max_sq_norm(dataset.data_handle(), rows, dim); (void)unexpected;",1),policy),
            ]
            for field in ["coarse_search_dtype","internal_distance_dtype","lut_dtype"]:
                import re
                changed,count=re.subn(r"("+field+r"\s*=\s*)CUDA_R_32F",r"\g<1>CUDA_R_16F",dispatch,count=1);assert count==1
                changes.append(("drop-"+field,changed,policy))
            for name,body,helper in changes:
                assert body!=dispatch or helper!=policy,name
                phase,rc,log=execute(name,body,helper)
                assert phase=="runtime",(name,"mutant did not compile",log)
                assert rc!=0,(name,"mutant survived")
                results.append({"mutation":name,"phase":phase,"exit":rc,"detected":True,"diagnostic":log[-600:]})
                print("RED",name,rc)
            for name in ["raw-source","byte-destination","short-copy"]:
                rc,log,detail=copy_check(name)
                results.append({"mutation":name,"phase":detail["phase"],"compile_exit":detail["compile_exit"],"runtime_exit":detail["runtime_exit"],"exit":rc,"detected":True,"diagnostic":log[-600:]});print("RED",name,rc)
    if a.output:a.output.write_text(json.dumps({"source":str(source),"policy_and_dispatch_passed":True,"copy_baseline":copy_baseline,"mutations":results,"limitations":"GPU build/allocation APIs are spies; actual SDK policy, memory guard and complete dispatch body execute; GPU behavior validated separately"},indent=2)+"\n")
    print("PASS upper precision; mutants",len(results));return 0

if __name__=="__main__":raise SystemExit(main())

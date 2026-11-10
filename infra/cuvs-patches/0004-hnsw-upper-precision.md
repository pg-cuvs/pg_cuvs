# 0004: HNSW upper IVF-PQ precision and typed gather

## Pinned source and composition

Apply after the canonical0001~0003 patches on cuVS v26.04.00, commit `60b19a0f62ba0bd716de349382828e43be6e2daa`. The manifest binds patch and final source hashes. This patch changes private hnsw.hpp, hnsw.cpp, and adds the CPU-only hnsw_precision.hpp policy helper. It does not change CAGRA base-builder parameters or public IPC fields.

## Defect and independent causes

The memory GPU hierarchy calls all_neighbors_graph for each actual upper population. With no explicit guard budget it chooses NN-descent below ten million neighbor slots and IVF-PQ at or above that threshold. The IVF-PQ shape constructor sets coarse, internal, and LUT to FP16. Large norms can exceed the format limit.

A second defect is independent of precision: hnswlib getDataByInternalId returns char*. Patch0001 corrected only the memory destination type, leaving source iterators that count bytes and numerically convert each byte to a float coordinate. This patch counts T elements at both iterators. An actual-header copy test now models char*, detects the previous code at runtime, and catches byte-source, byte-destination, and short-copy mutants. Its old float* stand-in was incorrect.

The binary295ba88c also contains legacy diagnostic hooks not fully represented in its partial source snapshot. Canonical0001~0003 was rebuilt and compared against it before the0004 work; the merge recipe does not import those hooks. The planner verifier binds the canonical manifest source hash instead of the diagnostic snapshot hash.

## Single-factor evidence

On each actual retained344411x64 level1 population, the diagnostic harness trained one IVF-PQ index and reused it while varying only valid dtype combinations. With LUT/internal/coarse order:

| Input | 16/32/32 overlap | 32/32/32 overlap | Duplicates16/32/32 -> all32 |
|---|---:|---:|---|
| uniform |0.2146875|0.2153125|0 -> 0|
| manifold |0.000625|0.790625|10676741 -> 0|

These are original-vector standalone controls, not product native exports. Uniform did not need LUT32 to remove corruption; manifold did. Centering alone did not recover either input, while positive power-of-two scaling removed duplicate neighbors. Those controls isolate numerical range from the invalid memory gather. Native experiments with the gather defect showed high overlap in the incorrect signed-byte space while original-space overlap remained near zero; finite distances and no duplicate IDs therefore did not certify correct coordinates.

IVF-PQ approximation quality is a separate issue. On one uniform index with all dtypes32, changing only n_probes from5 to172 moved overlap0.2159375 to0.5153125. Patch0004 does not change n_probes, pq_dim, n_lists, refinement, M, or the builder threshold.

## Selected policy

Let R2 be the maximum squared row norm of the actual upper query matrix, accumulated in double. This is the user's selected conservative product policy:

```text
bound = L2 ? 4*R2 : R2  // IP and cosine use R2
use_fp32 = !(bound < 65504)
```

Thus equality and NaN/inf select all three FP32 fields. The norm scan occurs only inside the existing IVF-PQ branch. Normalized rows retain the constructor's FP16 parameters. NN-descent remains on its original path, including the budget-driven choice. The common copy correction restores original coordinates for all memory GPU hierarchies; it does not preserve bytes of previously incorrect upper graphs.

## Upstream comparison

[Upstream #2246](https://github.com/NVIDIA/cuvs/pull/2246), commit7f2e67d61a90947b5f81f9ffed952be0a4343f82 (2026-07-06), probes at most128 leading queries and switches only coarse/internal when returned distances are nonfinite. **It leaves LUT in FP16.** It is not a norm rule or a validation of every query/codeword. On the original-vector manifold control the retained LUT16 remained unsafe.

[PR #3029](https://github.com/NVIDIA/cuvs/pull/3029), code commitdbae3e0e8aba5defee360215fd212921c6b402a1 (2026-11-09), corrects both source and destination types in the memory gather. It was still open at the source audit. Patch0004 incorporates the same required typed-source behavior within this pinned patch composition.

## Observation and validation

The runtime symbol is `pgcuvs_hnsw_upper_precision_patch_id`, value `pgcuvs-hnsw-upper-precision-v1@60b19a0f62ba0bd716de349382828e43be6e2daa`. The recipe resolves it with the two older markers from one loaded DSO. An INFO record reports actual upper rows/degree, max squared norm, bound and coarse/internal/LUT choice. Existing product ivfpq_distance_dtype receipts describe the base builder and are not relabeled as upper receipts.

CPU checks compile the actual SDK policy, memory guard and full dispatch body with GPU builder argument spies. They cover threshold equality, L2 versus IP/cosine, nonfinite/late rows, all three fields, exact1e7 dispatch, and a poisoned NN input that must never be scanned. Mutations execute these paths without relying on final source hashes to detect an altered policy. Actual source copy expressions are also compiled against the validated hnswlib getter type.

GPU acceptance is recorded separately in REPORT-r3: valid upper structure, a same-population FP16-range normal SDK reference, two-build median ratio <=1.2, upper kNN memory peak, recall drop <=0.01 and p50 ratio <=1.1 at ef10/80/200/1000, and unchanged normalized/default nsw policy. CPU0.47 is not used as an IVF-PQ quality threshold. A standalone overlap or sampled memory peak is not reported as full PG CREATE completion or a hard RSS limit.

## VM-n validation of this composition

The exact recipe produced DSO SHA256 `0325efc2dee4faf8b82805e12fbe3410f77d0f0ad6338e9e8b13bd0bf5445780`. The same loaded DSO returned all three expected markers. The committed product also built against canonical planner source87239d05; a normalized10000x64 native smoke checked both hierarchy modes and both SDKs with FP16 base receipts and valid query results.

The canonical0001~0003 normal reference calls its real all_neighbors_graph directly on the actual L1 population in SDK bucket order, after a positive power-of-two scale with sR<=64. This bypasses its known invalid memory gather without modifying that SDK. Reference scales were1/32 foruniform and1/2048 formanifold. The test does not mislabel its uncorrected from_cagra byte coordinates as a normal oracle.

All344411 nodes were evaluated after the100-node diagnostic showed a small uniform sample difference. Canonical brute-force L2Unexpanded candidates were refined in original-float32-to-float64 direct L2, with stable label ties. All rows certified the64-candidate exclusion margin; the minimum margins were3633.03 uniform and584654.91 manifold. The100-node original exact sets independently matched. No nodes or favorable samples were selected.

| Input | Normal FP16-range control r1/r2 | Native0004 r1/r2 all-node overlap |
|---|---|---|
| uniform |0.20885748 /0.20888179|0.20893342 /0.20897462|
| manifold |0.71880480 /0.71714100|0.78926649 /0.78778679|

All four native level1 structures had zero duplicate/self/out-of-range/oversized edges. The native build-median ratios were1.01396669 uniform and0.96069811 manifold. Initial single fresh query batches showed drift (some p50 ratios>1.1); those raw values were retained. A predeclared10-block alternating re-search of the exact retained native pairs on one CPU0 process used every block and gave all pooled per-index p50 ratios0.90697~1.02409. The main hnsw recall drops at ef10/80/200/1000 were at most0.002.

Separate unpooled upper-component profiling measured requested RMM peaks up to470264560 bytes, sampled device-used peaks1068302336 bytes, and sampled process RSS775839744 bytes. These component figures exclude the full parent native index; runtime/driver and allocator definitions remain distinct, and1ms sampled peaks can miss shorter transients. They are not a hard memory-cap guarantee.

Larger normalized IVF-PQ component cases retained FP16; smaller normalized NN cases retained the NN branch. Four full default-nsw native builds retained empty actual upper lists (the flat file header's maxlevel1 is a sentinel, not a real hierarchy). Full raw/native timing values and initial preparation failures remain in the external REPORT-r3 evidence. PG CREATE was not repeated as part of this SDK/native validation.

# HNSW ACE raw InnerProduct validation

Configure this directory against the cuVS package built from the canonical 0001+0002+0003 patchset, then build `cuvs_hnsw_ace_ip_factory_metric_probe`, `cuvs_hnsw_ace_subbuilder_budget_probe`, and `cuvs_hnsw_ace_ip_quality_pilot`.

`cuvs_hnsw_ace_ip_factory_metric_probe` invokes the SDK's real `cagra::index_params::from_hnsw_params` factory for `InnerProduct`, `CosineExpanded`, and `L2Expanded`. Running it with the prior memory-guard-v1 DSO must fail on the first `InnerProduct` top-level metric assertion; running it with the v2 DSO must pass all three assertions.

`cuvs_hnsw_ace_subbuilder_budget_probe legacy-admits` should complete against the prior v1 DSO. `cuvs_hnsw_ace_subbuilder_budget_probe guard-rejects` should fail the same real HNSW ACE build before its first subdataset allocation against v2, naming the source-derived NN-descent builder workspace. The budget case uses `N=8192`, `D=16`, `M=4`, 2 partitions, no disk build, a positive 0.007 GiB host budget, and a positive 1 GiB GPU budget. The old partition estimate is about 2 MiB at the actual partition size; the added NN-descent workspace and kNN graph exceed the host budget. The two modes allow Core to show that the prior SDK admitted the build and v2 rejects it at preflight.

Run the GPU pilot with one empty persistent directory argument. It builds and searches an 8192x16 unnormalized deterministic dataset through HNSW ACE with GPU hierarchy, 4 partitions, M=32, efConstruction=120, efSearch=300, and positive 1 GiB host and GPU budgets. It computes explicit CPU top-10 ground truth by descending raw dot product, requires recall@10 >= 0.80, and verifies that raw-IP and squared-L2 exact top-10 sets have at most 0.70 overlap. The dataset's deterministic norms span at least a 5x range, so the ranking check exercises raw magnitude semantics.

`check_metric_propagation_source.py SOURCE_ROOT` is a source-derived CPU regression for the production HNSW ACE factory/build/metadata path and the pre-allocation NN-descent guard. Core owns the SDK build, old-DSO RED run, new-DSO GREEN run, and GPU pilot.

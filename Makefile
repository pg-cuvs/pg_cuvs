# pg_cuvs Makefile
#
# Two layers:
#  1. PGXS targets (build/install) — run on the machine that has the
#     PostgreSQL + CUDA toolchain (typically the GCP GPU VM).
#  2. GCP remote targets (vm-start, sync, gpu-build, gpu-test, ...) —
#     run on the local laptop, SSH into the GPU VM, stream output back.
#
# The local laptop typically does not have a CUDA toolchain, so PGXS
# build commands only succeed when invoked on the VM.

# ---- PGXS configuration -------------------------------------------------
EXTENSION      = pg_cuvs
EXTVERSION     = 0.9.3
DATA           = sql/pg_cuvs--0.1.0.sql \
                 sql/pg_cuvs--0.1.0--0.2.0.sql \
                 sql/pg_cuvs--0.2.0.sql \
                 sql/pg_cuvs--0.2.0--0.3.0.sql \
                 sql/pg_cuvs--0.3.0.sql \
                 sql/pg_cuvs--0.3.0--0.4.0.sql \
                 sql/pg_cuvs--0.4.0.sql \
                 sql/pg_cuvs--0.4.0--0.5.0.sql \
                 sql/pg_cuvs--0.5.0.sql \
                 sql/pg_cuvs--0.5.0--0.6.0.sql \
                 sql/pg_cuvs--0.6.0.sql \
                 sql/pg_cuvs--0.6.0--0.7.0.sql \
                 sql/pg_cuvs--0.7.0.sql \
                 sql/pg_cuvs--0.7.0--0.8.0.sql \
                 sql/pg_cuvs--0.8.0.sql \
                 sql/pg_cuvs--0.8.0--0.8.1.sql \
                 sql/pg_cuvs--0.8.1.sql \
                 sql/pg_cuvs--0.8.1--0.8.2.sql \
                 sql/pg_cuvs--0.8.2.sql \
                 sql/pg_cuvs--0.8.2--0.9.0.sql \
                 sql/pg_cuvs--0.9.0.sql \
                 sql/pg_cuvs--0.9.0--0.9.1.sql \
                 sql/pg_cuvs--0.9.1.sql \
                 sql/pg_cuvs--0.9.1--0.9.2.sql \
                 sql/pg_cuvs--0.9.2.sql \
                 sql/pg_cuvs--0.9.2--0.9.3.sql \
                 sql/pg_cuvs--0.9.3.sql
MODULE_big     = pg_cuvs
REGRESS        = smoke upgrade_path cpu_fallback edge_cases cpu_hnsw_fallback build_hnsw build_hnsw_edge build_hnsw_halfvec build_hnsw_packing hnsw_insert_shift hnsw_layer_status hnsw_build_ef hnsw_source_conversion hnsw_source_maintenance hnsw_source_conversion_file pg_cuvs_hnsw metrics brute_force pg_cuvs_batch reloption_dir gc_orphans release_hardening pending_delta delta_recall build_params drop_subxact partition_prune filter_comparison prefilter_short_fill_guard ivfpq_smoke cagra_streaming cagra_dynbatch cagra_search_params auto_compact extend_vram_fallback stream_bf_recall fallback_stat vram_accounting build_lock flat_smoke transient_bf hw_profile routing_golden routing_golden_measured multi_vector daemon_uid_owner unordered_scan null_query_vector rmm_pool admission_reject
REGRESS_OPTS   = --inputdir=test --outputdir=test

# Tier-1 CI (CPU-reference shim, PGCUVS_CPU_SHIM=1) runs a SUBSET of REGRESS.
# Excluded = tests the shim cannot/should not reproduce on CPU:
#   build_hnsw/build_hnsw_edge/pg_cuvs_hnsw/build_hnsw_halfvec/
#     build_hnsw_packing/hnsw_insert_shift — CAGRA->pgvector HNSW *graph export* (real graph
#     structure; a Tier-2 / real-GPU concern).
REGRESS_TIER2_ONLY = build_hnsw build_hnsw_edge build_hnsw_halfvec build_hnsw_packing hnsw_insert_shift hnsw_build_ef hnsw_source_conversion_file pg_cuvs_hnsw routing_golden_measured
REGRESS_TIER1      = $(filter-out $(REGRESS_TIER2_ONLY),$(REGRESS))

# #124: build_oom, build_multi_oom, build_oom_evict_to_fit, and extend_cuda_oom
# call pg_cuvs_inject_{build,extend}_oom(), which reaches the daemon's
# CUVS_OP_INJECT_* opcodes. Those opcodes are compiled in ONLY under
# -DCUVS_TEST_HOOKS (pg_cuvs_server.c) — a production daemon now rejects them
# outright ("unknown op") rather than dispatching unconditionally, so these 4
# tests cannot run against the production daemon $(REGRESS)/$(REGRESS_TIER1)
# exercise. They run instead via `make installcheck-fault`, below, against a
# dedicated $(SERVER_TEST_BIN) (built with CUVS_TEST_HOOKS) on its own socket
# + index dir, so the production daemon and its REGRESS run are never touched.
#
# GPU-residency-dependent (#89 item 2): on the CPU shim (Tier-1 CI) the OOM
# injection is deterministic. On real GPU (esp. single-GPU dev VM) the
# evict-retry outcome depends on what is resident/evictable at run time, so a
# full-suite GPU run may flake on these — that is environment-dependent, not
# a code regression.
REGRESS_FAULT  = build_oom build_multi_oom build_oom_evict_to_fit extend_cuda_oom

# Isolation tests (pg_isolation_regress) for concurrent-session correctness that
# pg_regress cannot express: snapshot-aware tombstone filtering and write/query
# interleaving. Specs live in test/specs/*.spec, expected in test/expected/*.out.
# The daemon + GPU must be up, same as REGRESS.
ISOLATION      = delta_tombstone_snapshot delta_interleaving reindex_concurrent_delete flat_tombstone_snapshot flat_delta_interleaving flat_reindex_concurrent_delete
ISOLATION_OPTS = --inputdir=test --outputdir=test
# Specs that need a real GPU daemon (the CPU shim cannot build an HNSW graph from the heap), kept out of ISOLATION so that
# the tier1-shim CI job's installcheck-isolation does not run them; run them with installcheck-isolation-gpu.
ISOLATION_GPU  = pg_cuvs_hnsw_cic_writes

# C source files + the CUDA-compiled wrapper (built below by nvcc).
# PGXS only knows how to build .c → .o; the .cu → .o rule is custom,
# but the resulting object MUST be listed in OBJS to be linked into the .so.
OBJS           = src/pg_cuvs.o src/cuvs_ipc.o src/cuvs_util.o $(WRAPPER_OBJ) src/hnsw_export.o src/cuvs_hnsw_scan.o src/cuvs_hnsw_rls.o src/cuvs_hnsw_exact.o src/cuvs_build_corpus.o src/pg_cuvs_compaction.o src/cuvs_status_text.o src/cuvs_peak_meter.o src/cuvs_cagra_pages.o src/cuvs_cagra_pending_search.o src/cuvs_cagra_fold_worker.o

# nvcc settings (Phase 1: brute-force only; CAGRA added later)
NVCC          ?= nvcc
CUDA_ARCH     ?= sm_80
NVCC_FLAGS    ?= -O3 --compiler-options '-fPIC' -arch=$(CUDA_ARCH) -std=c++17 \
                 --extended-lambda \
                 -DRAFT_SYSTEM_LITTLE_ENDIAN=1 \
                 -DCUVS_BUILD_CAGRA_HNSWLIB

# cuVS / RAPIDS install root (conda env activates default)
CUVS_PREFIX   ?= $(CONDA_PREFIX)
CUVS_INCLUDE  ?= $(CUVS_PREFIX)/include
# cuVS 25.x+ ships bundled libcudacxx under include/rapids/
CUVS_RAPIDS_INCLUDE ?= $(CUVS_PREFIX)/include/rapids
CUVS_LIB      ?= $(CUVS_PREFIX)/lib
# Optional matching patched SDK cpp/src root. With it the ACE partition receipt
# records the effective count (computed twice, build refused if unstable); without
# it an ACE build hands the requested count to the SDK and the receipt records it
# as requested, effective count not guaranteed. The patched libcuvs.so is
# required either way.
CUVS_SDK_SOURCE_INCLUDE ?=
# Reset, and kept out of the environment (this Makefile has a bare `export` further down): a make
# started from a recipe would otherwise inherit the note and print it for a set variable, and nvcc
# would get it in its environment.
CUVS_SDK_PLAN_NOTE =
unexport CUVS_SDK_PLAN_NOTE
ifneq ($(strip $(CUVS_SDK_SOURCE_INCLUDE)),)
# The planner header pulls patched SDK sources (cagra_build.cuh, which patch 0006
# makes call raft::linalg::map_offset) into cuvs_wrapper.cu. Those RAFT kernels call
# constexpr host functions from device code, which nvcc rejects unless
# --expt-relaxed-constexpr is set, as the SDK's own build does.
CUVS_SDK_PLAN_FLAGS = -I$(CUVS_SDK_SOURCE_INCLUDE) -Xcompiler=-fopenmp --expt-relaxed-constexpr
CUVS_SDK_PLAN_LINK = -fopenmp
else
# One line when the GPU wrapper is compiled without the planner source. It is part of the
# src/cuvs_wrapper.o recipe, so CPU shim builds (which never build that object) and the
# CPU-only CI jobs print nothing.
CUVS_SDK_PLAN_NOTE = @echo "pg_cuvs: CUVS_SDK_SOURCE_INCLUDE is not set: the ACE effective partition count guarantee is OFF; the HNSW receipt will record the requested count (set CUVS_SDK_SOURCE_INCLUDE to the patched SDK cpp/src to turn it on)"
endif

# β3a GPU objects are opt-in. CPU and disabled native builds provide the
# same unavailable C entry point without including SDK source or CUDA.
PGCUVS_BETA3A_GPU_FOLD ?= 0
CUVS_BETA3A_SDK_SOURCE ?= $(CUVS_SDK_SOURCE_INCLUDE)
CUVS_FOLD_NATIVE = 0
CUVS_FOLD_OBJ = src/cuvs_cagra_fold_unavailable.o

# ---- CI Tier 1: CPU-reference shim (no CUDA/cuVS) -------------------------
# `make PGCUVS_CPU_SHIM=1` swaps the nvcc-built cuVS wrapper for a pure-C CPU
# shim (src/cuvs_wrapper_shim_cpu.c) so the extension + daemon build and run on
# a GPU-less hosted CI runner — zero CUDA toolkit. See design/ci-strategy.md
# (ADR-067). cuvs_wrapper.h is the single GPU boundary, so nothing else changes.
ifdef PGCUVS_CPU_SHIM
WRAPPER_OBJ      = src/cuvs_wrapper_shim_cpu.o $(CUVS_FOLD_OBJ)
CUVS_SO_LINK     = -lm
CUVS_SERVER_LINK = -lm
CUVS_LOGGER_LINK =
else
ifeq ($(CUVS_PAGE_FORMAT)$(PGCUVS_BETA3A_GPU_FOLD),11)
CUVS_FOLD_NATIVE = 1
CUVS_FOLD_OBJ = src/cuvs_cagra_fold_sdk.o
CUVS_FOLD_CPPFLAGS = -DPGCUVS_BETA3A_GPU_FOLD
CUVS_FOLD_PRODUCT_CPPFLAGS = -DCUVS_CAGRA_FOLD_ENABLED
CUVS_FOLD_LINK = -fopenmp
CUVS_FOLD_SO_LINK = -lrmm
endif
WRAPPER_OBJ      = src/cuvs_wrapper.o $(CUVS_FOLD_OBJ)
CUVS_SO_LINK     = -L$(CUVS_LIB) -lcuvs $(CUVS_FOLD_SO_LINK) -lcudart -Wl,-rpath,$(CUVS_LIB)
CUVS_SERVER_LINK = -L$(CUVS_LIB) -lcuvs -lrmm -lcudart -Wl,-rpath,$(CUVS_LIB)
# The wrapper expands RMM_LOG_* (rmm/mr/pool_memory_resource.hpp), which call
# rapids_logger::logger::log, defined in librapids_logger.so. libcuvs.so and
# librmm.so need it only indirectly, and `ld` does not take symbols from a
# library's own dependencies, so the link fails with "DSO missing from command
# line" unless it is named here, with or without CUVS_SDK_SOURCE_INCLUDE. A
# separate variable, so a hand-written CUVS_SO_LINK / CUVS_SERVER_LINK keeps it.
CUVS_LOGGER_LINK = -lrapids_logger
endif

PG_CPPFLAGS    = -I$(CUVS_INCLUDE) -I$(CUVS_RAPIDS_INCLUDE) -I./src
ifeq ($(CUVS_PAGE_FORMAT),1)
PG_CPPFLAGS += -DCUVS_PAGE_FORMAT
endif
PG_CPPFLAGS += $(CUVS_FOLD_PRODUCT_CPPFLAGS)
# -Wl,-rpath embeds the cuVS lib path so postmaster finds libcuvs.so
# without LD_LIBRARY_PATH being set (ADR-007).
SHLIB_LINK     = $(CUVS_SO_LINK) $(CUVS_LOGGER_LINK) $(CUVS_SDK_PLAN_LINK) $(CUVS_FOLD_LINK) \
                 -Wl,-Bstatic -lstdc++ -Wl,-Bdynamic -lrt

EXTRA_CLEAN += src/.cuvs-fold-mode src/cuvs_cagra_fold_sdk.o src/cuvs_cagra_fold_sdk.bc \
               src/cuvs_cagra_fold_unavailable.o src/cuvs_cagra_fold_unavailable.bc

PG_CONFIG     ?= pg_config
PGXS         := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# PGXS's implicit .c->.o rule does NOT track header dependencies, so a change to
# a shared header (e.g. a wire struct in cuvs_ipc.h) would otherwise leave stale
# extension objects linked against the old layout — an ABI mismatch vs the
# daemon. Force every extension object to rebuild when ANY project header
# changes. (The daemon objects get the same blanket rule via SERVER_ALL_OBJS.)
$(OBJS): $(wildcard src/*.h)
$(OBJS) $(OBJS:.o=.bc): src/.cuvs-fold-mode

# Build CUDA object before linking the .so. Pattern rule overrides PGXS
# default for this specific file since .cu needs nvcc, not gcc.
ifneq ($(strip $(CUVS_SDK_SOURCE_INCLUDE)),)
.PHONY: verify-hnsw-sdk-plan
verify-hnsw-sdk-plan:
	python3 scripts/verify-hnsw-sdk-plan.py --source '$(CUVS_SDK_SOURCE_INCLUDE)'
src/cuvs_wrapper.o: | verify-hnsw-sdk-plan
endif

src/cuvs_wrapper.o: src/cuvs_wrapper.cu src/cuvs_wrapper.h src/cuvs_hnsw_sdk_plan.cuh \
                    src/cuvs_hnsw_nn_sdk_params.cuh src/cuvs_hnsw_nn_precision.h src/cuvs_hnsw_nn_capacity.h src/cuvs_hnsw_nn_upper_contract.h src/cuvs_hnsw_nn_center.h \
                    src/cuvs_failed_allocation.hpp src/cuvs_rmm_failure_resource.hpp \
                    src/cuvs_cagra_fold.h src/cuvs_cagra_fold_capacity.h src/cuvs_cagra_fold_sdk.hpp src/.cuvs-fold-mode
	$(CUVS_SDK_PLAN_NOTE)
	$(NVCC) $(NVCC_FLAGS) $(CUVS_SDK_PLAN_FLAGS) $(CUVS_FOLD_CPPFLAGS) -I$(CUVS_INCLUDE) -I$(CUVS_RAPIDS_INCLUDE) -I./src -c $< -o $@

# A mode change must relink the products even when switching back to older
# OFF prerequisites in the same build directory.
.PHONY: cuvs-force-fold-mode
src/.cuvs-fold-mode: cuvs-force-fold-mode
	@tmp=$$(mktemp "$@.XXXXXX") && \
	  printf '%s\n' '$(CUVS_FOLD_NATIVE) $(if $(filter 1,$(CUVS_PAGE_FORMAT)),1,0)' > "$$tmp" && \
	  if cmp -s "$$tmp" "$@"; then rm "$$tmp"; else mv "$$tmp" "$@"; fi

# Match the existing wrapper objects' C linkage on disabled builds.
src/cuvs_cagra_fold_unavailable.o: src/cuvs_cagra_fold_unavailable.c src/cuvs_cagra_fold.h src/cuvs_wrapper.h
	$(CC) -O2 -g -fPIC -I./src -std=gnu11 -c $< -o $@

ifeq ($(CUVS_FOLD_NATIVE),1)
.PHONY: verify-cagra-fold-sdk
verify-cagra-fold-sdk:
	python3 scripts/verify-cagra-fold-sdk.py --source '$(CUVS_BETA3A_SDK_SOURCE)'

src/cuvs_cagra_fold_sdk.o: src/cuvs_cagra_fold_sdk.cu src/cuvs_cagra_fold_sdk.hpp src/cuvs_cagra_fold_capacity.h | verify-cagra-fold-sdk
	$(NVCC) $(filter-out -std=%,$(NVCC_FLAGS)) -std=c++20 --expt-relaxed-constexpr \
	    -DCUDA_API_PER_THREAD_DEFAULT_STREAM -Xcompiler=-fopenmp \
	    -I$(CUVS_INCLUDE) -I$(CUVS_RAPIDS_INCLUDE) -I./src \
	    -I'$(CUVS_BETA3A_SDK_SOURCE)/neighbors/detail/cagra' -c $< -o $@

# PGXS links one bitcode file per object. CUDA entry points live in the ELF.
src/cuvs_cagra_fold_sdk.bc: src/cuvs_cagra_fold_sdk.cu
	printf 'void cuvs_cagra_fold_sdk_jit_stub(void){}\n' | $(CLANG) -x c -emit-llvm -c - -o $@
endif

# PGXS generates LLVM bitcode (.bc) from .c sources for JIT. The .cu file
# has no PG-callable functions, so emit a stub bitcode so PGXS doesn't fail.
src/cuvs_wrapper.bc: src/cuvs_wrapper.cu src/cuvs_hnsw_nn_sdk_params.cuh src/cuvs_hnsw_nn_precision.h src/cuvs_hnsw_nn_capacity.h src/cuvs_hnsw_nn_upper_contract.h src/cuvs_hnsw_nn_center.h src/cuvs_failed_allocation.hpp src/cuvs_rmm_failure_resource.hpp
	@echo 'void cuvs_wrapper_jit_stub(void){}' > /tmp/_cuvs_stub.c
	@clang-15 -emit-llvm -c /tmp/_cuvs_stub.c -o $@ 2>/dev/null \
		|| clang -emit-llvm -c /tmp/_cuvs_stub.c -o $@
	@rm /tmp/_cuvs_stub.c

# ---- pg_cuvs_server binary -----------------------------------------------
# Standalone GPU daemon — NOT a PostgreSQL extension, no PGXS involvement.
# Links against libcuvs + libcudart + libpthread + librt.
SERVER_SRCS    = src/pg_cuvs_server.c src/cuvs_ipc.c src/cuvs_util.c
SERVER_OBJS    = src/pg_cuvs_server.o src/cuvs_ipc.o src/cuvs_util.o
SERVER_BIN     = pg_cuvs_server

CC             ?= gcc
SERVER_CFLAGS  = -O2 -g -Wall -Wextra -I./src \
                 -I$(CUVS_INCLUDE) -I$(CUVS_RAPIDS_INCLUDE) -std=gnu11 \
                 -D_POSIX_C_SOURCE=200809L
# Optional sanitizer/debug hooks for the daemon (e.g. CI ASAN job passes
# EXTRA_SERVER_CFLAGS="-fsanitize=address -fno-omit-frame-pointer"). Empty by default.
SERVER_CFLAGS += $(EXTRA_SERVER_CFLAGS) $(CUVS_FOLD_PRODUCT_CPPFLAGS)
ifdef PGCUVS_CPU_SHIM
SERVER_CFLAGS += -DPGCUVS_CPU_SHIM
endif
SERVER_LDFLAGS = $(CUVS_SERVER_LINK) $(CUVS_LOGGER_LINK) $(CUVS_SDK_PLAN_LINK) $(CUVS_FOLD_LINK) -lstdc++ \
                 -lpthread -lrt \
                 -lcurl -lssl -lcrypto
SERVER_LDFLAGS += $(EXTRA_SERVER_LDFLAGS)

# Every daemon object, including the *_test variants built with the CPU shim.
# The per-object rules below list header prereqs by hand and have drifted from
# the actual #include set (cuvs_ipc.c and cuvs_objstore.c both pull in
# cuvs_util.h, which carries the on-disk/wire structs CuvsDeltaHeader,
# CuvsTombstoneHeader, CuvsShardRecord, …). A missed prereq links a stale
# object against an old struct layout — the same ABI drift the extension-side
# rule below guards against, but inside the daemon binary. Rebuild them all
# when ANY project header changes.
SERVER_ALL_OBJS = src/pg_cuvs_server.o src/cuvs_objstore_server.o \
                  src/cuvs_ipc_server.o src/cuvs_util_server.o \
                  src/cuvs_build_corpus_server.o \
                  src/pg_cuvs_server_test.o src/cuvs_objstore_server_test.o \
                  src/cuvs_ipc_server_test.o src/cuvs_util_server_test.o \
                  src/cuvs_build_corpus_server_test.o \
                  src/cuvs_page_overlay_server.o src/cuvs_page_overlay_server_test.o
$(SERVER_ALL_OBJS): $(wildcard src/*.h)

# server .c → .o (not via PGXS — separate rule with no PG headers)
src/pg_cuvs_server.o: src/pg_cuvs_server.c src/cuvs_ipc.h src/cuvs_page_overlay.h src/cuvs_util.h src/cuvs_wrapper.h src/cuvs_objstore.h src/cuvs_build_corpus.h src/cuvs_bf_verdict.h src/.cuvs-fold-mode
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

# cuvs_objstore.o for server (GCS client — not linked into the PG extension .so)
src/cuvs_objstore_server.o: src/cuvs_objstore.c src/cuvs_objstore.h src/cuvs_ipc.h src/cuvs_version.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

# cuvs_ipc.o for server (same source, no PG headers needed)
# Note: PGXS also builds cuvs_ipc.o for the .so; use a separate target.
src/cuvs_ipc_server.o: src/cuvs_ipc.c src/cuvs_ipc.h src/cuvs_build_corpus.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

# cuvs_util.o for server (same source, no PG headers needed)
# Note: PGXS also builds cuvs_util.o for the .so; use a separate target.
src/cuvs_util_server.o: src/cuvs_util.c src/cuvs_util.h src/cuvs_ipc.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

# I3-B lock-free pool peak meter (C11 atomics), called by the RMM adaptor in
# the wrapper; linked wherever the wrapper is (daemon and extension).
src/cuvs_peak_meter_server.o: src/cuvs_peak_meter.c src/cuvs_peak_meter.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

# I3-B cache policy (pure; daemon only)
src/cuvs_cache_policy_server.o: src/cuvs_cache_policy.c src/cuvs_cache_policy.h src/cuvs_admission_contract.h src/cuvs_util.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

# cuvs_build_corpus.o for server (ADR-048: cuvs_fd_recv; same source, no PG headers)
src/cuvs_build_corpus_server.o: src/cuvs_build_corpus.c src/cuvs_build_corpus.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

# host-built HNSW upper layers for CUVS_OP_EXPORT_HNSW_UPPER (plain C, both daemon builds)
src/cuvs_hnsw_upper_server.o: src/cuvs_hnsw_upper.c src/cuvs_hnsw_upper.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

# CI Tier 1: pure-C CPU shim object (no CUDA). Shared by the .so and the daemon
# (same object both link cuvs_wrapper.o today). Built with gcc, -fPIC for the .so.
src/cuvs_wrapper_shim_cpu.o: src/cuvs_wrapper_shim_cpu.c src/cuvs_wrapper.h src/cuvs_ipc.h
	$(CC) -O2 -g -fPIC -I./src -std=gnu11 -c $< -o $@

src/cuvs_page_overlay_server.o: src/cuvs_page_overlay.c src/cuvs_page_overlay.h src/cuvs_ipc.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

$(SERVER_BIN): src/pg_cuvs_server.o src/cuvs_ipc_server.o src/cuvs_util_server.o src/cuvs_objstore_server.o src/cuvs_build_corpus_server.o src/cuvs_cache_policy_server.o src/cuvs_peak_meter_server.o src/cuvs_hnsw_upper_server.o src/cuvs_page_overlay_server.o $(WRAPPER_OBJ)
	$(CXX) -o $@ $^ $(SERVER_LDFLAGS)

server: $(SERVER_BIN)

install-server: server
	install -m 755 $(SERVER_BIN) $(shell $(PG_CONFIG) --bindir)/

.PHONY: server install-server

# ---- pg_cuvs_server_test binary (fault injection) ------------------------
# Same sources as the production daemon, but compiled with -DCUVS_TEST_HOOKS
# so the cuvs_fault() env-var hooks in handle_build are live. NEVER install
# this over the production binary. Object files use a _test suffix so they do
# not clobber the production .o files.
SERVER_TEST_BIN     = pg_cuvs_server_test
SERVER_TEST_CFLAGS  = $(SERVER_CFLAGS) -DCUVS_TEST_HOOKS

src/pg_cuvs_server_test.o: src/pg_cuvs_server.c src/cuvs_ipc.h src/cuvs_page_overlay.h src/cuvs_util.h src/cuvs_wrapper.h src/cuvs_objstore.h src/cuvs_bf_verdict.h src/.cuvs-fold-mode
	$(CC) $(SERVER_TEST_CFLAGS) -c $< -o $@

src/cuvs_ipc_server_test.o: src/cuvs_ipc.c src/cuvs_ipc.h src/cuvs_build_corpus.h
	$(CC) $(SERVER_TEST_CFLAGS) -c $< -o $@

src/cuvs_util_server_test.o: src/cuvs_util.c src/cuvs_util.h src/cuvs_ipc.h
	$(CC) $(SERVER_TEST_CFLAGS) -c $< -o $@

src/cuvs_objstore_server_test.o: src/cuvs_objstore.c src/cuvs_objstore.h src/cuvs_ipc.h src/cuvs_version.h
	$(CC) $(SERVER_TEST_CFLAGS) -c $< -o $@

src/cuvs_build_corpus_server_test.o: src/cuvs_build_corpus.c src/cuvs_build_corpus.h
	$(CC) $(SERVER_TEST_CFLAGS) -c $< -o $@

src/cuvs_cache_policy_server_test.o: src/cuvs_cache_policy.c src/cuvs_cache_policy.h src/cuvs_admission_contract.h src/cuvs_util.h
	$(CC) $(SERVER_TEST_CFLAGS) -c $< -o $@

src/cuvs_peak_meter_server_test.o: src/cuvs_peak_meter.c src/cuvs_peak_meter.h
	$(CC) $(SERVER_TEST_CFLAGS) -c $< -o $@

src/cuvs_hnsw_upper_server_test.o: src/cuvs_hnsw_upper.c src/cuvs_hnsw_upper.h
	$(CC) $(SERVER_TEST_CFLAGS) -c $< -o $@

src/cuvs_page_overlay_server_test.o: src/cuvs_page_overlay.c src/cuvs_page_overlay.h src/cuvs_ipc.h
	$(CC) $(SERVER_TEST_CFLAGS) -c $< -o $@

$(SERVER_TEST_BIN): src/pg_cuvs_server_test.o src/cuvs_ipc_server_test.o src/cuvs_util_server_test.o src/cuvs_objstore_server_test.o src/cuvs_build_corpus_server_test.o src/cuvs_cache_policy_server_test.o src/cuvs_peak_meter_server_test.o src/cuvs_hnsw_upper_server_test.o src/cuvs_page_overlay_server_test.o $(WRAPPER_OBJ)
	$(CXX) -o $@ $^ $(SERVER_LDFLAGS)

server-test: $(SERVER_TEST_BIN)

install-server-test: server-test
	install -m 755 $(SERVER_TEST_BIN) $(shell $(PG_CONFIG) --bindir)/

.PHONY: server-test install-server-test

# ---- Local unit tests ----------------------------------------------------
# No-framework unit tests for the dependency-free helpers in cuvs_util.c.
# Deliberately independent of PGXS/pg_config/CUDA so it runs on a laptop.
# librt exists on Linux (CI/VM) but not macOS — link it only where present.
TEST_RT_LIB := $(shell [ "$$(uname -s)" = "Linux" ] && echo -lrt)
test-unit: test-compact-retry test-device-norm test-failed-allocation test-build-oom-log test-peak-log-switch test/unit/test_cuvs_util.c src/cuvs_util.c src/cuvs_util.h src/cuvs_ipc.h \
           test/unit/test_build_corpus.c src/cuvs_build_corpus.c src/cuvs_build_corpus.h \
           test-hnsw-build test-hnsw-build-ipc test-hnsw-owner-lease test-hnsw-ace-quality test-hnsw-admission test-admission-contract \
           test-hnsw-nn-policy test-hnsw-nn-route test-hnsw-nn-precision test-hnsw-nn-capacity test-hnsw-nn-sdk-params test-hnsw-nn-consumer test-hnsw-nn-upper-receipt test-hnsw-nn-center test-hnsw-nn-mutations test-hnsw-ace-origin-json test-hnsw-ace-planner-branch test-hnsw-sdk-plan-gate test-hnsw-gpu-link-lines \
           test-i3-logs test-notice-wire test-admission-wire test-admission-message test-admission-ipc test-build-oom-thread test-cache-policy test-circuit-source test-status-text test-i3b-source test-peak-meter test-lw-policy test-lw-guard test-i3e-sql test-self-replace-notice test-host-free-fence test-vacuum-policy test-bf-verdict test-hnsw-digest
	$(CC) -I src -DCUVS_TEST_HOOKS -o test-unit test/unit/test_cuvs_util.c src/cuvs_util.c $(TEST_RT_LIB)
	./test-unit
	$(CC) -I src -o test-build-corpus test/unit/test_build_corpus.c src/cuvs_build_corpus.c $(TEST_RT_LIB)
	./test-build-corpus

# UC-2: no GPU scan path may end with 0 rows on an open circuit breaker
# (source check; the AM callbacks need a live backend to execute).
test-circuit-source: test/unit/check_circuit_source.py src/pg_cuvs.c
	python3 -B test/unit/check_circuit_source.py

.PHONY: test-unit test-circuit-source

# Source structure checks of the HNSW build routes, run by the CPU job of
# .github/workflows/ci.yml. The three check_* scripts read src/ as text, enforce
# the invariants the approved route design relies on, and replay planted
# defects against their own rules. The two test_* scripts compile copies of the
# real headers with one planted defect each and need the unit test to fail.
# The two unit tests the mutation scripts mutate (test-hnsw-auto-overflow,
# test-hnsw-explicit-route) are prerequisites: no other CI step runs them. The
# mutation scripts also run the unmutated build themselves before the first
# mutant and fail with BASELINE_NOT_GREEN if it does not pass. Each script
# writes a report into a directory that must not exist yet, so they get a fresh
# temporary one. All five run even if one fails. Python 3 stdlib and cc only:
# no git history, PostgreSQL, CUDA or daemon.
.PHONY: test-source-checks
test-source-checks: test-hnsw-auto-overflow test-hnsw-explicit-route
	set -e; out=$$(mktemp -d "$${TMPDIR:-/tmp}/pgcuvs-source-checks.XXXXXX"); \
	trap 'rm -rf "$$out"' EXIT; rc=0; \
	for c in check_hnsw_auto_overflow_source check_hnsw_explicit_source \
	         check_hnsw_ace_receipt_source \
	         test_hnsw_auto_overflow_mutations test_hnsw_explicit_mutations; do \
	    echo "== $$c"; python3 -I test/unit/$$c.py "$$out/$$c" || rc=1; \
	done; \
	exit $$rc
	python3 -I -B test/unit/check_cagra_relcache_source.py

# PostgreSQL 16, pgvector and the CPU shim. Uses private builds and clusters.
.PHONY: test-page-format
test-page-format:
	python3 -B test/page_format/run.py "$(PG_CONFIG)"

# K3: chunked spool digest used by the HNSW build during the daemon RPC.
test-hnsw-digest: test/unit/test_hnsw_digest.c src/cuvs_hnsw_digest.h
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I src -o $@ $< -lcrypto
	./$@

.PHONY: test-hnsw-digest
EXTRA_CLEAN += test-hnsw-digest

test-self-replace-notice:
	python3 -I -B test/unit/test_self_replace_notice.py

.PHONY: test-self-replace-notice

test-peak-log-switch:
	python3 -I -B test/unit/test_peak_log_switch.py

.PHONY: test-peak-log-switch

test-failed-allocation: test/unit/test_failed_allocation.cpp src/cuvs_failed_allocation.hpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror -pthread -I src $< -o $@
	./$@

test-build-oom-log:
	python3 -I -B test/unit/test_build_oom_log.py

.PHONY: test-failed-allocation test-build-oom-log

# HNSW admission and op-specific FD/owner lifecycle fixtures. No daemon, PG
# backend or CUDA is needed. Keep separate targets for bounded worker checks.
test-hnsw-build: test/unit/test_hnsw_build.c src/cuvs_hnsw_build_common.h
	$(CC) -std=gnu11 -Wall -Wextra -I src -o $@ test/unit/test_hnsw_build.c
	./$@

test-hnsw-owner-lease: test/unit/test_hnsw_owner_lease.c src/cuvs_ipc.c \
                       src/cuvs_util.c src/cuvs_build_corpus.c $(wildcard src/*.h)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_hnsw_owner_lease.c \
		src/cuvs_ipc.c src/cuvs_util.c src/cuvs_build_corpus.c -lpthread -lm $(TEST_RT_LIB)
	./$@

test-hnsw-build-ipc: test/unit/test_hnsw_build_ipc.c src/cuvs_ipc.c \
                    src/cuvs_util.c src/cuvs_build_corpus.c $(wildcard src/*.h)
	$(CC) -std=gnu11 -Wall -Wextra -I src -o $@ test/unit/test_hnsw_build_ipc.c \
		src/cuvs_ipc.c src/cuvs_util.c src/cuvs_build_corpus.c -lpthread -lm $(TEST_RT_LIB)
	./$@

test-hnsw-ace-quality: test/unit/test_hnsw_ace_quality.c src/cuvs_hnsw_policy.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_hnsw_ace_quality.c
	./$@

# UC-5 ACE quality rule / readback mutations: each must turn its test RED.
test-hnsw-ace-quality-mutations: test/unit/test_hnsw_ace_quality_mutations.py test-hnsw-ace-quality test-hnsw-build-ipc
	CC=$(CC) TEST_RT_LIB="$(TEST_RT_LIB)" python3 -I test/unit/test_hnsw_ace_quality_mutations.py

test-hnsw-policy: test/unit/test_hnsw_policy.c $(wildcard src/cuvs_hnsw*.h)
	$(CC) -std=gnu11 -Wall -Wextra -I src -o $@ test/unit/test_hnsw_policy.c -lpthread
	./$@

test-hnsw-explicit-route: test/unit/test_hnsw_explicit_route.c src/cuvs_hnsw_build_common.h src/cuvs_hnsw_policy.h src/cuvs_ipc.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_hnsw_explicit_route.c -lpthread
	./$@

# SDK side of build_algo=nn_descent (infra/cuvs-patches 0005). Needs a cuVS clone
# containing 60b19a0f; the patched tree is rebuilt from tracked patches each run.
#   make test-hnsw-nn-sdk CUVS_UPSTREAM_GIT=/path/to/cuvs
# Graph-consumer row-sort (patch 0006) planner checks on the same rebuilt tree;
# CUVS_ROW_SORT_SDK_SRC=<cpp/src of another 0006 build> optionally byte-compares it.
#   make test-hnsw-row-sort-sdk CUVS_UPSTREAM_GIT=...
.PHONY: test-hnsw-nn-sdk test-hnsw-row-sort-sdk
test-hnsw-nn-sdk:
	@test -n "$(CUVS_UPSTREAM_GIT)" || { echo "set CUVS_UPSTREAM_GIT to a cuVS clone containing 60b19a0f" >&2; exit 2; }
	set -e; t=$$(mktemp -d); trap 'rm -rf "$$t"' EXIT; \
	scripts/prepare-patched-cuvs-source.sh '$(CUVS_UPSTREAM_GIT)' "$$t/cuvs"; \
	n="$$t/cuvs/cpp/src/neighbors"; \
	python3 -I test/unit/check_hnsw_nn_sdk_source.py --sdk-src "$$n"; \
	python3 -I test/unit/test_hnsw_nn_upper_consumer.py --sdk-src "$$n"; \
	$(CXX) -std=c++17 -Wall -Wextra -Werror -I "$$n/detail" test/unit/test_hnsw_nn_upper_precision.cpp -o "$$t/upper-precision"; \
	"$$t/upper-precision"; \
	python3 -I test/unit/test_hnsw_nn_upper_precision_mutations.py --sdk-src "$$n"; \
	python3 -I test/native/hnsw-sdk/test_nn_upper_legacy_forwarder.py --source "$$t/cuvs/cpp/src"; \
	python3 -I test/native/hnsw-sdk/test_nn_upper_legacy_forwarder.py --source "$$t/cuvs/cpp/src" --mutant

test-hnsw-row-sort-sdk:
	@test -n "$(CUVS_UPSTREAM_GIT)" || { echo "set CUVS_UPSTREAM_GIT to a cuVS clone containing 60b19a0f" >&2; exit 2; }
	set -e; t=$$(mktemp -d); trap 'rm -rf "$$t"' EXIT; \
	scripts/prepare-patched-cuvs-source.sh '$(CUVS_UPSTREAM_GIT)' "$$t/cuvs"; \
	git -C "$$t/cuvs" show level-0005:cpp/src/neighbors/detail/cagra/cagra_build.cuh > "$$t/canonical-cagra_build.cuh"; \
	c="$$t/canonical-cagra_build.cuh"; b="$$t/cuvs/cpp/src"; \
	python3 -I scripts/verify-hnsw-sdk-plan.py --source "$$b"; \
	python3 -I -B test/unit/test_hnsw_sdk_plan_verifier.py --canonical-header "$$c" --row-sort-src "$$b"; \
	python3 -I -B test/unit/test_hnsw_sdk_plan_mutations.py --canonical-header "$$c" --row-sort-src "$$b"; \
	python3 -I test/unit/test_hnsw_row_sort_planner_equivalence.py --canonical-header "$$c" --row-sort-src "$$b"; \
	if [ -n '$(CUVS_ROW_SORT_SDK_SRC)' ]; then \
	  for f in neighbors/detail/cagra/cagra_build.cuh neighbors/detail/cagra/consumer_row_sort_plan.hpp; do \
	    cmp "$$b/$$f" '$(CUVS_ROW_SORT_SDK_SRC)'/"$$f"; done; \
	  echo "external source matches 0006 for the planner/consumer files"; \
	fi

.PHONY: test-hnsw-nn-precision test-hnsw-nn-policy test-hnsw-nn-capacity test-hnsw-nn-route test-hnsw-nn-sdk-params test-hnsw-nn-consumer test-hnsw-nn-upper-receipt
test-hnsw-nn-precision: test/unit/test_hnsw_nn_precision.c src/cuvs_hnsw_nn_precision.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_hnsw_nn_precision.c -lm
	./$@

test-hnsw-nn-policy: test/unit/test_hnsw_nn_policy.c src/cuvs_hnsw_policy.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_hnsw_nn_policy.c
	./$@

test-hnsw-nn-capacity: test/unit/test_hnsw_nn_capacity.c src/cuvs_hnsw_nn_capacity.h src/cuvs_hnsw_build_common.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_hnsw_nn_capacity.c -lm
	./$@

test-hnsw-nn-route: test/unit/test_hnsw_nn_route.c src/cuvs_hnsw_build_common.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_hnsw_nn_route.c -lpthread -lm
	./$@

test-hnsw-nn-sdk-params: test/unit/test_hnsw_nn_sdk_params.cpp src/cuvs_hnsw_nn_sdk_params.cuh
	$(CXX) -std=c++17 -Wall -Wextra -Werror -I src -o $@ test/unit/test_hnsw_nn_sdk_params.cpp
	./$@

test-hnsw-nn-consumer: test/unit/test_hnsw_nn_consumer.py src/cuvs_wrapper.cu src/cuvs_hnsw_nn_sdk_params.cuh
	ulimit -c 0; python3 test/unit/test_hnsw_nn_consumer.py

test-hnsw-nn-upper-receipt: test/unit/test_hnsw_nn_upper_receipt.py test/unit/test_hnsw_nn_upper_receipt_mutation.py src/hnsw_export.c src/cuvs_hnsw_policy.h
	python3 -I -B test/unit/test_hnsw_nn_upper_receipt.py
	python3 -I -B test/unit/test_hnsw_nn_upper_receipt_mutation.py

# ACE build_info JSON (origin of the partition count, exactness of the readback) rendered from
# the production hnsw_export.c on stored receipts, with and without the SDK source planner.
.PHONY: test-hnsw-ace-origin-json
test-hnsw-ace-origin-json: test/unit/test_hnsw_ace_origin_json.py src/hnsw_export.c src/cuvs_hnsw_policy.h
	python3 -I -B test/unit/test_hnsw_ace_origin_json.py

# The ACE branch of cuvs_wrapper.cu compiled with and without the SDK source planner, against
# stubs of the cuVS types it touches (no CUDA, no GPU).
.PHONY: test-hnsw-ace-planner-branch
test-hnsw-ace-planner-branch: test/unit/test_hnsw_ace_planner_branch.py src/cuvs_wrapper.cu src/cuvs_hnsw_policy.h
	CXX="$(CXX)" python3 -I -B test/unit/test_hnsw_ace_planner_branch.py

# The switch that turns the SDK source planner on (header include, OpenMP + source path gate, nvcc
# flags) and the one-line note of a GPU build without it; ACE no longer fails without the planner.
.PHONY: test-hnsw-sdk-plan-gate
test-hnsw-sdk-plan-gate: test/unit/test_hnsw_sdk_plan_gate.py src/cuvs_hnsw_sdk_plan.cuh src/cuvs_wrapper.cu Makefile
	CXX="$(CXX)" python3 -I -B test/unit/test_hnsw_sdk_plan_gate.py

# librapids_logger is named on the extension and daemon link lines of every GPU build.
.PHONY: test-hnsw-gpu-link-lines
test-hnsw-gpu-link-lines: test/unit/test_hnsw_gpu_link_lines.py Makefile
	python3 -I -B test/unit/test_hnsw_gpu_link_lines.py

# NN-descent L2 graph-input centering (docs/hnsw-build-options.md, nn_descent).
.PHONY: test-hnsw-nn-center
test-hnsw-nn-center: test/unit/test_hnsw_nn_center.c test/unit/test_hnsw_nn_center_wrapper.py src/cuvs_hnsw_nn_center.h src/cuvs_hnsw_nn_precision.h src/cuvs_wrapper.cu
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I src -o $@ test/unit/test_hnsw_nn_center.c -lm
	./$@
	ulimit -c 0; python3 -I -B test/unit/test_hnsw_nn_center_wrapper.py

EXTRA_CLEAN += test-hnsw-nn-policy test-hnsw-nn-precision test-hnsw-nn-capacity test-hnsw-nn-route test-hnsw-nn-sdk-params test-hnsw-nn-center

# NN guard mutants (each script compiles mutated copies in a temp dir and
# requires the matching unit test to fail; part of test-unit).
.PHONY: test-hnsw-nn-mutations
test-hnsw-nn-mutations:
	ulimit -c 0; python3 -I -B test/unit/test_hnsw_nn_policy_mutations.py
	ulimit -c 0; python3 -I -B test/unit/test_hnsw_nn_precision_mutations.py
	ulimit -c 0; python3 -I -B test/unit/test_hnsw_nn_capacity_mutations.py
	ulimit -c 0; python3 -I -B test/unit/test_hnsw_nn_sdk_params_mutations.py
	ulimit -c 0; python3 -I -B test/unit/test_hnsw_nn_center_mutations.py

# I3-C: CK-17 (12)-(14) auto overflow, pre-spool decision, CAGRA reltuples AR0.
test-hnsw-auto-overflow: test/unit/test_hnsw_auto_overflow.c src/cuvs_route_preflight.h \
                         src/cuvs_hnsw_build_common.h src/cuvs_hnsw_policy.h src/cuvs_admission_contract.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_hnsw_auto_overflow.c -lpthread -lm
	./$@

# I3-D: CK-33 resource pool wiring (dedicated RMM stream) with fake CUDA types.
test-res-pool: test/unit/test_res_pool.cpp src/cuvs_res_pool.hpp src/cuvs_wrapper.h
	$(CXX) -std=c++17 -Wall -Wextra -Werror -I src -o $@ test/unit/test_res_pool.cpp -lpthread
	./$@

test-hnsw-admission: test/unit/test_hnsw_admission.c src/cuvs_hnsw_build_common.h
	$(CC) -std=gnu11 -Wall -Wextra -I src -o $@ test/unit/test_hnsw_admission.c -lpthread
	./$@

# I3 shared admission contract: the estimate and the I4 hook, built as C11 and
# as C++17 (the header is also included by the C++/CUDA wrapper).
test-admission-contract: test/unit/test_admission_contract.c src/cuvs_admission_contract.h \
                         src/cuvs_util.c src/cuvs_util.h src/cuvs_ipc.h
	$(CC) -std=c11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_admission_contract.c \
		src/cuvs_util.c $(TEST_RT_LIB)
	./$@
	$(CC) -I src -c src/cuvs_util.c -o $@-util.o
	$(CXX) -std=c++17 -Wall -Wextra -Werror -I src -o $@-cxx -x c++ \
		test/unit/test_admission_contract.c -x none $@-util.o $(TEST_RT_LIB)
	./$@-cxx

# Sanitizers for the admission codec/message tests (CK-29 boundary). Set
# ADMISSION_TEST_SAN= on toolchains without libasan (e.g. musl).
ADMISSION_TEST_SAN ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined

# Integrated NOTICE channel boundary and message/DETAIL transport (CK-29).
test-i3-logs: test/unit/check_i3_logs.py test/unit/test_i3_logs.py
	python3 -I -B test/unit/test_i3_logs.py

.PHONY: test-i3-logs
test-notice-wire: test/unit/test_notice_wire.c src/cuvs_util.c src/cuvs_util.h src/cuvs_admission_contract.h
	$(CC) -std=c11 -g -Wall -Wextra -Werror $(ADMISSION_TEST_SAN) -I src \
		-o $@ test/unit/test_notice_wire.c src/cuvs_util.c $(TEST_RT_LIB)
	./$@

EXTRA_CLEAN += test-notice-wire
.PHONY: test-notice-wire

# err_out payload codec (C5, CK-29); ASAN/UBSan for the 511/512/513 B boundary.
test-admission-wire: test/unit/test_admission_wire.c src/cuvs_admission_contract.h \
                     src/cuvs_util.c src/cuvs_util.h src/cuvs_ipc.h
	$(CC) -std=c11 -Wall -Wextra -Werror $(ADMISSION_TEST_SAN) \
		-I src -o $@ test/unit/test_admission_wire.c \
		src/cuvs_util.c $(TEST_RT_LIB)
	./$@

# GPU-only (real CUDA build, not the shim): I3-A wrapper checks (dim%4 padded
# single dataset copy, attach, self-queries). Run after `make all server`.
test-gpu-wrapper-i3: test/gpu/test_wrapper_i3.c src/cuvs_wrapper.o $(CUVS_FOLD_OBJ) src/cuvs_util_server.o src/cuvs_peak_meter_server.o
	$(CC) -std=gnu11 -Wall -Wextra -I src -c $< -o $@.o
	$(CXX) -o $@ $@.o $(filter %.o,$^) $(SHLIB_LINK) -lpthread -lm
	./$@

# Exercise the actual mode-selected wrapper and fold objects from plain C.
.PHONY: test-cagra-fold-build
test-cagra-fold-build: test/unit/test_cagra_fold_build.c test/gpu/test_cagra_fold_early.cpp $(WRAPPER_OBJ) src/cuvs_util_server.o src/cuvs_peak_meter_server.o
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -DCUVS_EXPECT_GPU_FOLD=$(CUVS_FOLD_NATIVE) -c $< -o $@.o
	$(CXX) -std=c++17 -Wall -Wextra -Werror -I src -c test/gpu/test_cagra_fold_early.cpp -o $@.early.o
	$(CXX) -o $@ $@.o $@.early.o $(filter %.o,$^) $(SERVER_LDFLAGS) -Wl,--wrap=_Znwm -lm
	./$@
EXTRA_CLEAN += test-cagra-fold-build test-cagra-fold-build.o test-cagra-fold-build.early.o

.PHONY: test-rmm-fold-backing
test-rmm-fold-backing: test/i3b/gpu_failed_upstream.cu src/cuvs_rmm_failure_resource.hpp src/cuvs_admission_contract.h
	$(NVCC) $(NVCC_FLAGS) -DPGCUVS_BETA3A_GPU_FOLD -I$(CUVS_INCLUDE) -I$(CUVS_RAPIDS_INCLUDE) -I src -c $< -o $@.o
	$(CXX) -o $@ $@.o $(SERVER_LDFLAGS)
	./$@
EXTRA_CLEAN += test-rmm-fold-backing test-rmm-fold-backing.o

.PHONY: test-page-fold-gate-log
test-page-fold-gate-log:
	python3 -B test/page_format/test_fold_gate_log.py

.PHONY: test-cagra-fold-capacity
test-unit: test-cagra-fold-capacity
test-cagra-fold-capacity: test/unit/test_cagra_fold_capacity.c src/cuvs_cagra_fold_capacity.h src/cuvs_admission_contract.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ $<
	./$@
EXTRA_CLEAN += test-cagra-fold-capacity

# The build-OOM flag is per thread (CPU shim mirrors the GPU wrapper).
test-build-oom-thread: test/unit/test_build_oom_thread.c src/cuvs_wrapper_shim_cpu.c src/cuvs_wrapper.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_build_oom_thread.c \
		src/cuvs_wrapper_shim_cpu.c -lpthread -lm $(TEST_RT_LIB)
	./$@

# I3 reply-header fields through the real client against a mock daemon.
test-admission-ipc: test/unit/test_admission_ipc.c src/cuvs_ipc.c src/cuvs_ipc.h \
                    src/cuvs_util.c src/cuvs_build_corpus.c $(wildcard src/*.h)
	$(CC) -std=gnu11 -Wall -Wextra -Werror $(ADMISSION_TEST_SAN) -I src -o $@ \
		test/unit/test_admission_ipc.c src/cuvs_ipc.c src/cuvs_util.c src/cuvs_build_corpus.c \
		-lpthread -lm $(TEST_RT_LIB)
	./$@

test-unit: test-extra-search-ipc
test-extra-search-ipc: test/unit/test_extra_search_ipc.c src/cuvs_ipc.c src/cuvs_ipc.h \
                       src/cuvs_util.c src/cuvs_build_corpus.c $(wildcard src/*.h)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -Dsend=cuvs_test_send -I src -o $@ \
		test/unit/test_extra_search_ipc.c src/cuvs_ipc.c src/cuvs_util.c src/cuvs_build_corpus.c \
		-lpthread -lm $(TEST_RT_LIB)
	./$@

EXTRA_CLEAN += test-extra-search-ipc
.PHONY: test-extra-search-ipc

# #224: a connect() that waits for a full listen backlog retries after EINTR
# and expired slices within the call's budget, a pending cancel ends it as
# CANCELED, and the connection gets its full send timeout back. Linux only: on
# macOS a full backlog refuses at once instead of waiting. The mutations run
# with test-unit there, like test-hnsw-nn-mutations.
ifeq ($(shell uname -s),Linux)
test-unit: test-ipc-connect-eintr test-ipc-connect-eintr-mutations
endif
test-unit: test-ipc-backlog-source
test-ipc-connect-eintr: test/unit/test_ipc_connect_eintr.c src/cuvs_ipc.c src/cuvs_ipc.h \
                        src/cuvs_util.c src/cuvs_build_corpus.c $(wildcard src/*.h)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ \
		test/unit/test_ipc_connect_eintr.c src/cuvs_ipc.c src/cuvs_util.c src/cuvs_build_corpus.c \
		-lpthread -lm $(TEST_RT_LIB)
	./$@

# #224: the daemon listen() backlog holds at least one batch of reconnects.
test-ipc-backlog-source: test/unit/check_listen_backlog_source.py src/pg_cuvs_server.c
	python3 -I -B test/unit/check_listen_backlog_source.py

# Each #224 mutation (retry, slice, cancel, budget, send timeout, backlog) must
# turn a check RED. Client mutants need Linux; elsewhere only the backlog ones run.
test-ipc-connect-eintr-mutations: test/unit/test_ipc_connect_eintr_mutations.py
	CC=$(CC) TEST_RT_LIB="$(TEST_RT_LIB)" python3 -I -B test/unit/test_ipc_connect_eintr_mutations.py

EXTRA_CLEAN += test-ipc-connect-eintr
.PHONY: test-ipc-connect-eintr test-ipc-backlog-source test-ipc-connect-eintr-mutations

# User-visible DETAIL/HINT text built from err_out (C5, CK-8, CK-29).
test-admission-message: test/unit/test_admission_message.c src/cuvs_admission_contract.h \
                        src/cuvs_util.c src/cuvs_util.h src/cuvs_ipc.h
	$(CC) -std=c11 -Wall -Wextra -Werror $(ADMISSION_TEST_SAN) \
		-I src -o $@ test/unit/test_admission_message.c \
		src/cuvs_util.c $(TEST_RT_LIB)
	./$@

# Each mutation of the real header/definition must turn the test RED.
# ADMISSION_MUTATION_OUT must be a new directory.
ADMISSION_MUTATION_OUT ?= admission-contract-mutations
test-admission-contract-mutations: test/unit/test_admission_contract_mutations.py test-admission-contract \
                                   test-admission-wire test-admission-message test-admission-ipc
	python3 -I test/unit/test_admission_contract_mutations.py $(ADMISSION_MUTATION_OUT)

# I3-B cache policy (eviction eligibility, reclaim F fences, usage stats
# file, startup order, sidecar kind). Pure C, no daemon/PG/CUDA.
test-cache-policy: test/unit/test_cache_policy.c src/cuvs_cache_policy.c \
                   src/cuvs_cache_policy.h src/cuvs_util.c src/cuvs_admission_contract.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_cache_policy.c \
		src/cuvs_cache_policy.c src/cuvs_util.c -lm $(TEST_RT_LIB)
	./$@

# CK-36 (a) requester arguments and CK-37 (a) I-FC assignment sites.
test-i3b-source: test/unit/check_i3b_server_source.py src/pg_cuvs_server.c test/unit/check_i3b_docs.py
	python3 test/unit/check_i3b_server_source.py src/pg_cuvs_server.c
	python3 test/unit/check_i3b_docs.py

.PHONY: test-i3b-source

test-unit: test-page-overlay
.PHONY: test-page-overlay
test-page-overlay:
	ulimit -c 0; CC="$(CC)" python3 -I -B test/unit/test_page_overlay_mutations.py


# CK-34: the peak meter under 8 concurrent threads, reset, and off.
test-peak-meter: test/unit/test_peak_meter.c src/cuvs_peak_meter.c src/cuvs_peak_meter.h
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I src -o $@ test/unit/test_peak_meter.c \
		src/cuvs_peak_meter.c -lpthread
	./$@

EXTRA_CLEAN += test-peak-meter
.PHONY: test-peak-meter

EXTRA_CLEAN += test-cache-policy
.PHONY: test-cache-policy

# I3-B status text parsing (INDEX/MEMORY/RECLAIM_STATUS), row visibility and
# busy-message class used by the pg_cuvs_* cache SQL functions. Pure C.
test-status-text: test/unit/test_status_text.c src/cuvs_status_text.c \
                  src/cuvs_status_text.h src/cuvs_ipc.h \
                  test/unit/check_i3b_sql_perms.py test/unit/test_connectivity_sql_contract.py src/pg_cuvs.c \
                  sql/pg_cuvs--0.8.2--0.9.0.sql sql/pg_cuvs--0.9.0.sql sql/pg_cuvs--0.9.0--0.9.1.sql \
                  sql/pg_cuvs--0.9.1.sql sql/pg_cuvs--$(EXTVERSION).sql
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_status_text.c \
		src/cuvs_status_text.c
	./$@
	python3 test/unit/check_i3b_sql_perms.py
	python3 -I -B test/unit/test_connectivity_sql_contract.py

EXTRA_CLEAN += test-status-text
.PHONY: test-status-text

# I4 / ADR-088 managed large_workspace: pure decisions (C) and the build-only
# resource guard (C++, fake resource). Both run on any host.
test-lw-policy: test/unit/test_lw_policy.c src/cuvs_lw_policy.h src/cuvs_admission_contract.h \
                src/cuvs_util.c src/cuvs_util.h src/cuvs_ipc.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_lw_policy.c \
		src/cuvs_util.c $(TEST_RT_LIB)
	./$@

test-lw-guard: test/unit/test_lw_guard.cpp src/cuvs_lw_guard.hpp src/cuvs_lw_policy.h \
               src/cuvs_admission_contract.h
	$(CXX) -std=c++17 -Wall -Wextra -Werror -pthread -I src -o $@ test/unit/test_lw_guard.cpp
	./$@

# Q6: RAFT host frees inside a request wait for its stream (cuvs_host_free_fence.hpp).
test-host-free-fence: test/unit/test_host_free_fence.cpp src/cuvs_host_free_fence.hpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror -pthread -I src -o $@ test/unit/test_host_free_fence.cpp
	./$@

test-host-free-fence-mutations: test/unit/test_host_free_fence_mutations.py test-host-free-fence
	CXX=$(CXX) python3 -I test/unit/test_host_free_fence_mutations.py

# amvacuumcleanup: ANALYZE (analyze_only) never compacts (cuvs_vacuum_policy.h).
test-vacuum-policy: test/unit/test_vacuum_policy.c src/cuvs_vacuum_policy.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_vacuum_policy.c
	./$@

# #238: remembered brute_force unavailable verdict (cuvs_bf_verdict.h) and the
# cuvs_vectors_read failure classes it depends on.
test-bf-verdict: test/unit/test_bf_verdict.c src/cuvs_bf_verdict.h src/cuvs_util.c src/cuvs_util.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I src -o $@ test/unit/test_bf_verdict.c src/cuvs_util.c -lm $(TEST_RT_LIB)
	./$@

# #238: refresh_main_bf_cache consults the verdict before reading `.vectors`,
# notes a failed read with its real return value, and clears the verdict on a
# successful build (source check; also replays one planted defect per rule).
test-unit: test-bf-verdict-source
test-bf-verdict-source: test/unit/check_bf_verdict_source.py src/pg_cuvs_server.c
	python3 -I -B test/unit/check_bf_verdict_source.py

test-vacuum-policy-mutations: test/unit/test_vacuum_policy_mutations.py test-vacuum-policy src/pg_cuvs.c
	CC=$(CC) python3 -I test/unit/test_vacuum_policy_mutations.py

# Resident IVF-PQ kNN precision (fp16 bound) and the auto kNN-graph fallback to ACE.
test-device-norm:
	CXX=$(CXX) python3 -I test/unit/test_device_norm.py

.PHONY: test-device-norm

test-compact-retry:
	CXX=$(CXX) python3 -I test/unit/test_compact_retry.py

.PHONY: test-compact-retry

test-compact-retry-daemon:
	CC=$(CC) python3 -I test/unit/test_compact_retry_daemon_mutations.py

.PHONY: test-compact-retry-daemon

test-hnsw-precision-mutations: test/unit/test_hnsw_precision_mutations.py test-hnsw-policy
	CC=$(CC) python3 -I test/unit/test_hnsw_precision_mutations.py

.PHONY: test-hnsw-sdk-upper-precision
# Requires the exact SDK source and its actual downloaded hnswlib getter header.
test-hnsw-sdk-upper-precision:
	@test -n "$(CUVS_SDK_TEST_SOURCE)" -a -n "$(CUVS_HNSWLIB_HEADER)" || \
	  { echo "set CUVS_SDK_TEST_SOURCE and CUVS_HNSWLIB_HEADER" >&2; exit 2; }
	python3 test/native/hnsw-sdk/test_upper_level_precision.py \
	  --source "$(CUVS_SDK_TEST_SOURCE)" --compiler "$(CXX)" \
	  --hnswlib-header "$(CUVS_HNSWLIB_HEADER)" --mutations \
	  $(if $(CUVS_SDK_TEST_OUTPUT),--output "$(CUVS_SDK_TEST_OUTPUT)")

# CK-27 (a) for the I4 knobs: docs flag rows vs cuvs_lw_policy.h macros.
test-lw-docs: test/unit/check_lw_docs.py docs/reference.md src/cuvs_lw_policy.h
	python3 -I test/unit/check_lw_docs.py

# Each planted I4 defect must turn an I4 test RED. Linux (the daemon fixture
# needs memfd, /proc and shm); run as a non-root user. LW_MUTATION_OUT must be
# a new directory.
LW_MUTATION_OUT ?= lw-mutations
test-lw-mutations: test/unit/test_lw_mutations.py test-lw-policy test-lw-guard test-lw-daemon \
                   test-lw-startup test-lw-docs
	CC=$(CC) CXX=$(CXX) python3 -I test/unit/test_lw_mutations.py $(LW_MUTATION_OUT)

# Converter fixtures include private PG page helpers, with backend stubs and
# section garbage collection. Requires server headers, but no running backend.
ifeq ($(shell uname -s),Darwin)
HNSW_CONVERTER_UNIT_LINK = -Wl,-undefined,dynamic_lookup -Wl,-dead_strip
else
HNSW_CONVERTER_UNIT_LINK = -Wl,--gc-sections \
                          -L$(shell $(PG_CONFIG) --pkglibdir) -lpgcommon -lpgport -lm
endif
HNSW_UNIT_CPPFLAGS ?=
.PHONY: test-hnsw-converter-build
test-hnsw-converter: test-hnsw-converter-build
	./$@

test-hnsw-converter-build: test/unit/test_hnsw_converter.c src/hnsw_export.c \
                           src/hnsw_export.h src/cuvs_util.c $(wildcard src/*.h)
	$(CC) -std=gnu11 -O0 -Wall -Wextra -Wno-unused-parameter \
		-ffunction-sections -fdata-sections -I src \
		-I$(shell $(PG_CONFIG) --includedir) -I$(shell $(PG_CONFIG) --includedir-server) \
		$(HNSW_UNIT_CPPFLAGS) -o test-hnsw-converter test/unit/test_hnsw_converter.c src/cuvs_util.c \
		$(HNSW_CONVERTER_UNIT_LINK) $(TEST_RT_LIB)

EXTRA_CLEAN += test-hnsw-build test-hnsw-build-ipc test-hnsw-owner-lease test-hnsw-ace-quality test-hnsw-converter test-hnsw-admission test-hnsw-explicit-route test-hnsw-policy \
               test-admission-contract test-admission-contract-cxx test-admission-contract-util.o \
               test-admission-wire test-admission-message test-admission-ipc test-build-oom-thread test-hnsw-auto-overflow test-res-pool test-lw-policy test-lw-guard test-failed-allocation test-host-free-fence test-vacuum-policy test-bf-verdict
.PHONY: test-hnsw-build test-hnsw-build-ipc test-hnsw-owner-lease test-hnsw-ace-quality test-hnsw-ace-quality-mutations test-hnsw-converter test-hnsw-admission test-hnsw-explicit-route test-hnsw-policy \
        test-admission-contract test-admission-contract-mutations test-admission-wire \
        test-admission-message test-admission-ipc test-build-oom-thread test-gpu-wrapper-i3 test-hnsw-auto-overflow test-res-pool test-lw-policy test-lw-guard test-lw-mutations test-lw-docs \
        test-host-free-fence test-host-free-fence-mutations test-vacuum-policy test-vacuum-policy-mutations test-bf-verdict test-bf-verdict-source test-hnsw-precision-mutations

# ---- Documentation contract check ---------------------------------------
# CPU-only drift detector for the current version/defaults and release-facing
# caveats. It intentionally does not replace PostgreSQL, GPU, or benchmark QA.
docs-contract-check:
	bash scripts/docs-contract-check.sh

.PHONY: docs-contract-check

# ---- No-GPU regression target -------------------------------------------
# Runs the subset of tests that do NOT require a live GPU daemon.
# Currently this is just the C-level unit tests (test-unit).
#
# All pgregress tests (smoke, cpu_fallback, hnsw_import, …) require a running
# pg_cuvs_server daemon and therefore a GPU.  Use `make installcheck` on the
# GPU VM for the full suite.
#
# Intended for CI jobs on standard (CPU-only) runners — see .github/workflows/ci.yml.
installcheck-nogpu: test-unit
	@echo "[OK] No-GPU checks passed (test-unit)"

.PHONY: installcheck-nogpu

# ---- Isolation-only installcheck ----------------------------------------
# Runs ONLY the isolation suite (concurrent-session correctness), without
# re-running the full REGRESS suite. Mirrors the ISOLATION branch of the PGXS
# `installcheck` rule; ISOLATION_OPTS gets --dbname appended by PGXS when
# ISOLATION is set. Requires a running daemon + GPU, like installcheck.
installcheck-isolation:
	$(pg_isolation_regress_installcheck) $(ISOLATION_OPTS) $(ISOLATION)

# The isolation specs that need a real GPU daemon (ISOLATION_GPU). Requires a running GPU daemon.
installcheck-isolation-gpu:
	$(pg_isolation_regress_installcheck) $(ISOLATION_OPTS) $(ISOLATION_GPU)

# ---- Tier-1 installcheck (CPU-reference shim subset) ----------------------
# The REGRESS suite minus REGRESS_TIER2_ONLY. Used by the tier1-shim CI job;
# requires a (shim) daemon up, same as installcheck.
installcheck-tier1:
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(REGRESS_TIER1)

# Focused source-conversion regression. Also runs in both regression tiers.
installcheck-hnsw-source:
	$(pg_regress_installcheck) $(REGRESS_OPTS) hnsw_source_conversion hnsw_source_maintenance $(HNSW_SOURCE_FILE_TEST)

# Set HNSW_SOURCE_FILE_TEST=hnsw_source_conversion_file with a real GPU daemon.
HNSW_SOURCE_FILE_TEST ?=

.PHONY: installcheck-hnsw-source

# ---- Fault-injection installcheck (CUVS_TEST_HOOKS daemon) ----------------
# #124: REGRESS_FAULT needs the CUVS_OP_INJECT_* opcodes, which only exist in
# a -DCUVS_TEST_HOOKS build (pg_cuvs_server.c) — the production daemon
# rejects them ("unknown op"). Run REGRESS_FAULT against a dedicated
# $(SERVER_TEST_BIN) on its own socket + index dir instead, mirroring the
# test-daemon pattern `gpu-test-daemon` already uses
# (infra/scripts/tests/integration-test.sh): the production pg-cuvs-server
# daemon and its socket/index dir are never touched.
#
# Unlike installcheck-tier1, this does NOT try to point cuvs.socket_path/
# cuvs.index_dir at the test daemon from here (e.g. via ALTER DATABASE) —
# pg_regress drops and recreates its target database at the start of every
# run (see pg_regress.c drop_database_if_exists/create_database), which would
# silently wipe a pre-set ALTER DATABASE default before the tests ever ran.
# Instead, each REGRESS_FAULT .sql file SETs cuvs.socket_path/cuvs.index_dir
# to FAULT_SOCK/FAULT_IDX itself (session-level, first thing after CREATE
# EXTENSION) — safe because these 4 files are reachable ONLY through this
# target now (removed from REGRESS/REGRESS_TIER1 above).
#
# The daemon binds the socket 0660 (pg_cuvs_server.c); this target runs the
# daemon as the calling (non-postgres) OS user, so it needs the same chmod
# 666 widening the production systemd unit and the Tier-1 CI shim daemon do
# — otherwise the postgres-owned backend gets EACCES on connect().
FAULT_SOCK = /tmp/.s.pg_cuvs_test
FAULT_IDX  = /tmp/cuvs_indexes_test

installcheck-fault: server-test
	set -e; \
	echo "[fault] starting CUVS_TEST_HOOKS daemon ($(SERVER_TEST_BIN)) on $(FAULT_SOCK)"; \
	rm -f $(FAULT_SOCK); rm -rf $(FAULT_IDX); mkdir -p $(FAULT_IDX); chmod 0777 $(FAULT_IDX); \
	./$(SERVER_TEST_BIN) --socket $(FAULT_SOCK) --index-dir $(FAULT_IDX) --max-vram-mb 20480 \
		>/tmp/pg_cuvs_fault_daemon.log 2>&1 & \
	DAEMON_PID=$$!; \
	trap 'echo "[fault] cleanup"; \
	      kill $$DAEMON_PID 2>/dev/null || true; wait $$DAEMON_PID 2>/dev/null || true; \
	      rm -f $(FAULT_SOCK); rm -rf $(FAULT_IDX)' EXIT; \
	for i in $$(seq 1 60); do \
		[ -S $(FAULT_SOCK) ] && break; \
		if ! kill -0 $$DAEMON_PID 2>/dev/null; then \
			echo "[fault] test daemon died during startup"; cat /tmp/pg_cuvs_fault_daemon.log; exit 1; \
		fi; \
		sleep 0.5; \
	done; \
	test -S $(FAULT_SOCK) || { echo "[fault] test daemon socket never appeared"; cat /tmp/pg_cuvs_fault_daemon.log; exit 1; }; \
	chmod 666 $(FAULT_SOCK); \
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(REGRESS_FAULT)

.PHONY: installcheck-isolation installcheck-isolation-gpu installcheck-tier1 installcheck-fault

.PHONY: installcheck-isolation

# ---- Benchmark harness (Phase 1.5 #5) ----------------------------------
# Parameterized large-dataset benchmark. Runs the bash harness; N/DIM/K/M
# are passed through ONLY when set on the make command line, so the script's
# own small sanity defaults apply otherwise. Invoked on the VM by `gpu-bench`
# (and `gpu-bench-1m` for the phase-record 1M/1536 completion gate).
benchmark:
	$(if $(N),N=$(N)) $(if $(DIM),DIM=$(DIM)) $(if $(K),K=$(K)) $(if $(M),M=$(M)) \
		bash infra/scripts/benchmark/benchmark.sh

.PHONY: benchmark

# ---- Remote VM orchestration -------------------------------------------
# Local, gitignored config: VM_SSH_HOST (the ssh host, whatever the provider)
# and — for the legacy GCP path only — GCP_INSTANCE, GCP_ZONE, GCP_PROJECT.
#
# The file used to be `.env.gpu`. It holds no secrets (an ssh host alias, a
# conda env name, CUDA_ARCH), but the `.env` prefix trips tooling that refuses
# to read dotenv files, so it is now `gpu.conf`. Both are included and the
# legacy one is read first, so an existing .env.gpu keeps working and gpu.conf
# wins where both define a key.
-include .env.gpu
-include gpu.conf
export

# On GCP the external IP is ephemeral (reassigned on every stop/start), so
# the configured VM_SSH_HOST goes stale and the CURRENT IP has to come from gcloud.
# Brev (the current main provider, see infra/README.md) has no such lookup:
# VM_SSH_HOST *is* the host. So the gcloud probe is opt-in via USE_GCP_IP=1.
#
# Leaving it unconditional cost us twice. It forks gcloud once per $(VM_HOST)
# reference — recursive '=' re-evaluates the shell on every expansion — so a
# `make -n gpu-test-all` (6 references, nothing executed) took 6.7s vs 0.24s
# for a target that never mentions the host. Worse, a successful lookup WINS
# over VM_SSH_HOST: with a Brev host configured but a stale GCP_INSTANCE still
# in the config file and live gcloud auth, every gpu-* target would silently retarget
# the old GCP machine — and `sync` is `rsync --delete`.
# VM_SSH_HOST is provider-neutral; GCP_VM is the pre-Brev name kept as a
# fallback so an existing config file still resolves.
VM_SSH_HOST ?= $(GCP_VM)
RMM_POOL_MB ?= 512
RMM_POOL_MAX_PERCENT ?= 50
GCP_USER ?= ubuntu
VM_IP = $(if $(USE_GCP_IP),$(shell gcloud compute instances describe $(GCP_INSTANCE) --zone $(GCP_ZONE) $(if $(GCP_PROJECT),--project $(GCP_PROJECT)) --format='value(networkInterfaces[0].accessConfigs[0].natIP)' 2>/dev/null),)
VM_HOST = $(if $(VM_IP),$(GCP_USER)@$(VM_IP),$(VM_SSH_HOST))
unexport VM_IP VM_HOST

# Every remote command sources ~/.pg_cuvs_env, the environment file that
# infra/brev/bootstrap.sh wrote on the VM while it built (CONDA_PREFIX, PATH
# with the PG bin dir, LD_LIBRARY_PATH, NVCC, PGHOST). It is the single source
# of truth for the remote toolchain: encoding those paths here is what made the
# GCP-era targets (~/miniforge3, no pg_config on PATH) fail on Brev, and PGXS
# degrades SILENTLY when pg_config is missing, so the build "succeeds" without
# ever compiling. Hence the hard gate — a missing file must stop the target,
# not fall through to a wrong environment.
REMOTE_ENV = { test -f ~/.pg_cuvs_env || { echo 'ERROR: ~/.pg_cuvs_env missing on the VM — run infra/brev/bootstrap.sh first' >&2; exit 1; }; . ~/.pg_cuvs_env; }

.PHONY: vm-start vm-stop sync gpu-build gpu-test gpu-bench gpu-bench-1m gpu-shell \
	gpu-test-unit gpu-test-regress gpu-test-isolation gpu-test-daemon gpu-test-e2e \
	gpu-test-delta-restart gpu-test-all gpu-test-objstore gpu-test-vram gpu-test-maxidx \
	gpu-test-asan-export

vm-start:
	@test -n "$(GCP_INSTANCE)" || (echo "ERROR: set GCP_INSTANCE in gpu.conf"; exit 1)
	@test -n "$(GCP_PROJECT)" || (echo "ERROR: set GCP_PROJECT in gpu.conf"; exit 1)
	gcloud compute instances start $(GCP_INSTANCE) --zone $(GCP_ZONE) --project $(GCP_PROJECT)
	@echo "Waiting for SSH..."
	@until ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no $(VM_HOST) true 2>/dev/null; \
		do sleep 3; done
	@echo "VM ready: $(VM_HOST)"

vm-stop:
	@test -n "$(GCP_PROJECT)" || (echo "ERROR: set GCP_PROJECT in gpu.conf"; exit 1)
	gcloud compute instances stop $(GCP_INSTANCE) --zone $(GCP_ZONE) --project $(GCP_PROJECT)

# rsync local → VM. Excludes build artifacts to avoid clobbering remote .o files
# that were produced on the VM with the correct nvcc toolchain.
sync:
	rsync -avz --delete \
		--exclude '.git' \
		--exclude 'src/*.o' \
		--exclude 'src/*.bc' \
		--exclude '*.so' \
		--exclude '.env.gpu' \
		--exclude 'gpu.conf' \
		./ $(VM_HOST):~/pg_cuvs/

gpu-build:
	ssh -tt $(VM_HOST) "cd ~/pg_cuvs && \
		$(REMOTE_ENV) && \
		make 2>&1 | tee /tmp/pg_cuvs_build.log"

gpu-install:
	ssh -tt $(VM_HOST) "cd ~/pg_cuvs && \
		$(REMOTE_ENV) && \
		sudo -E make install"

# #169: gated on freshness — a stale deployment must not yield a green run.
gpu-test: gpu-verify-deployed
	ssh -tt $(VM_HOST) "cd ~/pg_cuvs && \
		$(REMOTE_ENV) && \
		make installcheck"

gpu-server:
	ssh -tt $(VM_HOST) "cd ~/pg_cuvs && \
		$(REMOTE_ENV) && \
		make server && sudo make install-server"

# ---- #169: deploy = install BOTH artifacts, then reload BOTH processes ------
# gpu-build makes only pg_cuvs.so; the daemon binary needs gpu-server. And an
# installed artifact is not a running one: the .so is loaded at postmaster start
# (shared_preload_libraries) and the daemon is long-lived. Skipping either half
# leaves the harness testing old code with no failure signal — see #169 for the
# three incidents. Prefer this over calling gpu-build/gpu-install by hand.
gpu-deploy: sync gpu-build gpu-install gpu-server gpu-reload gpu-verify-deployed

# Restart PostgreSQL and the daemon, then wait for the daemon to reach `active`
# (first CUDA context takes 12s-3min). Piped over stdin like gpu-postinstall.
gpu-reload:
	ssh $(VM_HOST) "$(REMOTE_ENV) && bash -s" \
		< infra/scripts/setup/reload-deployed.sh

# The staleness guard already existed — gpu-preflight.sh — but gpu-run.sh was
# its only caller, so every `make gpu-test*` ran outside it. Wiring it in here
# is most of what #169 needed; the script itself only gained the
# artifact-vs-source check (its checks 1-4 compare installed against running,
# which cannot see an install that never happened).
gpu-verify-deployed:
	infra/scripts/gpu-preflight.sh

# Idempotent post-install: home traversal perms, libstdc++ symlinks,
# PG role, and shared_preload_libraries setup. Run once after gpu-install.
# The script is piped over stdin (bash -s) rather than inlined to avoid
# fragile nested quoting; plain ssh (no -tt) since it needs no remote TTY.
gpu-postinstall:
	CONDA_ENV=$(CONDA_ENV) ssh $(VM_HOST) "$(REMOTE_ENV) && CONDA_ENV=$(CONDA_ENV) bash -s" \
		< infra/scripts/setup/postinstall.sh

# End-to-end durability smoke: build index, restart daemon, verify reload.
# Piped over stdin (bash -s); plain ssh, no remote TTY needed.
gpu-e2e:
	ssh $(VM_HOST) "$(REMOTE_ENV) && bash -s" < infra/scripts/tests/e2e-smoke.sh

# ---- Integration test suite (Phase 1.5 #3) -----------------------------
# Layered test targets. Unit tests run locally (no toolchain needed);
# the rest run on the GPU VM where PG + CUDA + daemon are available.

# Unit tests for the dependency-free helpers (cuvs_util). Runs on the VM
# for parity with the build toolchain; identical to local `make test-unit`.
gpu-test-unit:
	ssh -tt $(VM_HOST) "cd ~/pg_cuvs && \
		$(REMOTE_ENV) && \
		make test-unit"

# PG regression suite (smoke + cpu_fallback). Requires a daemon up for the
# CREATE INDEX paths; the production pg-cuvs-server systemd unit must be
# active and cuvs.index_dir set to its --index-dir.
gpu-test-regress:
	ssh -tt $(VM_HOST) "cd ~/pg_cuvs && \
		$(REMOTE_ENV) && \
		make installcheck"

# Isolation suite (snapshot-aware tombstone, write/query interleaving). Same
# daemon + GPU prerequisites as gpu-test-regress; runs only the isolation specs.
gpu-test-isolation:
	ssh -tt $(VM_HOST) "cd ~/pg_cuvs && \
		$(REMOTE_ENV) && \
		make installcheck-isolation && make installcheck-isolation-gpu"

# Fault-injection daemon integration tests. Builds the CUVS_TEST_HOOKS
# daemon, drives daemon-down / persist-fault / clean-build scenarios on a
# TEST socket + index dir, then restores the production daemon. Piped over
# stdin (bash -s); CONDA_ENV is forwarded so the script can compile.
gpu-test-daemon:
	ssh $(VM_HOST) "$(REMOTE_ENV) && \
		CONDA_ENV=$(CONDA_ENV) bash -s" \
		< infra/scripts/tests/integration-test.sh

# Phase 3C: real GCS snapshot round-trip + fail-closed certification against an
# EPHEMERAL bucket (created + destroyed by the script). Needs gcloud (/snap/bin)
# and a service account with storage.buckets.create. Piped over stdin (bash -s);
# /snap/bin is added to PATH so gcloud resolves in the non-login ssh shell.
# #165: daemon-created /dev/shm segments must not outlive their request. Needs a
# table `t` with a CAGRA index `t_cagra` on the VM. Piped over stdin (bash -s).
gpu-test-shm-residue:
	ssh $(VM_HOST) "$(REMOTE_ENV) && bash -s" \
		< infra/scripts/tests/shm-residue.sh

# #166 review: the SCM_RIGHTS sidecar handoff must not depend on the daemon's
# umask. Installs a temporary UMask=077 drop-in, exercises both export modes,
# and always removes it again. Piped over stdin (bash -s).
gpu-test-umask:
	ssh $(VM_HOST) "$(REMOTE_ENV) && bash -s" \
		< infra/scripts/tests/umask-hardened-export.sh

gpu-test-objstore:
	ssh $(VM_HOST) "$(REMOTE_ENV) && \
		PATH=\$$PATH:/snap/bin bash -s" \
		< infra/scripts/tests/objstore-roundtrip-e2e.sh

# ADR-065: a daemon started without --max-vram-mb must default to a bounded VRAM
# budget (a fraction of total), not unlimited. Runs a dedicated daemon on a test
# socket and asserts pg_stat_gpu_cache + the startup log. Piped over stdin.
gpu-test-vram:
	ssh $(VM_HOST) "$(REMOTE_ENV) && bash -s" \
		< infra/scripts/setup/vram-budget-default.sh

# MAX_INDEXES soft-cap: more tenants than registry slots must work (build w/o
# ERROR + queries auto-reload evicted indexes to GPU). Dedicated daemon with a
# tiny --max-indexes; asserts evictions>0 and reloads>0. Piped over stdin.
gpu-test-maxidx:
	ssh $(VM_HOST) "$(REMOTE_ENV) && bash -s" \
		< infra/scripts/tests/max-indexes-scale.sh

# End-to-end durability smoke (alias of gpu-e2e for naming symmetry).
gpu-test-e2e:
	ssh $(VM_HOST) "$(REMOTE_ENV) && bash -s" < infra/scripts/tests/e2e-smoke.sh

# Phase 3A pending-delta durability + fail-closed across a daemon restart.
# Piped over stdin (bash -s); plain ssh, no remote TTY needed.
gpu-test-delta-restart:
	ssh $(VM_HOST) "$(REMOTE_ENV) && bash -s" < infra/scripts/tests/delta-restart-e2e.sh

# #101/#114 regression guard: ASAN heap-buffer-overflow in handle_export_adjacency
# on export after a daemon restart. Deliberately NOT in gpu-test-all (5-10 min:
# ASAN daemon build + two restarts + a slow first CUDA context) -- run on demand
# or after touching cuvs_cagra_extract_adjacency / the deserialize/extend
# placeholder path. `cd` is required here (unlike the other piped scripts, which
# never call `make`) because the script builds+installs the ASAN daemon itself.
gpu-test-asan-export:
	ssh $(VM_HOST) "cd ~/pg_cuvs && $(REMOTE_ENV) && bash -s" < infra/scripts/tests/asan-export-restart.sh

# Full ladder: unit -> regress -> isolation -> daemon faults -> e2e durability.
gpu-test-all: gpu-verify-deployed gpu-test-unit gpu-test-regress gpu-test-isolation gpu-test-daemon \
	gpu-test-e2e gpu-test-delta-restart

gpu-server-start:
	ssh -tt $(VM_HOST) "pg_cuvs_server \
		--socket /tmp/.s.pg_cuvs \
		--index-dir \$$(psql -t -c 'SHOW data_directory' | tr -d ' ')/cuvs_indexes \
		--max-vram-mb 20480 --rmm-pool-mb $(RMM_POOL_MB) \
		--rmm-pool-max-percent $(RMM_POOL_MAX_PERCENT) &"

# Default sanity benchmark on the VM (small size from benchmark.sh defaults).
# N/DIM/K/M forwarded to the remote `make benchmark` only when set locally.
gpu-bench:
	@mkdir -p design
	ssh $(VM_HOST) "cd ~/pg_cuvs && \
		$(REMOTE_ENV) && \
		make benchmark $(if $(N),N=$(N)) $(if $(DIM),DIM=$(DIM)) $(if $(K),K=$(K)) $(if $(M),M=$(M)) 2>&1" \
		| tee design/bench_$(shell date +%Y%m%d_%H%M).log

# phase-record completion-gate run: 1M rows x 1536 dim (large VRAM-stress case).
# Explicit target so the heavy run is never the default. Reuses gpu-bench.
gpu-bench-1m:
	$(MAKE) gpu-bench N=1000000 DIM=1536

gpu-cohere:
	@echo "[gpu-cohere] Launching Cohere 1M benchmark (nohup, async)"
	ssh $(VM_HOST) "cd ~/pg_cuvs && \
		$(REMOTE_ENV) && \
		nohup bash bench/legacy/run_cohere.sh \
			--n $(if $(N),$(N),1000000) \
			--gpu $(if $(GPU),$(GPU),0) \
			> /tmp/cohere_bench.log 2>&1 &" && \
	echo "[gpu-cohere] Started. Poll with:" && \
	echo "  make gpu-cohere-log" && \
	echo "  make gpu-cohere-result"

gpu-cohere-log:
	ssh $(VM_HOST) "tail -50 /tmp/cohere_bench.log"

gpu-cohere-result:
	@N=$(if $(N),$(N),1000000); \
	ssh $(VM_HOST) "cat ~/pg_cuvs/bench/results/cohere_N$${N}_summary.csv 2>/dev/null \
		|| echo 'Not ready yet — check: make gpu-cohere-log'"

.PHONY: gpu-cohere gpu-cohere-log gpu-cohere-result

gpu-shell:
	ssh -tt $(VM_HOST)

# Report the VM's current power state and external IP (ephemeral IPs change on
# stop/start, so the configured VM_SSH_HOST can go stale). Usage: make vm-ip
vm-ip:
	@gcloud compute instances describe $(GCP_INSTANCE) --zone $(GCP_ZONE) \
		$(if $(GCP_PROJECT),--project $(GCP_PROJECT)) \
		--format='value(status,networkInterfaces[0].accessConfigs[0].natIP)'
.PHONY: vm-ip

# Run ad-hoc SQL on the VM's postgres DB from stdin. For introspection and
# Phase 3K integration checks. Usage: make gpu-sql < query.sql  (or via heredoc)
gpu-sql:
	ssh -o StrictHostKeyChecking=accept-new $(VM_HOST) \
		"$(REMOTE_ENV) && \
		psql -d $(if $(DB),$(DB),postgres) -P pager=off -A -F '|'"
.PHONY: gpu-sql

# ---- Comparative ANN benchmark (bench/legacy/anbench) -------------------------
# pg_cuvs vs pgvector(hnsw/ivfflat) vs raw cuvs vs faiss-gpu/cpu on the same
# real dataset (Cohere wiki en, 1024d, cosine). Runs entirely on the VM.
.PHONY: gpu-anbench gpu-anbench-5m gpu-anbench-agg

# One corpus size (default 1M). Override: make gpu-anbench N=5000000
gpu-anbench:
	@mkdir -p design/anbench
	ssh $(VM_HOST) "cd ~/pg_cuvs && N=$(if $(N),$(N),1000000) KS=$(if $(KS),$(KS),10,100) \
		bash bench/legacy/anbench/run_all.sh 2>&1" \
		| tee design/anbench/run_N$(if $(N),$(N),1000000)_$(shell date +%Y%m%d_%H%M).log

# Large tier (GPU-feasible max at 1024d).
gpu-anbench-5m:
	$(MAKE) gpu-anbench N=5000000

# Aggregate results into summary.csv/txt + Pareto plots, then pull back locally.
# aggregate.py needs a python; the env file activates the C/C++ build env, which
# has none, so the interpreter comes from the sibling bench env (bootstrap.sh's
# cuvs_bench) resolved off CONDA_PREFIX. matplotlib stays best-effort — plots are
# optional in aggregate.py and the /opt prefix is root-owned.
gpu-anbench-agg:
	@mkdir -p design/anbench
	ssh $(VM_HOST) "cd ~/pg_cuvs && $(REMOTE_ENV) && \
		PY=\$$(dirname \$$CONDA_PREFIX)/cuvs_bench/bin/python && \
		\$$PY -m pip install -q matplotlib 2>/dev/null; \
		\$$PY bench/legacy/anbench/aggregate.py"
	rsync -avz $(VM_HOST):~/pg_cuvs/design/anbench/ design/anbench/

# Convenience: full cycle on the VM. Routed through gpu-deploy (#169): the old
# form (sync build install test) rebuilt only the extension and restarted
# nothing, so it could test code that was never deployed.
gpu-cycle: gpu-deploy gpu-test

# I4 / ADR-088 through the actual daemon source (Linux CPU CI, non-root for the
# EACCES branch): knob refusal, oom_score_adj, CK-3b lw_pending, per-caller
# policy through handle_build/_multi/build_sharded, spill NOTICE, pool status.
EXTRA_CLEAN += test-lw-daemon
.PHONY: test-lw-daemon
test-lw-daemon:
	$(CC) -std=gnu11 -O1 -DPGCUVS_CPU_SHIM -ffunction-sections -fdata-sections -I src \
		test/unit/test_lw_daemon.c src/cuvs_ipc.c src/cuvs_util.c \
		src/cuvs_objstore.c src/cuvs_build_corpus.c src/cuvs_wrapper_shim_cpu.c \
		src/cuvs_cache_policy.c src/cuvs_peak_meter.c src/cuvs_page_overlay.c src/cuvs_hnsw_upper.c \
		-Wl,--gc-sections -lm -lpthread -lrt -lcurl -lssl -lcrypto -o test-lw-daemon
	./test-lw-daemon

EXTRA_CLEAN += test-page-fold-lifetime test-page-fold-events
.PHONY: test-page-fold-lifetime test-page-fold-events
ifeq ($(shell uname -s),Linux)
test-unit: test-page-fold-lifetime test-page-fold-events
endif
test-page-fold-lifetime:
	$(CC) -std=gnu11 -O1 -DPGCUVS_CPU_SHIM -ffunction-sections -fdata-sections -I src \
	    test/unit/test_page_fold_lifetime.c src/cuvs_ipc.c src/cuvs_util.c \
	    src/cuvs_objstore.c src/cuvs_build_corpus.c src/cuvs_wrapper_shim_cpu.c \
	    src/cuvs_cache_policy.c src/cuvs_peak_meter.c src/cuvs_page_overlay.c src/cuvs_hnsw_upper.c \
	    -Wl,--gc-sections -Wl,--wrap=cuvs_cagra_free -lm -lpthread -lrt -lcurl -lssl -lcrypto -o $@
	./$@

test-page-fold-events:
	$(CC) -std=gnu11 -O1 -DPGCUVS_CPU_SHIM -ffunction-sections -fdata-sections -I src \
	    test/unit/test_page_fold_events.c src/cuvs_ipc.c src/cuvs_util.c \
	    src/cuvs_objstore.c src/cuvs_build_corpus.c src/cuvs_wrapper_shim_cpu.c \
	    src/cuvs_cache_policy.c src/cuvs_peak_meter.c src/cuvs_page_overlay.c src/cuvs_hnsw_upper.c \
	    -Wl,--gc-sections -lm -lpthread -lrt -lcurl -lssl -lcrypto -o $@
	./$@

EXTRA_CLEAN += test-physical-build-guard
.PHONY: test-physical-build-guard test-physical-build-mutations
ifeq ($(shell uname -s),Linux)
test-unit: test-physical-build-guard

# the host upper-layer builder: levels and file size fixed by the input whatever the thread count,
# every element above level 0 linked on level 1, level 0 without self links or duplicates, the cancel stop.
test-hnsw-upper: test/unit/test_hnsw_upper.c src/cuvs_hnsw_upper.c src/cuvs_hnsw_upper.h
	$(CC) -std=gnu11 -O2 -Wall -Wextra -I src -o $@ test/unit/test_hnsw_upper.c src/cuvs_hnsw_upper.c -lm -pthread
	./$@

test-unit: test-hnsw-upper
EXTRA_CLEAN += test-hnsw-upper
.PHONY: test-hnsw-upper
test-admission-contract-mutations: test-physical-build-mutations
endif
test-physical-build-guard:
	$(CC) -std=gnu11 -O1 -DPGCUVS_CPU_SHIM -ffunction-sections -fdata-sections -I src \
		test/unit/test_physical_build_guard.c src/cuvs_ipc.c src/cuvs_util.c \
		src/cuvs_objstore.c src/cuvs_build_corpus.c src/cuvs_wrapper_shim_cpu.c \
		src/cuvs_cache_policy.c src/cuvs_peak_meter.c src/cuvs_page_overlay.c \
		-Wl,--gc-sections -lm -lpthread -lrt -lcurl -lssl -lcrypto -o $@
	./$@

test-physical-build-mutations:
	CC=$(CC) python3 -I test/unit/test_physical_build_mutations.py

# I3-E / C8: rejection counters, memory_status I3-E columns, build_status rows
# for in-flight unlocked CAGRA builds, through the actual daemon source (CPU
# shim; the build entry points and the RMM pool reader are fixtures).
test-i3e-observation:
	$(CC) -std=gnu11 -O1 -DPGCUVS_CPU_SHIM -ffunction-sections -fdata-sections -I src \
		test/unit/test_i3e_observation.c src/cuvs_ipc.c src/cuvs_util.c src/cuvs_status_text.c \
		src/cuvs_objstore.c src/cuvs_build_corpus.c src/cuvs_wrapper_shim_cpu.c \
		src/cuvs_cache_policy.c src/cuvs_peak_meter.c src/cuvs_page_overlay.c \
		-Wl,--gc-sections -lm -lpthread -lrt -lcurl -lssl -lcrypto -o test-i3e-observation
	./test-i3e-observation

# I3-E SQL surface and docs (no PG): the I3-E block in the 0.9.0 upgrade script
# and in every install script from 0.9.0 on,
# RETURNS TABLE vs daemon headers/kinds, grants, C9 table coverage.
test-i3e-sql:
	python3 -I test/unit/check_i3e_sql.py .

# I4 review B-2 (b): start the real CPU-shim daemon (no fixture) and check
# /proc/<pid>/oom_score_adj, the startup lines and the pool-status IPC reply.
# Linux, non-root. The daemon binary is built here without PGXS.
EXTRA_CLEAN += test-lw-startup test-lw-startup-server
.PHONY: test-lw-startup
LW_SHIM_DAEMON_SRCS = src/pg_cuvs_server.c src/cuvs_ipc.c src/cuvs_util.c src/cuvs_objstore.c \
                      src/cuvs_build_corpus.c src/cuvs_wrapper_shim_cpu.c \
                      src/cuvs_cache_policy.c src/cuvs_peak_meter.c src/cuvs_page_overlay.c src/cuvs_hnsw_upper.c
test-lw-startup:
	$(CC) -std=gnu11 -O1 -DPGCUVS_CPU_SHIM -D_POSIX_C_SOURCE=200809L -I src $(LW_SHIM_DAEMON_SRCS) \
		-lm -lpthread -lrt -lcurl -lssl -lcrypto -o test-lw-startup-server
	$(CC) -std=gnu11 -Wall -Wextra -I src -o test-lw-startup test/unit/test_lw_startup.c \
		src/cuvs_ipc.c src/cuvs_util.c src/cuvs_build_corpus.c -lm -lpthread -lrt
	./test-lw-startup ./test-lw-startup-server

# Actual daemon admission with physical GPU headroom fixture only (Linux CPU CI).
EXTRA_CLEAN += test-rmm-pending
.PHONY: test-rmm-pending
test-rmm-pending:
	$(CC) -std=gnu11 -O1 -DPGCUVS_CPU_SHIM -ffunction-sections -fdata-sections -I src \
		test/unit/test_rmm_pending.c src/cuvs_ipc.c src/cuvs_util.c \
		src/cuvs_objstore.c src/cuvs_build_corpus.c src/cuvs_wrapper_shim_cpu.c \
		src/cuvs_cache_policy.c src/cuvs_peak_meter.c src/cuvs_page_overlay.c \
		-Wl,--gc-sections -lm -lpthread -lrt -lcurl -lssl -lcrypto -o test-rmm-pending
	./test-rmm-pending

# ADR-089 AR8 pool-capacity premise through the actual daemon source (Linux
# CPU CI): Q6 shape refused, order AR1 -> AR8 -> AR6 -> make_room, nothing
# evicted or reserved, err_out and [admit] fields, knob parsing.
EXTRA_CLEAN += test-ar8-daemon
.PHONY: test-ar8-daemon
test-ar8-daemon:
	$(CC) -std=gnu11 -O1 -DPGCUVS_CPU_SHIM -ffunction-sections -fdata-sections -I src \
		test/unit/test_ar8_daemon.c src/cuvs_ipc.c src/cuvs_util.c \
		src/cuvs_objstore.c src/cuvs_build_corpus.c src/cuvs_wrapper_shim_cpu.c \
		src/cuvs_cache_policy.c src/cuvs_peak_meter.c src/cuvs_page_overlay.c src/cuvs_hnsw_upper.c \
		-Wl,--gc-sections -lm -lpthread -lrt -lcurl -lssl -lcrypto -o test-ar8-daemon
	./test-ar8-daemon

# hnsw route decisions through the actual daemon source (Linux CPU CI): op 40
# and op 23 (real client) send the status-13 DETAIL bytes he hb ge gb rp rl,
# the plan's disk refusal text, and the decided route on [hnsw_route].
EXTRA_CLEAN += test-hnsw-route-daemon
.PHONY: test-hnsw-route-daemon
test-hnsw-route-daemon:
	$(CC) -std=gnu11 -O1 -DPGCUVS_CPU_SHIM -ffunction-sections -fdata-sections -I src \
		test/unit/test_hnsw_route_daemon.c src/cuvs_ipc.c src/cuvs_util.c \
		src/cuvs_objstore.c src/cuvs_build_corpus.c src/cuvs_wrapper_shim_cpu.c \
		src/cuvs_cache_policy.c src/cuvs_peak_meter.c src/cuvs_page_overlay.c \
		-Wl,--gc-sections -lm -lpthread -lrt -lcurl -lssl -lcrypto -o test-hnsw-route-daemon
	./test-hnsw-route-daemon

# Fold-enabled daemon admission with the CPU shim and controlled claims.
EXTRA_CLEAN += test-page-fold-admission
FOLD_ADMISSION_CASE ?=
.PHONY: test-page-fold-admission
ifeq ($(shell uname -s),Linux)
test-unit: test-page-fold-admission
endif
test-page-fold-admission:
	$(CC) -std=gnu11 -O1 -DPGCUVS_CPU_SHIM -DCUVS_CAGRA_FOLD_ENABLED -ffunction-sections -fdata-sections -I src \
		test/unit/test_hnsw_route_daemon.c src/cuvs_ipc.c src/cuvs_util.c \
		src/cuvs_objstore.c src/cuvs_build_corpus.c src/cuvs_wrapper_shim_cpu.c \
		src/cuvs_cache_policy.c src/cuvs_peak_meter.c src/cuvs_page_overlay.c src/cuvs_hnsw_upper.c \
		src/cuvs_cagra_fold_unavailable.c \
		-Wl,--gc-sections -lm -lpthread -lrt -lcurl -lssl -lcrypto -o $@
	./$@ $(FOLD_ADMISSION_CASE)

# Each planted AR8 defect must turn an AR8 test RED. The daemon cases need
# Linux (elsewhere they are SKIP and the run fails). AR8_MUTATION_OUT must be
# a new directory.
AR8_MUTATION_OUT ?= ar8-mutations
.PHONY: test-ar8-mutations
test-ar8-mutations: test/unit/test_ar8_mutations.py
	CC=$(CC) python3 -I test/unit/test_ar8_mutations.py $(AR8_MUTATION_OUT)

# Installed-extension integration check; requires a live CPU-shim or GPU daemon.
.PHONY: test-stale-interim
test-stale-interim:
	python3 test/stale_interim.py

# K3 input digest against a live backend and a scripted fake daemon
# (test/k3_fake_daemon.c); needs the installed extension, a superuser
# session (PGDATABASE/PGHOST) and PGCUVS_SERVER_LOG = the server log file.
.PHONY: test-k3-digest
test-k3-digest:
	python3 test/k3_digest_integration.py

# #203: PostgreSQL/CUDA-free structural checks (real file adapter also lives in
# test-hnsw-converter). No ANN search-policy defaults are altered.
test-connectivity: test/unit/test_connectivity.c src/cuvs_connectivity.h
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Isrc $< -o $@
	./$@

EXTRA_CLEAN += test-connectivity
.PHONY: test-connectivity

test-unit: test-connectivity

# Explicit exact scans of pg_cuvs_hnsw on a real PostgreSQL 16 backend (CPU only, no
# daemon). The four scripts need a disposable cluster that preloads pg_cuvs, with the
# vector and pg_cuvs extensions created in HNSW_EXACT_DB; they create their own schemas
# and refuse to run unless the data directory matches HNSW_EXACT_DATADIR.
#   make test-hnsw-exact-pg HNSW_EXACT_DATADIR=/var/lib/postgresql/16/main \
#        HNSW_EXACT_SOCKET=/var/run/postgresql HNSW_EXACT_PORT=5432 HNSW_EXACT_USER=$$USER
HNSW_EXACT_OUT ?= /tmp/pgcuvs-hnsw-exact-results
HNSW_EXACT_DB ?= postgres
HNSW_EXACT_PORT ?= 5432
HNSW_EXACT_SOCKET ?= /var/run/postgresql
HNSW_EXACT_USER ?= $(USER)
HNSW_EXACT_DATADIR ?= /var/lib/postgresql/16/main
.PHONY: test-hnsw-exact-pg
test-hnsw-exact-pg:
	@test -n "$(HNSW_EXACT_DATADIR)" || { echo "set HNSW_EXACT_DATADIR"; exit 2; }
	rm -rf $(HNSW_EXACT_OUT)
	mkdir -p $(HNSW_EXACT_OUT)
	run=$$(date +%s); \
	for t in pg product_pg rls_pg protected_pg; do \
	    python3 test/unit/test_hnsw_exact_$$t.py --output $(HNSW_EXACT_OUT)/$$t \
	        --schema hx$${run}_$$t --socket $(HNSW_EXACT_SOCKET) --port $(HNSW_EXACT_PORT) \
	        --user $(HNSW_EXACT_USER) --database $(HNSW_EXACT_DB) \
	        --expected-data-directory $(HNSW_EXACT_DATADIR) --pg-bin $$(pg_config --bindir) \
	        || exit 1; \
	done

# Isolated native PostgreSQL CPU regression; server/dev binaries and psql needed.
# It starts its own Unix-socket cluster and does not connect to an existing DB.
.PHONY: test-connectivity-pg-interrupts
test-connectivity-pg-interrupts:
	python3 test/unit/test_connectivity_pg_interrupts.py --output $(or $(CONNECTIVITY_TEST_OUTPUT),/tmp/pgcuvs-connectivity-pg-results)

# Requires >=1.25GiB free tmpfs (Docker: --tmpfs /pgtmp:size=1600m,mode=1777).
.PHONY: test-connectivity-pg-workspace
test-connectivity-pg-workspace:
	python3 test/unit/test_connectivity_pg_workspace.py --tmpfs $(or $(CONNECTIVITY_TEST_TMPFS),/pgtmp) --output $(or $(CONNECTIVITY_TEST_OUTPUT),/tmp/pgcuvs-connectivity-workspace-results)

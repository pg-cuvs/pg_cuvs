
## 프로젝트 문서 구조

문서의 권한과 갱신 규칙은 [`docs/doc-map.md`](docs/doc-map.md)가 단일 기준이다.
현행 동작을 확인할 때는 코드/SQL과 `docs/reference.md` 및 `ARCHITECTURE.md`를
우선한다. `README.md`는 개요·quickstart, `BENCHMARK.md`는 측정 결과만 담당한다.

| 파일 | 역할 | 상태 |
|------|------|------|
| `docs/reference.md` | 현행 AM, GUC, SQL 함수, view, search mode 표면 | Current / Verified |
| `ARCHITECTURE.md` | 현재 프로세스·IPC·수명주기·실패 경계 | Current / Verified |
| `ROADMAP.md` | GitHub 이슈와 연결된 미완료 작업·release readiness | Active planning |
| `design/decisions.md` | 결정의 배경과 대안의 역사 | Historical / append-only |
| `design/specs/phase-record.md` | Phase별 as-built 기록과 검증 증거 | Frozen history |

새 설계 결정은 ADR로 기록하되 `supersedes`와 현행 SSOT 링크를 남긴다. 구현이
끝난 항목은 ROADMAP의 active backlog에서 제거하고 코드·테스트 증거를 링크한다.

---

## 하네스: pg_cuvs 개발 스킬

**목표:** GPU VM 기반 개발 루프의 재현성 확보와 코드/테스트 품질의 주기적 개선.

**트리거:**
- VM 신규 생성·재빌드·환경 구성(brev, bootstrap.sh, libcuvs, ldconfig 사고) → `gpu-vm-provision` 스킬
- 부트스트랩된 VM의 일상 개발 루프(sync, gpu-build, gpu-test, 빌드 함정) → `pg-cuvs-dev` 스킬
- 코드 전체 훑기, 버그/중복 헌팅, 테스트 속도 점검 → `quality-loop` 스킬
- 단순 질문은 스킬 없이 직접 응답 가능. 상세 경계는 각 스킬 description 참조.

**변경 이력:**
| 날짜 | 변경 내용 | 대상 | 사유 |
|------|----------|------|------|
| 2026-08-05 | 배포 신선도 게이트 도입 — 스킬 워크플로우 표에 `gpu-deploy`/`gpu-server`/`gpu-reload`/`gpu-verify-deployed` 및 "무엇을 고치면 무엇을 다시 돌려야 하나" 매트릭스 추가 | skills/pg-cuvs-dev, Makefile, infra/scripts/gpu-preflight.sh | #169 — 하네스가 구 코드를 조용히 테스트하는 3갈래. `gpu-preflight.sh`는 있었으나 `gpu.conf` 파싱 버그로 실행 불가였고 호출자도 `gpu-run.sh` 하나뿐이었다 |
| 2026-07-28 | quality-loop 스킬 추가 | skills/quality-loop | 품질 루프 도입 (버그·중복·테스트 속도 감사) |
| 2026-07-28 | GPU 스킬 2종 트리거 경계 정리, 프로비저닝 중복 섹션을 참조로 대체, disk-recovery.md dead link 수정 | skills/pg-cuvs-dev, skills/gpu-vm-provision | 하네스 진단에서 트리거 중복·이중 관리 발견 |
| 2026-07-28 | 하네스 포인터 등록 | CLAUDE.md | 변경 추적 진입점 부재 |
| 2026-07-28 | GPU 스킬 2종 GCP → Brev 전환 (bootstrap 재빌드 모델, shadeform/sm_80, vm-start·stop 무효화 명시, GCP는 레거시 섹션으로) | skills/gpu-vm-provision, skills/pg-cuvs-dev, skills/quality-loop | GCP 크레딧 만료, Brev(Massed Compute A100)가 현 메인 프로바이더 |
| 2026-07-28 | 트리거 공백 보완("VM 올려줘"→provision, "VM 내려줘/삭제"→dev), 함정 F·H에 bootstrap gotcha SSoT 인용 추가 | skills/gpu-vm-provision, skills/pg-cuvs-dev | 책임 분리 검토에서 트리거 틈·드리프트 위험 발견 |

---

## Karpathy Coding Guidelines

> Behavioral guidelines to reduce common LLM coding mistakes. These apply to ALL code-writing agents.

### 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

### 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

### 5. Commit Often

**Keep work units small and commit frequently.**

- Don't mix multiple intentions in a single commit.
- Cut at the point where "I'd want to be able to revert to here."
- If modifying multiple files or working on a feature unit, create a branch.

**The same rule applies to the PR, not just the commit.** This repo squash-merges
by default (`viewerDefaultMergeMethod: SQUASH`), so **the PR is the unit that
lands on `main`** — a PR carrying four intentions becomes one four-intention
commit no matter how cleanly its branch was committed. That is how #113 became a
single 88-file commit and #115 fused a CUDA wrapper fix with the systemd unit,
the `GCP_VM` → `VM_SSH_HOST` rename, and 19 documents. Neither wrapper fix can
now be reverted without taking the infrastructure with it, and `git bisect` lands
on the whole bundle.

Split a PR when any of these is true:

| Signal | Example from #115 |
|---|---|
| Structural + behavioral together (Tidy First) | Makefile rename (structural) + wrapper sentinel fix (behavioral) |
| Two changes revertible independently | the systemd unit vs. the TAIL CONTRACT fix |
| Different subsystems with no shared cause | `src/*.cu` vs. `infra/brev/` |
| Docs that don't describe this change | playbook edits riding along with a bug fix |

Docs and tests that exist *because of* the change belong **with** it — the test
proving a fix, the comment explaining it. Split by cause, not by file type.

When a PR already bundles intentions and re-splitting is not worth it, merge it
with `gh pr merge --rebase` (allowed here) so the branch's separated commits
survive on `main`. Prefer splitting; rebase is the fallback, and it only works if
the commits were clean to begin with.

> Follow the git plugin skills for branch names, commit messages, and PR format:
> `git:branch-name-convention`, `git:commit-message-convention`, `git:pr-convention`

---

*These guidelines are working if: fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.*

---

## Kent Beck Coding Guidelines

> TDD and Tidy First principles. Maintain code quality through test-driven development and structure/behavior separation.
> These apply to ALL code-writing agents.

### 1. Red → Green → Refactor

**Failing test → Make it pass → Clean up. Follow this order.**

- Write a failing test first for each small unit of functionality.
- Implement only the minimum code needed to make the test pass.
- Refactor only after the test is passing.
- Always re-run tests after refactoring.

### 2. Tidy First

**Never mix structural changes with behavioral changes.**

- Structural changes (renaming, extracting methods, moving code) → behavior stays the same
- Behavioral changes (adding features, modifications) → structure stays the same
- If both are needed: structure first, behavior second
- Separate each into its own commit

### 3. Make It Work, Make It Right, Make It Fast

**Work → Clean up → Optimize. Never skip steps.**

- First make it work (simplest thing that works)
- Then clean up the code (remove duplication, reveal intent)
- Optimize only when necessary (don't optimize speculatively)

### 4. Commit Discipline

**Only commit when tests pass.**

- Commit only after all tests pass and linter warnings are resolved
- One commit = one logical unit of change
- State in the commit message whether it is a structural or behavioral change
- One PR = one logical unit too — under squash merge the PR is what lands on
  `main`, so clean commits under a bundled PR still collapse. See "Commit Often"
  in the Karpathy section for the split criteria.

> Follow the git plugin skills for branch names, commit messages, and PR format:
> `git:branch-name-convention`, `git:commit-message-convention`, `git:pr-convention`

### 5. FIRST Principles

Every test must be:
- **Fast**: milliseconds, not seconds
- **Independent**: no shared state between tests
- **Repeatable**: same result regardless of environment
- **Self-validating**: pass/fail, no manual inspection
- **Timely**: written just before the code it tests

### 6. AAA Pattern

Structure every test as:
```
// Arrange — set up test data and dependencies
// Act     — execute the function/method
// Assert  — verify expected outcome
```

### 7. Test Pyramid

- 70% Unit Tests: fast, isolated, numerous
- 20% Integration Tests: module boundaries
- 10% Acceptance Tests: user-facing scenarios

Don't invert the pyramid. Integration-heavy suites are slow and fragile.

---

*These guidelines are working if: tests are written before implementation, structural and behavioral changes never appear in the same commit, and "make it work" always precedes "make it right".*

---

## Boris Cherny Coding Guidelines

> Workflow orchestration principles. Improve task quality through divide-and-conquer, learning, elegance, and autonomy.
> Applies complementarily alongside Karpathy (code quality) and Kent Beck (TDD).
> These apply to ALL code-writing agents.

### 1. Divide and Conquer

**Break complex problems down and tackle them in parallel.**

- Actively use subagents to keep the main context clean.
- Delegate research, exploration, and analysis to subagents.
- One subagent = one focused goal.
- **Broad, parallelizable, or repository-wide tasks** (full codebase analysis, multi-module changes, parallel research) → delegate to a subagent or team. Simple multi-step sequences (read → edit → verify) stay in the main context.

### 2. Learn from Every Mistake

**After user corrections, always document the lesson learned.**

- Write rules to prevent the same mistake from happening again.
- Iteratively refine lessons until the error rate drops.
- Review relevant project lessons at the start of each session.

### 3. Demand Elegance, But Know When to Stop

**For non-trivial changes, ask: "Is there a more elegant solution?"**

- If something feels hacky: "Synthesize everything learned so far and implement an elegant solution."
- Skip this process for simple, obvious fixes — no over-engineering.
- Challenge your own work before submitting.

### 4. Fix Bugs Autonomously

**When given a bug report within the current task's scope, fix it. Don't ask the user how.**

- Point to logs, errors, and failing tests — then resolve them.
- Keep the user's context switching at zero.
- Fix CI failures on your own without being told.
- If a fix would touch code outside the current task's scope, flag it rather than silently modifying it.

### 5. Pick the Right Parallelization Primitive

**Context isolates cleanly → spawn subagents. Agents need to challenge each other → use `CreateTeam`.**

- Task scope is bounded by a directory, module, or investigation target → spawn a subagent per scope.
- Cross-validation, competing hypotheses, or findings that need debate → use `CreateTeam` so agents message each other directly.
- Cross-layer work (frontend + backend + tests) with coordination needed → use `CreateTeam`.
- Subagents: lower cost, results flow back to main context. One focused goal per subagent.
- Agent Teams: higher cost, agents communicate directly — use only when inter-agent communication adds value.

---

*These guidelines are working if: complex tasks are divided among subagents, mistakes lead to documented lessons, and bugs are fixed autonomously without asking the user how.*

<!-- ooo:START -->
<!-- ooo:VERSION:0.39.1 -->
# Ouroboros — Specification-First AI Development

> Before telling AI what to build, define what should be built.
> As Socrates asked 2,500 years ago — "What do you truly know?"
> Ouroboros turns that question into an evolutionary AI workflow engine.

Most AI coding fails at the input, not the output. Ouroboros fixes this by
**exposing hidden assumptions before any code is written**.

1. **Socratic Clarity** — Question until ambiguity <= 0.2
2. **Ontological Precision** — Solve the root problem, not symptoms
3. **Evolutionary Loops** — Each evaluation cycle feeds back into better specs

```
Interview -> Seed -> Execute -> Evaluate
    ^                               |
    +------- Evolutionary Loop -----+
```

## ooo Commands

Each command loads its agent/MCP on-demand. Details in each skill file.

| Command | Loads |
|---------|-------|
| `ooo` | — |
| `ooo interview` | `ouroboros:socratic-interviewer` |
| `ooo seed` | `ouroboros:seed-architect` |
| `ooo run` | MCP required |
| `ooo evolve` | MCP: `evolve_step` |
| `ooo evaluate` | `ouroboros:evaluator` |
| `ooo unstuck` | `ouroboros:{persona}` |
| `ooo status` | MCP: `session_status` |
| `ooo setup` | — |
| `ooo help` | — |

## Agents

Loaded on-demand — not preloaded.

**Core**: socratic-interviewer, ontologist, seed-architect, evaluator,
wonder, reflect, advocate, contrarian, judge
**Support**: hacker, simplifier, researcher, architect
<!-- ooo:END -->

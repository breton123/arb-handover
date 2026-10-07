# AGENTS.md

## 1. Purpose

This repository is a latency-first Solana HFT platform.

The long-term system is not a single arbitrage bot. It is a reusable platform for:

- high-speed Solana data ingestion
- shred and transaction reconstruction
- local chain/state reconstruction
- protocol and pool state
- strategy execution
- transaction construction/signing/sending
- deterministic capture and replay
- latency measurement and performance research

Arbitrage is the first strategy running on the platform, not the architecture itself.

The most important reusable components are the **data plane** and **state engine**.

---

# 2. Primary Engineering Objective

The critical path is:

```text
packet received
    ↓
decoded/reconstructed
    ↓
state updated
    ↓
opportunity detected
    ↓
transaction prepared
    ↓
signed
    ↓
sent
```

Latency through this path is a core competitive advantage.

Engineering priorities are therefore:

1. Correctness
2. Determinism
3. Measurability
4. Latency
5. Throughput
6. Maintainability

Do not sacrifice correctness for speculative performance.

Do not sacrifice measurable performance for architectural convenience.

Performance claims must be supported by measurements.

---

# 3. Language Policy

Production hot-path code should be written in **C** unless there is a strong documented reason otherwise.

Rust, Python, or other languages may be used for:

- reference implementations
- protocol research
- correctness oracles
- fixture generation
- analysis scripts
- deployment tooling
- benchmark analysis
- offline research

Non-C code must not enter the production hot path without an explicit architectural decision.

---

# 4. Repository Architecture

The repository should remain divided into clear subsystems.

Expected top-level architecture:

```text
src/
    net/
    shred/
    state/
    protocols/
    strategies/
    execution/
    telemetry/

apps/
tests/
bench/
tools/
docs/
configs/
```

Conceptually:

```text
DATA PLANE
    ↓
STATE ENGINE
    ↓
PROTOCOL STATE
    ↓
STRATEGY RUNTIME
    ↓
EXECUTION ENGINE
```

Subsystem boundaries are intentional.

Agents must not bypass them for convenience.

---

# 5. Core Architectural Rule

## Strategies must not own infrastructure.

Strategy code must not directly implement:

- packet receiving
- shred decoding
- account storage
- generic state reconstruction
- signing infrastructure
- generic transaction sending
- feed racing
- generic telemetry

Likewise, infrastructure must not contain strategy-specific assumptions unless explicitly designed as a reusable primitive.

Example:

BAD:

```text
src/state/raydium_arb_special_case.c
```

GOOD:

```text
src/state/
src/protocols/raydium/
src/strategies/arb/
```

Reusable infrastructure belongs below the strategy layer.

---

# 6. Data and State Are First-Class Systems

The state engine is not part of the arbitrage strategy.

It must be usable by:

- arbitrage
- market making
- trigger-based strategies
- research tools
- replay
- diagnostics
- future unknown strategies

Live and replay inputs should eventually converge on the same state-processing code.

Preferred model:

```text
LIVE INPUT ──────┐
                 ├──> STATE ENGINE
REPLAY INPUT ────┘
```

Do not create separate state semantics for live and offline operation.

---

# 7. Dependency Direction

Dependencies should generally flow downward:

```text
apps
 ↓
strategies
 ↓
protocols
 ↓
state
 ↓
shred/data
 ↓
networking
 ↓
core utilities
```

Execution may be consumed by strategies/apps but should remain independently reusable.

Lower-level systems must not depend on higher-level strategy code.

Examples:

`state` must not import `strategies/arb`.

`net` must not know what a Raydium pool is.

`protocols/raydium` must not own NIC receive logic.

If a dependency would violate this direction, stop and reconsider the architecture.

---

# 8. Hot Path Rules

The steady-state hot path must be obvious from the code.

Avoid hidden work.

The following require explicit justification inside hot-path code:

- heap allocation
- freeing memory
- blocking syscalls
- locks
- mutexes
- condition variables
- dynamic dispatch
- indirect function calls in tight loops
- unnecessary copying
- large stack allocations
- string formatting
- logging
- filesystem access
- dynamic configuration lookup
- repeated hashing
- unnecessary atomic operations

Target rule:

> No `malloc()` or `free()` in the steady-state production hot path.

Prefer:

- startup allocation
- fixed-capacity structures
- arenas
- memory pools
- ring buffers
- contiguous arrays
- integer IDs instead of repeated key lookup
- precomputed data
- prebuilt transaction layouts
- single-writer ownership where practical

Do not optimize blindly.

Measure before and after.

---

# 9. Ownership Model

Every important runtime structure should have clear ownership.

Agents introducing shared mutable state must document:

- who creates it
- who owns it
- who may mutate it
- who may read it
- how lifetime is controlled
- whether synchronization is required

Prefer:

```text
one writer
many readers
```

over uncontrolled shared mutation.

Prefer passing small descriptors or IDs rather than transferring large objects between stages.

Example:

```c
typedef struct {
    uint32_t pool_id;
    uint64_t sequence;
    uint64_t rx_tsc;
} pool_update_t;
```

instead of copying entire pool/account structures through queues.

---

# 10. Public Interfaces

Subsystem interfaces should be small and explicit.

Do not expose internal structures simply because doing so is convenient.

Header files should define the contract.

Implementation details should remain private where practical.

Public interfaces should avoid:

- unnecessary genericity
- callbacks where direct calls suffice
- opaque ownership
- hidden allocation
- surprising mutation
- expensive implicit conversions

If an API sits on the hot path, its cost should be understandable by inspection.

---

# 11. Protocol Isolation

Protocol-specific code belongs under:

```text
src/protocols/<protocol>/
```

Example:

```text
src/protocols/raydium/
    decode.c
    state.c
    math.c
    adapter.c
```

Protocol-specific layouts, instructions, math, account parsing, or state transitions must not leak into generic infrastructure unless they represent a genuine reusable abstraction.

Adding a new protocol should not require redesigning networking or generic state storage.

---

# 12. Application Binaries

Programs under `apps/` should primarily compose reusable components.

Examples:

```text
apps/searcher
apps/capture
apps/replay
apps/peer_monitor
apps/diagnostics
```

Large reusable implementations should not live inside application entry points.

BAD:

```text
apps/searcher/main.c
    8,000 lines of networking/state/arb code
```

GOOD:

```text
apps/searcher/main.c
    configuration
    initialization
    component wiring
    lifecycle
```

---

# 13. Capture and Replay

Deterministic capture/replay is a core system capability.

Any major data-plane or state feature should consider whether it can be:

1. captured
2. replayed
3. reproduced
4. regression tested

Replay should reuse production decoding and state logic wherever possible.

Do not create fake replay-only logic that behaves differently from production.

When fixing a timing/state bug, create a regression fixture when practical.

---

# 14. Telemetry

Latency measurement is part of the architecture.

Important stages should preserve timing information where practical.

Examples:

```text
rx
decode
reconstruct
state_apply
protocol_update
strategy
tx_build
sign
send
```

Prefer a monotonic high-resolution timestamp suitable for latency measurement.

Avoid converting timestamps to human-readable formats inside the hot path.

Telemetry collection must itself be benchmarked if it affects critical execution.

---

# 15. Performance Changes

Never claim that a change is faster because it "should be faster."

Performance changes require evidence.

When changing hot-path code:

1. record the existing benchmark
2. make the change
3. run the same benchmark
4. compare latency/distribution
5. check correctness
6. record important results

Where useful report:

- minimum
- median
- p95
- p99
- maximum
- cycles
- throughput

Do not optimize only for averages when tail latency matters.

---

# 16. Correctness Before Micro-Optimization

Do not introduce difficult low-level optimization into code whose behaviour is not yet verified.

Preferred progression:

```text
correct reference
    ↓
tests
    ↓
benchmark
    ↓
profile
    ↓
optimize
    ↓
benchmark again
```

When implementing unfamiliar Solana serialization, protocol layouts, or cryptographic behaviour, reference implementations may be used to prove correctness before replacing them with optimized C.

---

# 17. Testing Requirements

Behavioural changes require tests.

Use the smallest appropriate test level:

### Unit tests

For:

- parsing
- serialization
- math
- state transitions
- protocol decoding
- data structures

### Integration tests

For:

- subsystem interactions
- decoder → state
- state → protocol
- strategy → execution

### Replay tests

For:

- real packet sequences
- ordering bugs
- slot boundaries
- protocol behaviour
- production regressions

### Benchmarks

For:

- packet parsing
- state mutation
- lookup structures
- protocol updates
- opportunity evaluation
- transaction construction
- signing
- queues/rings

A benchmark is not a correctness test.

A correctness test is not a benchmark.

---

# 18. Documentation

Architecture decisions must exist outside individual developers' heads.

Each major subsystem should eventually contain a README describing:

```text
Purpose
Responsibilities
Non-responsibilities
Inputs
Outputs
Public API
Data ownership
Thread ownership
Memory ownership
Failure behaviour
Performance expectations
Testing
```

The **Non-responsibilities** section is important.

It prevents subsystem scope from expanding indefinitely.

---

# 19. Architecture Decision Records

Significant architectural decisions should be documented under:

```text
docs/adr/
```

Format:

```text
# Title

## Context

## Decision

## Alternatives Considered

## Consequences
```

Create an ADR when changing things such as:

- ownership models
- persistent formats
- event formats
- major subsystem boundaries
- threading architecture
- queue design
- state model
- capture format
- networking architecture
- external dependencies

Do not create ADRs for trivial implementation details.

---

# 20. Stable Formats

Treat persisted or externally consumed formats carefully.

Examples:

- capture files
- replay files
- snapshots
- telemetry records
- shared-memory layouts
- wire structures

Do not silently change them.

If compatibility matters, use:

- magic values
- versions
- explicit sizes
- migration or compatibility logic

Document format changes.

---

# 21. Multi-Agent Development

This repository is expected to have multiple humans and multiple coding agents working concurrently.

Agents must minimize overlap.

Before starting work:

1. identify the subsystem being changed
2. read this file
3. read relevant subsystem documentation
4. inspect existing interfaces
5. identify files likely to change
6. avoid touching unrelated files

Prefer tasks with clear ownership boundaries.

Examples:

```text
Agent A -> Raydium adapter
Agent B -> replay engine
Agent C -> RX telemetry
Agent D -> pool table benchmark
```

Avoid assigning multiple agents broad tasks such as:

```text
"improve the whole searcher"
```

---

# 22. Git and Worktree Policy

Each independent human/agent task should use its own branch.

Preferred:

```text
feature/state-replay
feature/raydium-decoder
perf/rx-ring
fix/slot-ordering
```

When multiple agents operate simultaneously, prefer separate Git worktrees.

Example:

```text
worktrees/
    louis-state/
    friend-protocols/
    agent-replay/
    agent-rx/
```

Agents must not share a working tree while making independent edits.

---

# 23. Change Scope

Agents should make the smallest coherent change that solves the assigned problem.

Do not perform unrelated cleanup.

Do not rename large parts of the repository during an unrelated task.

Do not rewrite functioning code purely for style.

Do not change public APIs unless necessary.

If an apparently small task requires a major architectural change, stop and document why.

---

# 24. Cross-Subsystem Changes

Cross-subsystem changes deserve extra scrutiny because they create merge conflicts and coupling.

Before modifying another subsystem's public contract:

1. determine whether the change can remain local
2. explain why the existing contract is insufficient
3. update affected tests
4. update relevant documentation
5. create an ADR if the change is architectural

Agents should not casually modify shared headers used by many components.

---

# 25. Agent Communication

When finishing a task, provide a concise handoff containing:

```text
Goal
Files changed
Behaviour changed
Tests added/run
Benchmarks run
Known limitations
Follow-up work
```

If a task is incomplete, say exactly what remains.

Never represent untested behaviour as verified.

Never represent benchmark assumptions as measured results.

---

# 26. TODO Policy

Do not leave vague TODOs.

BAD:

```c
// TODO: optimize this
```

GOOD:

```c
// TODO(perf): benchmark replacing pubkey hash lookup with pool_id direct indexing.
// Current p99 lookup: see docs/benchmarks/pool_lookup.md
```

TODOs should describe:

- what remains
- why it matters
- what blocks completion where relevant

---

# 27. Error Handling

Cold-path failures may use richer diagnostics.

Hot-path errors should avoid expensive formatting.

Unexpected malformed network input must not corrupt state.

Parsing code must perform required bounds checks.

Do not remove correctness checks simply to save latency without proving the input invariant elsewhere.

Fatal invariants should fail visibly rather than silently corrupt state.

---

# 28. Logging

Logging must not accidentally enter latency-critical loops.

Prefer:

- counters
- ring-buffered telemetry
- sampled events
- deferred formatting

instead of synchronous formatted logs.

Debug logging that materially changes timing must not be used for latency conclusions.

---

# 29. Configuration

Configuration should generally be resolved during startup.

Avoid repeated string/config lookups in the hot path.

Convert configuration into efficient runtime representations during initialization.

Example:

```text
"raydium" string
    ↓ startup
PROTOCOL_RAYDIUM integer enum
```

---

# 30. External Dependencies

Dependencies require justification.

Before adding one, consider:

- runtime cost
- allocation behaviour
- transitive dependencies
- maintenance status
- auditability
- binary size
- whether it enters the hot path

Prefer small, auditable implementations for critical primitives where reasonable.

Do not reimplement complex security-sensitive primitives casually.

---

# 31. Security-Critical Code

Cryptography, signing, key handling, and transaction authorization require additional care.

Never:

- log private keys
- commit secrets
- expose signing material to unrelated components
- duplicate key material unnecessarily

Keep signing interfaces narrow.

Performance work must not weaken key safety.

---

# 32. Build Quality

The project should maintain strict compiler warnings.

New warnings should not be ignored without reason.

Prefer a build configuration that eventually supports:

```text
-Wall
-Wextra
-Wpedantic
-Wconversion where practical
-Wshadow where practical
```

Sanitizer builds should exist for development/testing where practical:

```text
ASan
UBSan
TSan where applicable
```

Production benchmarks must not be performed under sanitizers.

---

# 33. Coding Style

Prefer code that is mechanically simple and easy to inspect.

Use:

- explicit types
- clear ownership
- short hot-path functions
- predictable control flow
- descriptive names
- fixed-width integers for protocol/state layouts

Avoid clever abstractions that obscure machine behaviour.

Comments should explain **why**, not restate obvious code.

---

# 34. Data Layout

Data layout is an architectural concern.

When designing hot structures, consider:

- cache-line size
- alignment
- AoS vs SoA
- false sharing
- read/write frequency
- prefetch behaviour
- NUMA ownership
- object size
- lookup pattern

Do not pad or align structures speculatively without measurement unless required for correctness.

Document deliberately cache-aligned structures.

---

# 35. Threading

Thread creation and CPU assignment should be explicit.

Critical threads should eventually have documented responsibility.

Example:

```text
RX core
reconstruction core
state core
search core
TX core
```

Do not introduce thread pools into the hot path simply for convenience.

Avoid migration of latency-sensitive threads where CPU pinning is intentionally used.

Cross-thread communication must have documented ownership and queue semantics.

---

# 36. Performance Regression Discipline

A function being "cleaner" is not enough justification for making the hot path slower.

If a structural change causes a measurable latency regression, explicitly document the trade-off.

Large regressions require architectural justification.

Maintainability and performance are both requirements.

---

# 37. Temporary Research Code

Experimental code is allowed, but it must be clearly separated.

Use:

```text
tools/
experiments/
bench/
```

rather than inserting experimental logic into production components.

Once an experiment becomes production functionality:

- move it into the correct subsystem
- add tests
- document it
- remove obsolete experimental paths

---

# 38. No Premature Generalization

Do not build abstractions for hypothetical future requirements unless they clearly reduce current complexity.

We expect future:

- protocols
- strategies
- feeds
- send paths

but interfaces should evolve from real requirements.

Simple duplicated code can sometimes be preferable to a bad generic abstraction.

---

# 39. Refactoring Rules

Before large refactors:

1. ensure tests cover existing behaviour
2. establish benchmarks for affected hot paths
3. refactor without behaviour change
4. verify tests
5. rerun benchmarks

Do not combine large refactors with new functionality unless necessary.

---

# 40. Definition of Done

A task is complete when applicable items are satisfied:

- code compiles
- relevant tests pass
- new behaviour is tested
- hot-path changes are benchmarked
- no unrelated files were modified
- public interfaces remain coherent
- documentation reflects architectural changes
- known limitations are documented
- handoff notes are provided

---

# 41. Rules for Agents

Before editing:

- read this file
- inspect the relevant subsystem
- understand ownership and dependency direction
- avoid unrelated edits

While editing:

- preserve subsystem boundaries
- keep changes focused
- protect correctness
- avoid hidden hot-path work
- add tests
- measure performance-sensitive changes

Before finishing:

- build the project
- run relevant tests
- run relevant benchmarks when required
- inspect the diff
- remove debugging artefacts
- update documentation where necessary
- summarize the handoff

---

# 42. Final Principle

The system should become faster without becoming impossible to reason about.

The ideal hot path is:

- explicit
- deterministic
- measurable
- cache-conscious
- allocation-free in steady state
- minimally synchronized
- easy to replay
- easy to benchmark
- easy to inspect

Architecture exists to let multiple people move quickly **without destroying those properties**.
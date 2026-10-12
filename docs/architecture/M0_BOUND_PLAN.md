# M0 Bound ApplicationPlan — typed preflight contract

**Status:** implemented as a draft slice in [PR #9](https://github.com/qigao/turbo-interact/pull/9). This is **not** a StreamGraph v2 runner, Turbo-Flow plugin, Android connector, or verified model loader.

## API progression

- \`ti_application_spec_validate\`: previous **shape-only** preflight, with no graph/model/provider proof.
- \`ti_application_bindings_validate\`: **bound preflight**, using a borrowed trusted catalog of typed graph ports/links, model bytes and an action Provider capability snapshot.
- \`ti_application_plan_compile_bound\`: validates before allocation, then publishes a plan that deep-copies semantic targets, exact port contracts and graph links. It retains no model bytes, callback, graph context or device handle.
- \`ti_application_plan_action_capabilities\`: reports required vs selected Provider capabilities from the immutable compile snapshot. This is **not live Android authorization**.
- \`ti_application_plan_bound_port/link\`: return copies of the frozen logical bridge ports/edges, not executable native graph nodes.

Existing \`ti_application_plan_compile\` remains an **explicitly selected shape-only operation** for standalone M0 tests and does not secretly attempt to bind data/model providers. Product integration requiring bound artifacts must use \`compile_bound\`; there is no fallback from its rejection.

## Graph Port contracts

Only these wiring profiles are currently admitted by bound preflight:

| Plan | Required link direction | Typed values |
| --- | --- | --- |
| With Turbo-Flow | StreamGraph output -> Turbo-Flow input | Observation, exact schema_id |
| With Turbo-Flow | Turbo-Flow output -> SCXML input | Decision, exact schema_id |
| Explicit direct plan | StreamGraph output -> SCXML input | Observation, exact schema_id |

Graph role, port direction/kind, \`schema_id\`, duplicate IDs and duplicate input bindings are checked before any external effects. Every declared target schema must have an Observation link from StreamGraph. No raw CPU/GPU FrameLease is admitted as an Observation payload. Graph IDs and port catalog descriptions are supplied by the embedding host: this step does **not** call the actual upstream Graph compilers, and must be supplemented with qualified adapter checks against their installed SDKs.

## Model artifact proof levels

The registered model has an exact ID, expected 32-byte digest, schema ID, non-empty immutable bytes and a borrowed **trusted SHA-256 callback**.

A bound preflight fails when:

- The model ID is unresolved, duplicated or unused by the plan.
- Target declared model digest/schema differs from the artifact descriptor.
- The trusted SHA-256 provider cannot hash all model bytes.
- The returned digest differs from the expected digest.

TurboInteract does **not** embed another cryptographic hash implementation and does **not** open model files by itself. In production, the embedding host must use a qualified crypto provider and ensure the callback hashes the **same immutable bytes it will pass to the model loader**. The current unit fixture uses the SHA-256 \`abc\` known-answer vector with a constrained fake callback to test preflight behavior; it does not qualify a production crypto provider. Pack signatures, trusted origin, model inference compatibility and actual file-open integrity require separate gates.

## Action capability binding

The trusted host supplies \`required_action_capabilities\`, derived from a reviewed Task/SCXML action plan; a selected provider supplies \`provider_id\`, \`capabilities\` and an \`authorized\` snapshot. Preflight requires all needed bits, rejects unknown bits, refuses unlisted/unauthorized providers and never selects a fallback backend. At runtime the Action Owner must **recheck current permission, session/display generation and conflict ownership**. A successful snapshot is not a permission token.

## Boundaries and acceptance

Tests execute native C11 shape/binding checks, immutable deep-copy snapshots and installed C11/C++17 link consumers on Windows/Linux/macOS, plus Linux ASan/UBSan. The bound unit fixture covers schema mismatch, missing hash provider, wrong digest, mismatched bytes, insufficient capabilities, explicit no-DataGraph plans, queue capacity bounds and target identity resolution.

Not provided: live GPU FrameStore/pinning, graph instance materialization, signed App Packs, actual model file hashes without an injected crypto provider, Android authorization enforcement, or end-to-end latency/conformance. Those remain tracked by [#2](https://github.com/qigao/turbo-interact/issues/2), [#3](https://github.com/qigao/turbo-interact/issues/3), [#4](https://github.com/qigao/turbo-interact/issues/4), and downstream issues.

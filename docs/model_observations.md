# Native model observations

The package advertises `model-state@1`. A negotiated `observe` or
`poll_observations` result can carry the root's latest native serving-model
observation alongside the existing lifecycle result. The capability needs the
Logonomic SDK extension that permits an absent native provider; there is no
separate provider or sequence protocol.

## Root evidence and cadence

The existing interaction reader consumes `llm.request` from the current
session's `agents/main/wire.jsonl`. A fact must identify `agentId: "main"` and
`kind: "loop"`; its literal `model` and optional literal `provider` are projected.
`modelAlias`, configuration, `usage.record`, and message-wrapper metadata are
not serving-model evidence. Compaction and prediction requests are excluded.

This source is present in the repository's captured
[`native-completed/wire.jsonl`](../tests/fixtures/native-completed/wire.jsonl).
The native source contract is also explicit in Kimi's
[`llmRequestOps.ts`](https://github.com/MoonshotAI/kimi-code/blob/6e35efa98402b2a5a198aa929ef178e79ab26bb4/packages/agent-core-v2/src/agent/loop/llmRequestOps.ts)
and
[`wire-manifest.d.ts`](https://github.com/MoonshotAI/kimi-code/blob/6e35efa98402b2a5a198aa929ef178e79ab26bb4/packages/agent-core-v2/docs/wire-manifest.d.ts).

The reader starts at the native `SessionStart` frontier already captured by the
helper. Historical wire records and an unfinished record crossing that frontier
cannot become facts for the new process. Each observation uses the binding's
source-captured activation nonce and the native completed-record byte position
as its ordering value. It neither reads a current host identity at delivery time
nor creates a second sequence protocol. A repeated scan repeats the same source
position; the host's existing ordering barrier rejects that duplicate.

The existing filesystem observer schedules acquisition. The latest confirmed
root fact is carried at the end of its normal observation window and by
subsequent polls; several native requests within one acquisition are coalesced
to the latest one. No model-only lifecycle event, Session Log row, additional
filesystem poll, or model timer is introduced. Missing, malformed, oversized,
or auxiliary model records leave the last valid fact intact.

## Coverage boundaries

Root parsing is covered by `kimi_model_tests`, including source-frontier
isolation, model changes, negotiation, source order, and exclusion of auxiliary,
billing and child records. This is a fixture gate, not a live Kimi account run.

Child facts remain unconfirmed. The current lifecycle decoder maps native
agent identifiers into call/run identities, while the agent-local wire reports
the native identifier. A model must not be attached until that own-run mapping
is established; parent models are never inherited.

Structured ACP responses currently expose selected-model configuration, which
does not establish the model that served the inspected operation. Structured
model observations are therefore omitted. Native interactive and structured
account-free gates are still required before claiming those cells complete.

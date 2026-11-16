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
billing and child records. The production adapter and helper compile with the
official Linux development SDK, and this registered test passes. These are
component gates; package conformance and host timeline acceptance are separate.

### Native Kimi Code 2.6.2 check (2026-11-16)

The official npm CLI completed an actual request to a loopback Chat Completions
endpoint using a fresh profile, with telemetry and updates disabled. The
production `kimi_helper` captured its native `SessionStart` frontier through a
capture transport. The model request used `native-root-model`, and the CLI
printed the endpoint's reply. This check did not use an account or a remote
model service.

The resulting root wire emitted **no `llm.request` record**. The current bundle
declares `LlmRequest`, but its request observer no longer dispatches that event.
Consequently this CLI's root badge remains unconfirmed. The implementation
covers native versions that emit the documented request record and the older
captured fixture; the manifest declaration alone is not a live gate.

The same run distinguishes per-step usage from serving-model identity:

| Native observation | Actual value |
|---|---|
| Loopback request `model` | `native-root-model` |
| Root `profile.bind.modelAlias` | `scripted-alias` |
| Root `usage.record.model`, `usageScope: "turn"` | `scripted-alias` |
| Root `llm.request` records | None |

This is not merely an aggregate-usage limitation. Native
[`requestObservation.ts`](https://github.com/MoonshotAI/kimi-code/blob/6e35efa98402b2a5a198aa929ef178e79ab26bb4/packages/agent-core-v2/src/agent/loop/requestObservation.ts)
passes `request.modelAlias` to the usage service, and
[`usageAgentModel.ts`](https://github.com/MoonshotAI/kimi-code/blob/6e35efa98402b2a5a198aa929ef178e79ab26bb4/packages/agent-core-v2/src/session/usage/usageAgentModel.ts)
copies it into `UsageRecord.model`. Resolving that alias through configuration
would manufacture serving-model evidence, so it remains excluded.

The reproducible Linux probe is
[`kimi_native_model_probe.py`](../tests/kimi_native_model_probe.py). Run it through
the build queue with an official CLI, the built production helper, and a new
output directory:

```sh
queued-build --slots 1 -- python3 tests/kimi_native_model_probe.py \
  --cli /path/to/kimi-code/dist/main.mjs \
  --helper /path/to/build/kimi_helper \
  --output-directory /path/to/new/native-model-probe
```

It uses `bwrap` to isolate the real home mount, preserves the actual home
environment selector, and writes only its fresh fixture directory. It records
native stdout, stderr and helper delivery. It exits nonzero when the source
cell is unconfirmed, including on 2.6.2; successful model completion alone is
not reported as a model-observation pass.

Child facts remain unconfirmed. The current lifecycle decoder maps native
agent identifiers into call/run identities, while the agent-local wire reports
the native identifier. A model must not be attached until that own-run mapping
is established; parent models are never inherited.

Structured ACP responses currently expose selected-model configuration, which
does not establish the model that served the inspected operation. Structured
model observations are therefore omitted. Native model-observation and
structured gates are still required before claiming those cells complete.

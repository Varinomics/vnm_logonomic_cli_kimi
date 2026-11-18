# Native model observations

Kimi publishes the most recent model it observes for the root agent through
`model-state@1`. Negotiated `observe` and `poll_observations` results can carry
the sample, including a page with no lifecycle events. Several requests within
one acquisition are coalesced to the latest sample.

The source is a complete `llm.request` record in the bound session's
`agents/main/wire.jsonl` with `agentId: "main"` and `kind: "loop"`. The record's
`model` is the literal request model. Its `provider` identifies a transport
protocol, which can serve requests for many backends, so the package omits the
optional backend field.

The helper captures the source frontier at native SessionStart. Acquisition
excludes earlier records and a partial record crossing that frontier. It binds
the completed record to the activation nonce and uses its ending byte position
for source order. Replaying an observation preserves both values. The package
uses its existing filesystem subscription.

## Native coverage

Kimi Code 2.4.1 emitted the root request with its literal model in an isolated
loopback run. Kimi Code 2.6.2 and 2.6.3 completed a request without emitting
that event. A 2.6.3 run with distinct root and child models recorded configured
aliases in their respective usage wires. The session log listed both request
models with the same turn and step and no agent ID. These sources cannot
establish a serving model for a particular agent. A session with no accepted
request record contributes no model update. Child models and structured ACP
selections are omitted.

The isolated Linux [native probe](../tests/kimi_native_model_probe.py) checks
for a root record against a loopback endpoint using a fresh profile, the
official CLI, and the built helper. It exits nonzero if the native CLI finishes
without the request record.

- [Kimi 2.4.1 wire producer](https://github.com/MoonshotAI/kimi-code/blob/2c73f3f50af4fa7f2540ca0d2e33b07a1408e3a7/packages/agent-core-v2/src/agent/llmRequester/llmRequesterService.ts#L864-L893)
- [Kimi 2.6.3 request observation and logging](https://github.com/MoonshotAI/kimi-code/blob/b0de2fbd2de04d87689225aafd3912e5d193b62d/packages/agent-core-v2/src/agent/loop/requestObservation.ts)

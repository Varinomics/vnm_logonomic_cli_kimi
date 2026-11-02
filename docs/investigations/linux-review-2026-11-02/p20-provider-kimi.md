# P20: Kimi resume, readers, and quota cleanup

## Outcome and continuation

This is an investigation checkpoint for findings 46, 68, 69, and 138. Current
source inspection supports the resume-hook defect and identifies the reader
failure paths. It does not establish that a real Kimi installation produced an
oversized or deeply nested offending wire record. No compiled fixture or native
quota cleanup has run at this checkpoint.

The owner repository is `Varinomics/vnm_logonomic_cli_kimi`. Work is on
`work/linux-review-20261102-p20-provider-kimi`, based on the `master` source
retrieved at task start (`567363dd2450deb3265141a6be485f379ed99d86`). The canonical
checkout remains on `master`; the implementation worktree began clean. This
report is the first intended publication. Its draft PR link and executed gate
results will be recorded when available.

The next action is to execute a resume-preparation regression using the genuine
three-hook configuration, then correct only the required SessionStart check.
Reader fixtures and Linux dependency preparation can proceed independently.
Native Windows child cleanup remains a separate gate.

## Findings and evidence

| Finding | Current evidence | Disposition at this checkpoint | Remaining evidence |
|---|---|---|---|
| 46 | `has_start_hook()` compares the complete configuration with `updated_config()`, which now installs five hooks. | Source-confirmed: a valid previous three-hook configuration fails the resume precondition because permission hooks are absent. Correct the SessionStart-specific check after the exact fixture fails. | Compiled preparation regression and negative hook cases. |
| 68 | The interaction reader parses every complete line before checking its type. Parser and inventory failures escape into the shared lifecycle observer. | Source-confirmed failure paths; native trigger unestablished. Investigate valid irrelevant records separately from required interaction records and provenance loss. | Executed oversized/deep-record and post-admission inventory fixtures; safe remedy assessed against the existing observation contract. |
| 69 | The usage reader fully parses each line before filtering `metadata`, `forked`, and `usage.record`; line buffering also enforces the document bound. | Source-confirmed rejection path; native trigger unestablished. Required usage and ownership evidence must remain strict. | Executed packaged usage-cache fixtures with valid metadata and usage around irrelevant records. |
| 138 | `Owned_server::~Owned_server()` calls `terminate()`, waits up to 1,000 ms, then kills and waits for completion if the child remains alive. | Source-confirmed cleanup sequence. The additional wait applies to a still-running child that does not exit after `terminate()`, including unsuccessful graceful cleanup after a successful request. | Native Windows inert-console-child timing and no-survivor check; POSIX graceful-shutdown coverage. |

### Resume admission

[`has_start_hook()`](../../../src/kimi_config.cpp) currently uses the fixed point
of the complete managed-configuration transformation as its predicate.
[`prepare_resume()`](../../../src/kimi_preparation.cpp) needs the installed
SessionStart route after verifying the selected launcher and bound account.
Permission hooks are independent of that start-hook precondition. A new launch
rewrites the configuration, so the demonstrated source defect does not imply
lost session history. The correction must preserve parsing, the expected helper
command, relevant hook validity, and selected account/session identity.

### Reader isolation and provenance

[`Interaction_reader::acquire()`](../../../src/kimi_interactions.cpp) enforces
a 4 MiB aggregate pending-record bound and parses complete records using the
SDK's depth/member limits before its `accept()` method inspects `type`.
[`scan()` and `update_observation()`](../../../src/kimi_observer.cpp) do not
contain those failures within the optional reader. In contrast, the main wire
reader discards overlong lines and skips parser-rejected records. This source
difference is independently testable without claiming an observed native Kimi
producer failure.

Inventory handling has two distinct cases. The
[`SessionStart` helper](../../../src/kimi_helper.cpp) already converts a failed
frontier capture into an explicit null interaction frontier, preserving session
admission; existing permission fixtures cover an inventory above 256 entries
at this point. Growth above the bound after successful admission is discovered
inside the continuous observer and can fail the shared observation call.

The current capability exposes one continuous observation outcome for both
subagent and attention events. Dropping an admitted interaction reader while
reporting success could strand accepted pending attention without a truthful
loss signal. A blanket catch-and-disable change therefore is not justified by
the fact that frontier capture is optional. The post-admission capacity case
needs an explicit disposition, and rewritten history must still fail because
it cannot prove which prompts belong to the admitted process.

[`refresh_file()`](../../../src/kimi_usage.cpp) consumes the first native metadata
record for creator provenance, fork boundaries for inherited ownership, and
canonical usage events for accepted accounting and correction. Any filtering
must preserve these records, the first-record position, native byte offsets,
raw/prefix digests, and stable-file checks. Host predecessor streams also use
the line reader; provider-native record handling must not relax their schema.

Neither a substring search for `interaction.`/`usage.record` nor a tolerant
parser that collapses duplicate top-level keys establishes a trustworthy record
type. No global SDK limit increase, customer-file deletion, malformed consumed
record skipping, or unproved numeric correction is part of this checkpoint.

## Validation actually performed

| Check | Inputs | Actual result |
|---|---|---|
| Assignment integrity | P20 ZIP manifest | All 11 listed payload sizes and SHA-256 values matched. |
| Current provider source | GitHub `master` snapshot at task start | All 92 repository blobs matched their Git object IDs; the exact source tree and upstream commit were restored locally. |
| Publication trigger inspection | `.github/workflows/ci.yml` | The provider workflow declares `workflow_dispatch` only. No private hosted workflow was dispatched. |
| Source/caller inspection | Configuration, preparation, interaction, observer, usage, quota cleanup, and focused fixture sources | Findings above are source evidence. |
| Compiled local fixtures | Standalone Kimi project | Pending SDK/source preparation; no pass claimed. |

Focused gates are `kimi_preparation_tests`, `kimi_observer_tests`,
`kimi_permission_tests`, relevant `kimi_usage_cache_tests_*` phases, and
applicable quota fixtures. Compiler-probing configuration will reserve one
queued build slot; builds will reserve no more than four slots with matching
parallel width and effective persistent ccache. The selected SDK, dependency
source inventory, commands, and actual results will be added after execution.

## Native follow-up

On Windows, run `kimi_quota_startup_tests` with an owned inert console child.
Establish the still-live cleanup duration and verify that the exact child does
not survive probe completion. Linux compilation and POSIX termination do not
prove the Windows path. Preserve the POSIX graceful-shutdown behavior and test
it through the same owned fixture where available.

No authenticated account/quota campaign, live-user UI operation, private hosted
CI, or release qualification has been performed or inferred from these checks.

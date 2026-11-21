# Kimi CLI add-on for Logonomic

[![Provider CI](https://github.com/Varinomics/vnm_logonomic_cli_kimi/actions/workflows/ci.yml/badge.svg?branch=master&event=workflow_dispatch)](https://github.com/Varinomics/vnm_logonomic_cli_kimi/actions/workflows/ci.yml)

This standalone C++20 package supplies Kimi launcher discovery, account selection,
interactive launch and exact resume, delegated ACP implementation sessions, quota,
historical token usage, hook delivery, and continuous Agent lifecycle observation.

When Logonomic requests session guidance, the hook saves the complete UTF-8
instructions under the selected Kimi home's `scratch/logonomic` directory and
returns a short file-reading reminder. The complete title, summary, worktree
and coordination instructions remain available without an unsolicited terminal
dump. Kimi's default agent can use native `Read`; a custom agent needs an
available file-reading tool. If its tool policy prevents reading files, the
optional guidance is unavailable and the ordinary session continues.
The file is scoped to the provider session and package generation, replaced on
subsequent guidance requests, and removed at `SessionEnd`. Resumed sessions use
the same hook route. This does not change tool approval rules or the user's
agent instructions.

It uses the Logonomic CLI add-on SDK, the `vnm_filesystem_observation` library, QtCore, platform APIs and toml++ for configuration parsing.

The normal launch requests a host-owned transformation of the selected Kimi
`config.toml`. Only Logonomic's marked hook blocks are replaced. The host owns
path resolution, symlink preservation, conflict detection, and atomic replacement;
the adapter transforms snapshot bytes without writing the user's configuration.
Sibling account selection uses the host's `account-profile-managed-configuration@1`
binding: the admitted account selects one frozen native home for preparation,
configuration and launch. A host that cannot bind a different selected profile
receives an explicit account mismatch before any transform is requested.

Configuration edits use toml++ source regions and validate both documents.
User string contents and comments remain byte-preserved; modified marked tables
with extra fields are refused. The header-only dependency tracks its upstream
`master` branch and supports the `KIMI_TOMLPLUSPLUS_SOURCE` local source override.
Its MIT license accompanies the packaged executables as
`notices/tomlplusplus-LICENSE.txt`, copied from the actual dependency checkout;
`package/provenance/tomlplusplus.json` records the source and incorporation.

The Kimi icons derive from Lobe Icons. Their unchanged MIT notice is in
`package/notices/lobe_icons_mit.txt`; `package/provenance/lobe_icons.json`
records the upstream source, inherited fill variants and viewport normalization.

Usage scans read each Agent wire log one line at a time, so only a single line
is bounded, by the SDK parser's document limit. An incremental scan reuses a
log's cached usage while its size and modification time are unchanged; equal
values retain the existing fingerprint contract. Repair rereads surviving logs.
Native rewrites must reproduce an accepted byte prefix before that prefix can
authorize a numeric correction; a changed legacy file without retained prefix
proof first establishes its current raw accounting through ordinary acceptance.

On `usage-scan@3`, `usage-temporal@1` preserves positive increments from native
integer epoch-millisecond timestamps before Agent files are reduced to a
conversation. The UTC representation retains every native millisecond; the
consumer chooses its reporting timezone. Unsupported timestamps contribute
unavailable temporal tokens, and no conversation completion date is used to
distribute earlier usage. Optional proof is bounded independently of accounting
history and stored in a versioned per-file carrier whose digest binds the
original accounting row. Missing, unknown or damaged carriers leave cached
accounting usable, including history whose original logs have been deleted.

Fork accounting follows native session ancestry and ordered usage occurrences
through full and turn-specific copies. Only a proved inherited prefix is
excluded; original reverted spend, unrelated identical occurrences, and the
fork's own later usage remain counted. The cache retains the event sequence and
its integrity binding so an accepted ancestor can still prove ownership after
its native files disappear. A fork with no own spend remains an explicit zero
record.

`usage-owned-event-correction@1` repairs previously accepted copied usage against
the host's exact predecessor and selected cache. It removes only proved copies
whose repository and temporal components can be mapped, then admits ordinary
growth. Each accepted file keeps its raw, owned, or unresolved accounting mode
on later scans; rejected and cancelled candidates cannot change that mode.
An unchanged legacy fingerprint additionally requires exact reproduced numeric,
category, coverage, and recognized temporal accounting before correction.

Missing or ambiguous ancestry cannot establish ownership. Existing unresolved
rows retain accepted accounting and can admit proved later append growth; a
fresh unresolved billable row makes the scan unavailable. Legacy markerless
agents in forked sessions remain subject to that limit. An optional first native
metadata `creator_session_id` matching the verified containing session can prove
a newly created wire; this package does not install a native producer that emits
that witness.

Credentials and session history stay in the selected Kimi profile. Hooks invoke
the package helper through the host-provided helpers directory.

Continuous observation binds the exact profile and native session from an
authenticated SessionStart. The host's transition nonce correlates observation
and polling. `lifecycle-correlation-lifetime@1` reserves that nonce before
classification and retires it when the host abandons the chain. Ordinary watch
windows preserve unread pages; the final closed page remains replayable until
the host acknowledges its exact consumption cursor.
Agent stops are delivered before the final SessionEnd, including
background completion and file replay. Child observation requires persisted native events.
Kimi Code 0.39.1 recorded direct `Agent` calls in `wire.jsonl` and kept
`AgentSwarm` child lifecycles internal, so those children had no timeline entries.
Ambiguous session paths supply no new native source binding.

The SDK's shared filesystem lifecycle driver owns reservations, cursors, queues
and subscriptions. Native wire discovery, interaction frontiers and the final
captured-wire drain remain Kimi-specific. Incomplete discovery or reads retain a
service-paced retry without failing the root observation channel. Existing
admitted sources can still publish valid evidence while optional inventory is
unavailable. At the host's 128-live-child or 128-live-attention bounds, new
admissions are refused locally. The bounded queue reserves space for admitted
child stops, identified attention resolutions and the final root SessionEnd.

Delegated execution requires the native ACP contract and a supported observed
version (at least 0.39.1); future releases are not rejected by an upper version
ceiling. `delegated-model-selection@1` applies a task's model and native reasoning
effort through ACP `session/set_config_option` (`model` and `thinking`). A saved
launcher model remains effective when the task does not override it. The task
waits for exact native acknowledgments before permission mode and Task-server
admission; unsupported values fail before the prompt. These session settings
preserve the selected account and saved defaults. Interactive resume additionally verifies native `--session [id]` support
and the installed SessionStart hook. The quota helper reads managed usage over
HTTPS when the selected official Kimi endpoint and file credential are known
and native token renewal is not due. Kimi remains the sole owner of credential
renewal and storage. Proxy/TLS overrides, unsupported configuration, and failed
direct requests use the native route within the same supervised operation.
That route starts an owned loopback server, accepts only that process's registry
entry, and shuts it down after the
request. Authentication absence remains distinct from a failed quota request.

## Building and qualification

The [contributor guide](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/CONTRIBUTING.md) covers local package checks.
Send provider pull requests to [the kimi repository](https://github.com/Varinomics/vnm_logonomic_cli_kimi/pulls).
See the [repository license](LICENSE) for the terms covering Varinomics code.

CMake fetches the SDK from `vnm_logonomic_cli_addon` on `master` by default. An explicit
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` path overrides that dependency for coordinated
local development. Observation reuses the embedding build's
`vnm_framework::vnm_filesystem_observation` target. A standalone build accepts
`KIMI_OBSERVATION_SOURCE`, otherwise uses the installed `vnm_filesystem_observation`
package or fetches the [filesystem observation library](https://github.com/Varinomics/vnm_filesystem_observation)
on `master`. The library selects its
`vnm_qt_dispatch` dependency; source overrides use
`FETCHCONTENT_SOURCE_DIR_VNM_QT_DISPATCH` and `FETCHCONTENT_SOURCE_DIR_VNM_CMAKE`.
The filesystem observer requires Qt Core. Outside Windows, the quota probe also
requires libcurl development files.

The `kimi_package` target produces `package/logonomic.cli.kimi` beneath a
single-configuration build directory, or `package/<config>/logonomic.cli.kimi`
with a multi-configuration generator, including its integrity index.
For matching Qt notices, additional runtime notices and Linux deployment inputs,
use `KIMI_QTBASE_SOURCE`, `KIMI_RUNTIME_NOTICES_DIRECTORY`,
`KIMI_LINUX_RUNTIME_MATERIAL` and `KIMI_PATCHELF_EXECUTABLE` as described in
[QtCore package assembly](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/sdk/logonomic_cli_addon_sdk/docs/package_contract.md#qtcore-package-assembly). The provider's CI preparation script delegates to that recipe.

CTest registers focused contract checks,
package validation, and SDK conformance. Implementation fixtures cover ACP
admission, hook ownership, Agent correlation/replay, Unicode labels, and quota
normalization. Qualification is a separate step; source availability does not
establish a successful build or test run.

Use `cmake --install` to export the indexed package to a fresh directory. See [SDK package export](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/sdk/logonomic_cli_addon_sdk/docs/package_contract.md#exporting-a-package).

## Structured and delegated sessions

The paired structured-session and delegated-session contracts use version 3.
An authenticated ACP session/close reply completes inspection and returns the
answer. This is not evidence of clean native process exit.

Delegated preparation configures each Task server in ACP `session/new` as the
host relay with `--descriptor <path> --server <id>` and no environment, and
acknowledges connection version 1; a request without a connection is refused as
`task_server_connection_unavailable`. The adapter sends Kimi's ACP builtin
`/mcp`, which reports each MCP server's status and enabled tool count without a
model turn. It is sent only after Kimi's advertised command list names it;
otherwise the text would reach the model, and the task fails as
`task_server_status_unavailable`. Kimi connects session MCP servers in the
background and announces no change over ACP, so a pending server is queried
again every 100 ms until Kimi's own startup timeout settles it. The task prompt
is released only when every Task server is reported exactly once as connected
over stdio with as many tools as it has required actions. The host relay limits
the catalog to those actions, and preparation refuses actions that Kimi would
register under one tool name (`required_control_name_collision`), so equal
counts mean every one was registered. Any other report fails the task as
`required_control_unavailable`.

Delegated runs retain the launcher's permission posture and user configuration.
The package approves a permission request once only when Kimi names one of this
Task's own tools; every other request receives its reject-once option. It never
selects a persistent approval. Kimi ACP ignores top-level permission flags, so
an explicit launcher `--yolo`/`-y` selects ACP `auto` (Ask When Needed), `--auto`
selects ACP `yolo` (Never Ask), and `--plan` selects ACP `plan`. Without those
flags, the adapter leaves the session's mode alone. File edits and commands
need permission from the launcher or the user's own Kimi configuration.

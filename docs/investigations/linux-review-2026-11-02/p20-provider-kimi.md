# P20: Kimi resume, readers, and quota cleanup

## Outcome

Finding 46 has a causal production correction: resume accepts the previous
three-hook configuration when its required SessionStart hook remains intact.
The unchanged production code failed the new preparation fixture with
`session_start_hook_unavailable`; the corrected candidate passed preparation,
configuration contract, observer, and permission tests. Findings 68 and 69 have
executed component reproductions and remain open reader limitations. Finding
138 has a source-confirmed Windows cleanup delay and a concrete native follow-up
fixture; its production path is unchanged.

Work is in [draft PR 9](https://github.com/Varinomics/vnm_logonomic_cli_kimi/pull/9),
branch `work/linux-review-20261102-p20-provider-kimi`, based on task-start
`master` commit `567363dd2450deb3265141a6be485f379ed99d86`. The canonical checkout
remains on `master`. This report supersedes the initial source-only checkpoint.
Independent code/test review found no blocking defect and accepted the bounded
reader investigation disposition, with the next-step qualifications below.
Independent review of this updated report also found no blocking documentation
or evidence defect.

| Finding | Evidence and disposition | Remaining work |
|---|---|---|
| 46 | Exact old-profile failure reproduced before the fix; candidate passes. Only the required start hook governs resume admission. | None within this scoped correction. |
| 68 | Valid irrelevant records fail the real interaction reader; inventory growth to 257 after admission fails shared observation. No reader production change. | Provider-local selective-reader work and truthful handling of attention coverage loss after admission; native compatibility evidence. |
| 69 | Valid irrelevant records fail a new usage scan; the previous accepted total remains intact. No usage production change. | A safe selective reader preserving ownership and accounting proofs; deterministic Linux fixtures can continue. |
| 138 | Owned-child destructor waits up to one second after Windows `terminate()` before killing a live child. No Windows execution or quota production change. | Execute the native fixture below, then assess the minimal Windows-only correction. |

## Resume correction and proof

Previously, [`has_start_hook()`](../../../src/kimi_config.cpp) compared the
configuration with the full `updated_config()` result. That installer now adds
five hooks, so absence of either permission hook incorrectly vetoed an intact
SessionStart route. [`prepare_resume()`](../../../src/kimi_preparation.cpp)
requires that route after launcher and account validation; resume does not
install or repair the profile.

The shared transformation now accepts an internal SessionStart-only selection.
The public installer still transforms all five hooks. Resume uses the existing
parsed TOML, managed-marker ownership, exact helper command, timeout, duplicate,
and modified-table rules for SessionStart alone. Unrelated permission hooks no
longer determine this predicate. No profile write was added.

The [preparation regression](../../../tests/kimi_preparation_tests.cpp) freezes
three-hook syntax from
[the producer before permission hooks were added](https://github.com/Varinomics/vnm_logonomic_cli_kimi/blob/7bc7f64b245a57d9f394ec36262b722c66300ab9/src/kimi_config.cpp).
It does not generate that fixture through the current installer. On unchanged
production it failed with `session_start_hook_unavailable` (CTest exit 8).
After the correction, the same resume path passed and verified byte-for-byte
profile preservation. Candidate checks also cover missing, wrong-command,
wrong-timeout, duplicate, and modified SessionStart hooks; a marker inside a
string; CRLF and Unicode text; and independently modified permission hooks.
The existing full-installer contract suite passed.

## Reader failures, native relevance, and the preserved contract

[`Interaction_reader::acquire()`](../../../src/kimi_interactions.cpp) enforces a
4 MiB aggregate pending-record bound and parses each complete record before
`accept()` selects its type. [`refresh_file()`](../../../src/kimi_usage.cpp)
also fully parses records before selecting native metadata, fork boundaries,
and usage events; its `read_stable_lines()` rejects oversized pending lines.
The SDK defaults include a 4 MiB document, 1 MiB decoded string, depth 32, and
1,024 members per object. The [main observation scan](../../../src/kimi_observer.cpp)
instead discards oversized lines and skips parser-rejected records, so the
interaction reader can end shared observation where the older wire path would
have continued.

The opt-in diagnostics exercise those real components with required records
before and after valid irrelevant records. A widened test-only parse confirms
the generated JSON is valid; production limits remain unchanged. The final
fixtures use `llm.tools_snapshot`, tool descriptions beyond the string/document
bound, or 40 nested arrays inside tool parameters. Type-last variants exercise
classification after a large or deep payload.

| Candidate diagnostic | Observed result |
|---|---|
| Interaction, small irrelevant tool snapshot | Complete; two observations (request and resolution). |
| Interaction, oversized irrelevant snapshot | `interaction_record_capacity_exhausted`; only the preceding request observed. |
| Interaction, deep irrelevant snapshot | `invalid_interaction_wire_record`; only the preceding request observed. |
| Usage, small irrelevant snapshot between usage values 7 and 11 | Success, total 18. |
| Usage, string bound; document bound; document bound with type last; depth bound with type last | Each returns `usage_record_invalid`; previously accepted total 7 retained. |
| Observer, inventory grows to 257 after admission | `wire_observation_failed`; retirement and correlation-release assertions pass. |

All three diagnostic commands exited 0, meaning the investigation completed.
They report the actual outcome and permit a future correct success; they do
not make the undesirable failure a permanent correctness expectation. The
earlier immutable baseline used generated `content.part` usage records and
other irrelevant interaction shapes. It showed the same errors. The final
native-shaped records above were separately executed on the candidate.

The inspected native Kimi
[tool-snapshot schema](https://github.com/MoonshotAI/kimi-code/blob/main/packages/agent-core-v2/src/agent/loop/llmRequestOps.ts)
declares tool descriptions as strings and parameters as arbitrary record
values, without an explicit size or nested-value bound in that declaration.
The [wire service](https://github.com/MoonshotAI/kimi-code/blob/main/packages/agent-core-v2/src/wire/wireService.ts)
and [append-store serializer](https://github.com/MoonshotAI/kimi-code/blob/main/packages/agent-core-v2/src/persistence/backends/node-fs/appendLogStore.ts)
persist records as `JSON.stringify(record)` followed by a newline. Inspected
main tree: `a2afdbc7d1fa0737d45d5780cc6c2bd38e477b55` (a tree ID, not a commit).
The corresponding schema, service, and writer blob IDs were
`122fe03e9c7a6c574410c1c8509ca0c79be1a10b`,
`9fa50f29b22dcecc17f353101921d0b167242ab8`, and
`32409812a0713982b4943890394e4a16c2431848`.
This is relevant source-permitted shape evidence. No installed Kimi instance
was observed emitting an extreme record or a 257-entry inventory in this task.
That limits prevalence evidence; it is not a prerequisite to recognizing the
component failures or continuing deterministic Linux work.

Inventory loss has a separate boundary. The
[`SessionStart` helper](../../../src/kimi_helper.cpp) already catches optional
frontier-capture failure and publishes a null interaction frontier, preserving
initial admission. Existing permission fixtures cover an inventory above 256
there. Growth after admission is discovered inside
[`scan()` / `update_observation()`](../../../src/kimi_observer.cpp) and fails
the shared continuous observation. That capability has one outcome for both
subagent and attention events. Silently abandoning an admitted interaction
reader could leave accepted pending attention without a truthful loss signal.
The diagnostic confirms explicit loss and retirement; it does not establish
that the current capacity bound is the desired product behavior.

A remedy must preserve malformed consumed-record failures, strict ownership
and numeric checks, and rewritten-history refusal. Usage also needs the first
metadata ownership witness, fork lineage, canonical event proof, every raw
byte in prefix hashes, full line offsets, and first-record position even when
a record is ignored. The same line reader serves strict host predecessor
streams; native filtering must not weaken those streams. The permission suite
now explicitly checks malformed required records, duplicate top-level types,
and same-size history replacement.

No existing small bounded projection helper preserving these rules was
identified. Raising full-DOM limits moves the boundary. Substring or prefix
classification cannot handle escaped field names and later duplicate types.
The [Qt 6.12 JSON parser](https://raw.githubusercontent.com/qt/qtbase/v6.12.0/src/corelib/serialization/qjsonparser.cpp)
retains the last duplicate key, so substituting its permissive DOM parsing
would change the strict envelope semantics. A provider-local streaming or
projection reader is a credible next implementation. It needs compatible
lexical, structural, duplicate, and Unicode handling, bounded retention, and
type-last support. Usage already has a raw-byte callback that can preserve
full/prefix hashing; no cache schema change or broad SDK/host redesign has been
shown necessary. Independent review accepted investigation as this PR's bounded
outcome, while keeping both defects unresolved and inventory coverage loss a
separate design question. The disposition rests on the uncompleted safe-reader
design and proof work, not solely on absence of an installed extreme log.

## Executed Linux validation

| Check | Actual result |
|---|---|
| Baseline preparation, unchanged production plus old-profile regression | Expected failure: `session_start_hook_unavailable`, CTest exit 8, 0.03 s. |
| Baseline normal observer / permission | Both passed, 2.37 s / 25.91 s; strict malformed, duplicate, and rewrite cases included. |
| Baseline packaged usage-cache `owned` / `observation` | Both passed, 4.84 s / 3.61 s. |
| Baseline Linux quota startup | Passed, 10.06 s; inert-child Linux behavior only. |
| Candidate preparation / full configuration contract | Both passed, 0.03 s / 0.56 s. |
| Candidate observer / permission | Both passed, 2.80 s / 25.72 s. |
| Candidate opt-in reader / inventory diagnostics | Completed with the outcomes above; defects remain reproduced. |
| Source and build selection | Both immutable snapshots verified; candidate 93 files and 824 dependency files unchanged, no code/assets difference from the final worktree. Compiler launcher verified in `build.ninja`. |

Baseline normal CTest selection: 5/5 passed in 46.79 s. Candidate selection:
4/4 passed in 29.12 s. Usage-cache and quota baseline results retain their
original snapshot attribution; those production paths were unchanged and were
not rerun on the candidate. This was focused component and packaged-adapter
validation, without an authenticated account, native Kimi campaign, Windows
runtime, full integration suite, or private hosted CI run.

The toolchain was Ubuntu 24.04.3, glibc 2.39, GCC/G++ 13.3.0, vendor Qt 6.12.0,
CMake 3.31.10, Ninja 1.13.2, and ccache 4.9.1, in Release mode. SDK
`logonomic-linux-development-sdk-2026.10.23.1` was reused from the preceding
workspace and its extracted regular payload hashes were independently checked;
the archive was absent, so this task did not repeat an archive hash check.
SDK preflight passed. Missing optional SQL drivers and Qt Quick 3D spatial
audio did not affect these Qt Core fixtures and provide no native capability
evidence.

Current-at-task-start source overrides were built instead of reusing owned
product binaries. These hashes describe the historical inputs; they do not
establish future dependency pins or approvals.

| Source | Selected revision |
|---|---|
| `Varinomics/logonomic` SDK | `958b4041ff64744656d5b8595e4c34150514ecaf` |
| `Varinomics/vnm_framework` observation leaf | `e2f76f52bbda75bc2510fa07961546834b7db235` |
| `Varinomics/vnm_qt_dispatch` | `666f6d054b4cf79b39a240370eeb7053d2a07ee3` |
| `Varinomics/vnm_cmake` | `66d5ee14f4f69c952daa58a95a4759ad77a636f4` |
| `marzer/tomlplusplus` | `3c71044e1e1bcfc50391d0d93e63609295ad3e18` |
| Matching Qt base notice source | `025bdad181de241e81bf853c8a2d7bf3d19261a9` |

Compiler-probing configuration reserved one shared queue slot; builds reserved
four slots with `--parallel 4`. Tests ran serially through `sdk-run-headless`.
The retained compiler cache was effective: baseline added 109 cacheable calls
(12 hits, 97 misses); candidate added 108 (82 hits, 26 misses). The configured
provider paths were the immutable `repro-source` and `candidate-source`, not a
mutable checkout or an older product build.

### Reproducing the focused checks

Use a separate source snapshot and build directory. Set the paths below to the
PR source, an activated Linux development SDK, and source checkouts arranged
under the named dependency directories. `qtbase-notices` must contain the
selected Qt version's `.cmake.conf`, `LICENSES`, and third-party notices.
`kimi_notices` must contain the resolved non-Qt runtime notices named
`<library filename>.txt`, as required by
[`kimi_package_runtime.cmake`](../../../cmake/kimi_package_runtime.cmake).
This run used SDK/system package copyright files and the
[ICU 73.2 license](https://github.com/unicode-org/icu/blob/release-73-2/icu4c/LICENSE)
for the matching ICU libraries. Ubuntu libcurl development headers/library
were at the paths supplied below. These are standalone commands; the task's
temporary configure script is not required.

```bash
kimi_source=/absolute/path/to/p20-source
kimi_build=/absolute/path/to/p20-build
kimi_sdk=/absolute/path/to/logonomic-linux-development-sdk
kimi_deps=/absolute/path/to/source-checkouts
kimi_notices=/absolute/path/to/runtime-notices
kimi_cache=/absolute/path/to/persistent-compiler-cache
source "$kimi_sdk/activate.sh"
export PATH="$kimi_sdk/tools/cmake/bin:$kimi_sdk/tools/ninja:$PATH"
export XDG_CACHE_HOME=/tmp/varinomics-qt-sdk-host-cache
export CCACHE_DIR="$kimi_cache"
export CMAKE_C_COMPILER_LAUNCHER="$kimi_sdk/bin/ccache"
export CMAKE_CXX_COMPILER_LAUNCHER="$kimi_sdk/bin/ccache"

queued-build --slots 1 -- cmake -S "$kimi_source" -B "$kimi_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_CXX_COMPILER_LAUNCHER="$kimi_sdk/bin/ccache" \
  -DCMAKE_PREFIX_PATH="$kimi_sdk/qt;$kimi_sdk/support/usr" \
  -DLOGONOMIC_CLI_ADDON_SDK_SOURCE="$kimi_deps/logonomic/sdk/logonomic_cli_addon_sdk" \
  -DKIMI_TOMLPLUSPLUS_SOURCE="$kimi_deps/tomlplusplus" \
  -DKIMI_FRAMEWORK_SOURCE="$kimi_deps/vnm_framework" \
  -DVNM_QT_DISPATCH_SOURCE_DIR="$kimi_deps/vnm_qt_dispatch" \
  -DFETCHCONTENT_SOURCE_DIR_VNM_CMAKE="$kimi_deps/vnm_cmake" \
  -DKIMI_QTBASE_SOURCE="$kimi_deps/qtbase-notices" \
  -DKIMI_RUNTIME_NOTICES_DIRECTORY="$kimi_notices" \
  -DKIMI_PATCHELF_EXECUTABLE="$kimi_sdk/support/usr/bin/patchelf" \
  -DCURL_LIBRARY=/usr/lib/x86_64-linux-gnu/libcurl.so \
  -DCURL_INCLUDE_DIR=/usr/include/x86_64-linux-gnu \
  -DVNM_FILESYSTEM_OBSERVATION_BUILD_TESTS=OFF \
  -DVNM_FILESYSTEM_OBSERVATION_ENABLE_TEST_HOOKS=OFF \
  -DVNM_QT_DISPATCH_BUILD_TESTING=OFF

queued-build --slots 4 -- cmake --build "$kimi_build" --parallel 4 --target \
  kimi_preparation_tests kimi_contract_tests kimi_permission_tests \
  kimi_observer_tests kimi_usage_tests
sdk-run-headless ctest --test-dir "$kimi_build" --output-on-failure \
  --no-tests=error -V -j 1 \
  -R '^kimi_(preparation_tests|contract_tests|observer_tests|permission_tests)$'
sdk-run-headless "$kimi_build/kimi_permission_tests" --investigate-records
sdk-run-headless "$kimi_build/kimi_observer_tests" --investigate-inventory
sdk-run-headless "$kimi_build/kimi_usage_tests" --wire-envelope-probe
"$kimi_sdk/bin/ccache" --show-stats
```

For the fail-first comparison, use a separate copy containing the regression
tests but `src/kimi_config.cpp` from
`567363dd2450deb3265141a6be485f379ed99d86`, configure/build it as above, and run
`sdk-run-headless ctest --test-dir "$kimi_build" --output-on-failure --no-tests=error -V -R '^kimi_preparation_tests$'`.
The expected baseline output is `session_start_hook_unavailable`. Keep the
candidate source intact for the passing comparison.

The additional baseline gates can be reproduced in that baseline build with:

```bash
queued-build --slots 4 -- cmake --build "$kimi_build" --parallel 4 --target \
  kimi_usage_cache_tests kimi_quota_startup_tests
sdk-run-headless ctest --test-dir "$kimi_build" --output-on-failure \
  --no-tests=error --timeout 180 -V -j 1 \
  -R '^kimi_(observer_tests|permission_tests|usage_cache_tests_(owned|observation)|quota_startup_tests)$'
```

Supplemental raw execution evidence remains in the task workspace under
`gate-logs/validation-summary.json`, `repro-*.{log,json}`, and
`candidate-*.{log,json}`. Setup source inventories and SDK provenance are under
`setup/`. The result tables and commands above carry the durable handoff; these
temporary paths are not prerequisites for reproducing it.

## Windows quota follow-up and remaining decision

[`Owned_server::~Owned_server()`](../../../src/kimi_quota_probe.cpp) calls
`terminate()`, waits 1,000 ms, then kills and reaps a still-running owned child.
Qt documents that Windows
[`terminate()`](https://doc.qt.io/qt-6/qprocess.html#terminate) sends `WM_CLOSE`;
a console process without an appropriate event loop/handler requires
[`kill()`](https://doc.qt.io/qt-6/qprocess.html#kill), which uses
`TerminateProcess`. The extra wait is conditional on the child remaining
alive, including after successful HTTP cleanup. The Linux readiness-timeout
fixture does not isolate that delay or prove Windows child reaping.

Extend [`kimi_quota_startup_tests`](../../../tests/kimi_quota_startup_tests.cpp)
on Windows with an owned inert console child launched through the existing
`web` path. Have it publish its own matching server-registry entry with a
nonzero loopback port, omit `server.token`, and remain alive. The missing token
fails the helper before HTTP cleanup, isolating the destructor without the
10-second readiness timeout or three-second HTTP shutdown wait. Retain an OS
handle to that exact child occurrence, measure helper completion, and require
the handle to be signaled when the probe returns. Record the original failure
reason as well as timing and absence of a survivor.

After native reproduction, the minimal candidate is Windows-only
`m_process.kill(); m_process.waitForFinished(-1);` in the owned-child destructor,
preserving the POSIX terminate/grace/fallback path and existing QProcess child
ownership. No PID-based killing or general polling policy is needed.

The next reader decision is a provider-local selective parser meeting the
strict consumed-record/provenance contract, plus truthful handling of attention
coverage loss after admission. Deterministic Linux fixtures can establish that
behavior without waiting for a native extreme log. A native tool snapshot
beyond a provider bound, with version, raw wire bytes, adjacent required events,
and retained-cache evidence, would strengthen compatibility proof. Those open
items and native Windows execution remain follow-up work; the executed Linux
checks establish the scoped resume correction and reader failures only.

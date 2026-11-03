# Kimi reader isolation: reproduced checkpoint

## Outcome at this checkpoint

The irrelevant-record failures from [PR #9](https://github.com/Varinomics/vnm_logonomic_cli_kimi/pull/9)
are reproduced on unchanged provider production source, with newly added C++
regressions and refreshed owned dependencies. An oversized irrelevant record
raises `interaction_record_capacity_exhausted` in the interaction reader and
ends the actual shared observer with `wire_observation_failed`. Irrelevant
usage records beyond the string, document or depth bound raise
`usage_record_invalid`; the previously accepted seven-token cache remains
unchanged. The small usage control succeeds with eighteen tokens.

This is an early reproduced checkpoint. It adds reader fixtures and this
report; it does not contain a production reader correction or establish that
the new success assertions pass. The new regressions assert the required
future behavior, rather than making the existing failures permanent expected
successes. The diagnostic usage probe intentionally returns zero after
reporting each outcome; its exit status is not a reader-correctness verdict.

The work is confined to provider-local interaction and usage reading. Resume
configuration, the separate 257-agent coverage issue, and Windows quota cleanup
remain outside this PR. No shared SDK, cache-schema or host-policy change is
included in this checkpoint.

## Fresh executed evidence

The executed baseline remains the historical `567363dd2450deb3265141a6be485f379ed99d86`
snapshot. Before this checkpoint was published, master advanced to
`d135e0c` by merging PR #9. Publication therefore uses that newer parent; its
interaction and usage production readers are byte-identical to the executed
historical base. The published test files preserve the upstream PR #9
diagnostics alongside these fail-first fixtures. That mechanical test
reconciliation has not been executed at this checkpoint and will be validated
with the follow-up candidate; it does not replace the historical execution
evidence below. The upstream resume configuration and inventory diagnostic
remain inherited parent content, outside this reader-only change.

All commands below used the immutable `baseline-source` snapshot, actual C++
provider sources and their normal package assembler. The production source was
`master` commit `567363dd2450deb3265141a6be485f379ed99d86`; only
`tests/kimi_permission_tests.cpp`, `tests/kimi_observer_tests.cpp` and
`tests/kimi_usage_tests.cpp` differed from that tree. The 92 provider files and
633 selected dependency blobs were verified before and after test execution.
The provider snapshot and compiled dependency inputs remained unchanged. One
unused SDK test-manifest fixture was restored during setup; SDK tests were
disabled and that file was not a build input.

| Executed check | Result | Elapsed time |
|---|---|---:|
| Release build of permission, observer, usage and usage-cache targets | Passed; real provider package assembled with 85 indexed files. | Not used as performance evidence. |
| `kimi_permission_tests --reader-isolation` | Exit 1: `interaction_record_capacity_exhausted`. | 0.122 s |
| `kimi_observer_tests --reader-isolation` | Exit 1: `reader isolation ended observation: wire_observation_failed`. | 0.271 s |
| `kimi_usage_tests --wire-envelope-probe` | Exit 0 after reporting the outcomes below; accepted accounting retained on each failed scan. | 0.328 s |
| `kimi_usage_tests --reader-records` | Exit 1: `usage_record_invalid`. | 0.177 s |
| `kimi_usage_cache_tests_owned` | Passed through the packaged adapter. | 9.14 s |
| `kimi_usage_cache_tests_observation` | Passed through the packaged adapter. | 5.35 s |

The two normal usage-cache CTests passed in 14.49 seconds total. The new
permission and observer regressions are also called by their ordinary test
entry points, so those ordinary registrations were not run again merely to
repeat the same intentional baseline failures. Later assertions inside the
failing modes are not credited as passing baseline coverage.

The usage probe printed:

```text
small-irrelevant: success, total_tokens=18
string-bound: usage_record_invalid, accepted_total_tokens=7 retained
document-bound: usage_record_invalid, accepted_total_tokens=7 retained
document-bound-type-last: usage_record_invalid, accepted_total_tokens=7 retained
depth-bound-type-last: usage_record_invalid, accepted_total_tokens=7 retained
```

The `owned` cache phase additionally passed its existing child-event,
zero-token correction, rejected-replay and ordinary-reparse assertions. The
`observation` phase reported `unavailable/usage_observation_unsettled` during
state replacement, then a settled total of 150 in the same generation and
process. These are preservation controls on unchanged production, not proof
that irrelevant-record isolation is implemented.

## What the new fixtures exercise

The [interaction fixture](../../tests/kimi_permission_tests.cpp) calls the
real `Interaction_reader`. It interleaves main-agent and child-agent requests
and resolutions with a tool snapshot larger than 4 MiB, keeps that record
unfinished across acquisitions, splits an escaped type discriminator, and
places the discriminator after its payload. A deeply nested irrelevant record
follows it. Success assertions require another agent's wire to progress while
the large record is unfinished, exact request/resolution order, no replay on
reacquisition, correct handling of escaped consumed types, and refusal of a
same-size rewrite inside the raw 4096-byte history anchor. Separate strict
cases include malformed required JSON, duplicate discriminators, nested
duplicates, invalid Unicode and numbers, and oversized/deep consumed records.

The [observer fixture](../../tests/kimi_observer_tests.cpp) uses the maintained
`Lifecycle_observer`, filesystem observation service and actual file writes.
It establishes an admitted interaction frontier, receives a request, appends
an irrelevant snapshot across writes, and expects the following resolution
without ending shared observation. Its baseline failure is observed through
that real receiver path, rather than inferred solely from a parser unit test.

The [usage fixtures](../../tests/kimi_usage_tests.cpp) call the actual
`scan_usage` implementation through the existing `Selected_cache`
fixture. They start from accepted accounting, place irrelevant records between
usage events, and exercise escaped/type-last discriminators across the reader's
1 MiB physical chunks. Assertions require the original physical size and raw
prefix SHA-256, including leading NUL bytes and an unterminated tail, while
only complete consumed events contribute accounting. Required-record and
first-record cases are defined separately. These assertions are candidate
acceptance requirements; the baseline `--reader-records` failure prevents
crediting all of them as executed successes at this checkpoint.

The constructed tool snapshots use the native `llm.tools_snapshot` shape
identified in PR #9. No installed Kimi process was observed emitting these
extremes. The evidence establishes component behavior with generated
records; it does not establish how often a native installation produces them.

## Source and toolchain attribution

The provider base is the same production reader source reviewed in PR #9.
This build resolves the owned dependency source tuple afresh rather than
reusing that PR's earlier product binaries or crediting its test results.
The hashes below describe the inputs to this execution, not future dependency
pins or an approval requirement.

| Source input | Revision |
|---|---|
| Kimi provider production base | `567363dd2450deb3265141a6be485f379ed99d86` |
| Logonomic add-on SDK source | `25db6999238a1cf3de907d3b844e6b6ee81dc35e` |
| Framework filesystem observation source | `75c8db7fb1cd2b01f161894e31e3188052e88dc2` |
| Qt dispatch source | `9244c66fa483830269e98dd37fc842c960253809` |
| CMake support source | `66d5ee14f4f69c952daa58a95a4759ad77a636f4` |
| tomlplusplus | `3c71044e1e1bcfc50391d0d93e63609295ad3e18` |
| Matching Qt base notice source | `025bdad181de241e81bf853c8a2d7bf3d19261a9` |

Executed historical baseline fixture SHA-256 values:

| File | SHA-256 |
|---|---|
| `tests/kimi_permission_tests.cpp` | `dd1740e6d1e421428143d51eb6f7e491e3427473bd7edad9be0aaae6f64b1629` |
| `tests/kimi_observer_tests.cpp` | `cfd7b55eddc8c0b345d675b7c26527a15b77014f1dd191a5cc194e1c3b3a1fc6` |
| `tests/kimi_usage_tests.cpp` | `a67bf856ec9bcec95d4c07cc26b1b07da03b35fbd2b77c1644a72fd675d6b1d0` |

The environment was Ubuntu 24.04.3 / glibc 2.39 / GCC 13.3, using Qt 6.12.0,
CMake 3.31.10, Ninja 1.13.2 and ccache 4.9.1. The public
`logonomic-linux-development-sdk-2026.10.23.1` archive was freshly downloaded
and its SHA-256 verified as
`56100ede7b00bd7faf06a7b64c57b94106f63e3ba1b77c0bbdf5d61d7ca220c2`.
SDK preflight passed. Optional unavailable SQL and spatial-audio capabilities
were unrelated to these Qt Core tests. Source builds used one queued slot for
configuration and four slots with matching build parallelism. The retained
compiler cache was selected in generated build commands and recorded 107 new
cacheable calls: 12 hits and 95 misses.

## Reproduction commands

Set `kimi_source` to a separate source snapshot containing these fixtures and
the unchanged production base, and supply the dependency sources above under
`kimi_deps`. Activate the verified SDK. `kimi_qt_notices` is the matching Qt
source's notice tree, and `kimi_runtime_notices` contains notices named after
the resolved external runtime libraries, as required by the repository's
normal package assembler. The executed build used matching SDK/system package
copyright files and the upstream ICU 73.2 license for the vendor ICU libraries.

```bash
source "$kimi_sdk/activate.sh"
export PATH="$kimi_sdk/tools/cmake/bin:$kimi_sdk/tools/ninja:$PATH"
export CCACHE_DIR="$kimi_cache"

queued-build --slots 1 -- cmake -S "$kimi_source" -B "$kimi_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_CXX_COMPILER_LAUNCHER="$kimi_sdk/bin/ccache" \
  -DCMAKE_PREFIX_PATH="$kimi_sdk/qt;$kimi_sdk/support/usr" \
  -DLOGONOMIC_CLI_ADDON_SDK_SOURCE="$kimi_deps/logonomic/sdk/logonomic_cli_addon_sdk" \
  -DKIMI_TOMLPLUSPLUS_SOURCE="$kimi_deps/tomlplusplus" \
  -DKIMI_FRAMEWORK_SOURCE="$kimi_deps/vnm_framework" \
  -DVNM_QT_DISPATCH_SOURCE_DIR="$kimi_deps/vnm_qt_dispatch" \
  -DFETCHCONTENT_SOURCE_DIR_VNM_CMAKE="$kimi_deps/vnm_cmake" \
  -DKIMI_QTBASE_SOURCE="$kimi_qt_notices" \
  -DKIMI_RUNTIME_NOTICES_DIRECTORY="$kimi_runtime_notices" \
  -DKIMI_PATCHELF_EXECUTABLE="$kimi_sdk/support/usr/bin/patchelf" \
  -DCURL_LIBRARY=/usr/lib/x86_64-linux-gnu/libcurl.so \
  -DCURL_INCLUDE_DIR=/usr/include/x86_64-linux-gnu \
  -DVNM_FILESYSTEM_OBSERVATION_BUILD_TESTS=OFF \
  -DVNM_FILESYSTEM_OBSERVATION_ENABLE_TEST_HOOKS=OFF \
  -DVNM_QT_DISPATCH_BUILD_TESTING=OFF

queued-build --slots 4 -- cmake --build "$kimi_build" --parallel 4 --target \
  kimi_permission_tests kimi_observer_tests kimi_usage_tests kimi_usage_cache_tests

sdk-run-headless "$kimi_build/kimi_permission_tests" --reader-isolation
sdk-run-headless "$kimi_build/kimi_observer_tests" --reader-isolation
sdk-run-headless "$kimi_build/kimi_usage_tests" --wire-envelope-probe
sdk-run-headless "$kimi_build/kimi_usage_tests" --reader-records
sdk-run-headless ctest --test-dir "$kimi_build" --output-on-failure \
  --no-tests=error -V -j 1 -R '^kimi_usage_cache_tests_(owned|observation)$'
```

Run the four executable invocations independently when reproducing the
baseline: their failure exits must not prevent later diagnostics from running.
The baseline interaction, observer and `--reader-records` failures above are
expected observations of the defect, not expected-success test registrations.

The complete source inventory and execution output are retained in the task
workspace under `gate-logs/baseline-source-inputs.json`,
`baseline-source-verification.json`, `baseline-validation-summary.json`, and
the corresponding `baseline-*.log` / `baseline-*.status.json` files. The result
tables, diagnostic output and commands here provide the durable checkpoint.

## Remaining work and evidence limits

Implement provider-local selection that can classify large/deep irrelevant
records without constructing their full DOM, including type-last and escaped
discriminators. Keep full strict validation for consumed records, preserve
first-record position even when a record is ignored, retain physical byte
offsets and raw prefix/history hashing, and retain accepted accounting on a
failed refresh. Validate the integrated candidate through these actual C++
paths, including the remaining required-record and first-record assertions.

No private hosted CI, authenticated native account campaign, live-user UI,
Windows execution or release qualification was performed. This checkpoint
does not close the reader findings or the separate inventory-coverage issue.

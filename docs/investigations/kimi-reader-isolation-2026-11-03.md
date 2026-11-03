# Kimi reader isolation: reproduction and causal refinements

## Current status

The early reproduced checkpoint was published in
[PR #10](https://github.com/Varinomics/vnm_logonomic_cli_kimi/pull/10),
[commit 537a2ab](https://github.com/Varinomics/vnm_logonomic_cli_kimi/commit/537a2ab3f1e0d14b185341073713f02407f64021).
The first integrated reader implementation subsequently passed all thirteen
selected CTests and every original usage probe. Independent review then found
an additional cross-wire buffer-pressure case, which has also been reproduced
against that exact implementation. The refinement now removes shared pending
record buffers, rereads only completed consumed records, and checks their
first-pass digest before parsing. It was published as
[implementation checkpoint 256b2ad](https://github.com/Varinomics/vnm_logonomic_cli_kimi/commit/256b2ad2b52f9b398fcfcbf94015a4fcf01d10c9)
while final gates were pending. The refined snapshot has now passed all seven
affected normal CTest registrations and all five usage-probe variants.
Independent code, fixture and execution review found no blocking defect and
confirmed closure of the reproduced cross-wire capacity case.

The sections below preserve the early evidence and identify the later inputs
and results separately. Resume configuration, 257-agent coverage and Windows
quota cleanup remain outside this reader-only work.

## Early reproduced checkpoint

The irrelevant-record failures from [PR #9](https://github.com/Varinomics/vnm_logonomic_cli_kimi/pull/9)
are reproduced on unchanged provider production source, with newly added C++
regressions and refreshed owned dependencies. An oversized irrelevant record
raises `interaction_record_capacity_exhausted` in the interaction reader and
ends the actual shared observer with `wire_observation_failed`. Irrelevant
usage records beyond the string, document or depth bound raise
`usage_record_invalid`; the previously accepted seven-token cache remains
unchanged. The small usage control succeeds with eighteen tokens.

The early checkpoint added reader fixtures and this report; it contained no
production reader correction and did not establish that the new success
assertions pass. The new regressions assert the required
future behavior, rather than making the existing failures permanent expected
successes. The diagnostic usage probe intentionally returns zero after
reporting each outcome; its exit status is not a reader-correctness verdict.

The work is confined to provider-local interaction and usage reading. Resume
configuration, the separate 257-agent coverage issue, and Windows quota cleanup
remain outside this PR. No shared SDK, cache-schema or host-policy change is
included in this checkpoint.

## Baseline executed evidence

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

## What the initial fixtures exercise

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

## First integrated candidate and the remaining cross-wire case

The first candidate introduced
[`Wire_record_envelope`](../../src/kimi_wire_record.h), a provider-local
incremental lexical/structural reader. It recognizes decoded top-level type
keys and values across chunks, including escape sequences, and continues
through the complete envelope so a later duplicate discriminator or malformed
suffix cannot disguise a consumed record. It retains one grammar state per
open container instead of a recursive DOM; scalar payloads do not grow the
scanner's retained state. Interaction and usage readers apply their own
consumption policies and still send consumed records through their existing
bounded SDK parser and semantic validation.

Usage selection consumes usage records, fork boundaries, and metadata only in
the first physical record. Every complete physical record advances the byte
position and first-record state even when ignored. Raw-byte observation occurs
before selection, preserving full-file and accepted-prefix hashes, including
leading NUL bytes, line delimiters and unterminated tails. The strict host
predecessor reader retains its separate full-record validation path.

The first candidate was frozen as `candidate-source`, with 95 provider files
and the same 633 dependency blobs. Before/after verification found no changed
inputs. Its actual Release build and package assembly succeeded. Serial CTest
execution passed **13/13 in 101.50 seconds**, using the existing registration
timeouts without overrides:

| Registration | Result | Elapsed time |
|---|---|---:|
| `kimi_observer_tests` | Passed. | 3.05 s |
| `kimi_contract_tests` | Passed. | 0.58 s |
| `kimi_permission_tests` | Passed. | 34.43 s |
| `kimi_wire_cancellation_tests` | Passed. | 0.00 s |
| `kimi_usage_tests` | Passed. | 1.61 s |
| `kimi_usage_correction_tests` | Passed. | 0.05 s |
| `kimi_usage_cache_tests_cache` | Passed. | 19.86 s |
| `kimi_usage_cache_tests_temporal` | Passed. | 6.33 s |
| `kimi_usage_cache_tests_refresh` | Passed. | 5.21 s |
| `kimi_usage_cache_tests_capacity` | Passed. | 9.70 s |
| `kimi_usage_cache_tests_owned` | Passed. | 8.32 s |
| `kimi_usage_cache_tests_observation` | Passed. | 5.83 s |
| `kimi_usage_cache_tests_reader` | Passed. | 6.50 s |

The original usage probe completed in 0.237 seconds and printed success with
`total_tokens=18` for all five variants: small, string-bound, document-bound,
document-bound/type-last, and depth-bound/type-last. The candidate build added
108 cacheable compilation calls, comprising 82 hits and 26 misses; these counts
exclude its preceding configuration probes.

The new packaged `reader` cache phase establishes more than a small control:
it accepts seven tokens across oversized/deep ignored records and an
unterminated usage tail, verifies the exact physical size and raw SHA-256,
then counts the completed tail and later append for a total of 31. Rewriting
only ignored payload bytes cannot authorize growth from a retained prefix.
Malformed usage, ambiguous duplicate types and malformed fork records retain
`usage_record_invalid` and cannot seal a replacement cache. A large irrelevant
record in the strict host predecessor stream remains unavailable. The
previously accepted cache data and manifest remain byte-for-byte unchanged,
and accepted total 31 survives later disappearance of the native wire.

### Additional failure discovered after the passing selection

The first interaction implementation still charged all unfinished records to
one shared 4 MiB retained-byte allowance. The original large-record fixture
crossed that allowance within its own record and released its retained bytes.
It therefore missed a different state: a main-agent record remains unfinished
at **4 MiB minus 100 bytes**, with its type discriminator still after the
payload, while a valid request longer than 100 bytes arrives on another agent's
wire. Shared accounting marks the unrelated required record as exhausted.

A new `interaction_incomplete_irrelevant_record_isolation()` fixture reproduces
this exact sequence. Its test translation unit was compiled with the maintained
permission target's compiler and linker arguments against the already frozen
candidate `libkimi_provider.a`; only test-source/object/output paths and the
link dependency-file destination changed. Production source and existing build
objects were untouched. `--reader-isolation` exited **1** with
`interaction_record_capacity_exhausted` in **0.088 seconds**. The test-source
and linked-library hashes, complete commands and output are retained as
`gate-logs/aggregate-repro-inputs.json`, `aggregate-repro-build.log`,
`aggregate-repro.log`, and `aggregate-repro.status.json`.

This is a reproduced defect in the first candidate, not a timeout or an
inference from the earlier baseline.

### Refined interaction implementation

The refinement retains only the per-stream envelope state, physical byte count
and digest for an unfinished record. It does not reserve a shared raw-record
buffer. Once a complete consumed record is identified, the reader enforces
the existing 4 MiB bound, rereads those exact bytes from the same open input
handle, and compares their SHA-256 with the first-pass digest before invoking
the unchanged SDK parser and interaction validation. The input cursor returns
to the physical end of the current read chunk, so rereading cannot skip or
replay interleaved records. Existing raw history anchors remain in effect.

The final permission fixture includes the exact near-limit interleaving case
and a consumed record split across acquisitions. It checks both unchanged
bytes and a same-size rewrite before the final 4096-byte anchor; the latter
must produce `interaction_wire_changed` before accepting the reread record.
These run through the normal permission registration as well as its focused
`--reader-isolation` mode. The normal registration passed on the final snapshot
as recorded below; the focused mode was not repeated merely to duplicate it.

## Final integrated snapshot and focused validation

The final compilation used the immutable `refined-source` snapshot with 96
provider files and the same 633 selected dependency blobs. Every file matched
its recorded digest before configuration and after all tests. The nine changed
build, production and test files exactly match implementation checkpoint
`256b2ad2b52f9b398fcfcbf94015a4fcf01d10c9`. The frozen report predates that
checkpoint's prose update; this later evidence update changes only the mutable
repository report and does not mutate the compilation snapshot.

Relative to the first passing candidate, executable changes comprise the
interaction reread/digest refinement, its permission fixtures, inherited
upstream PR #9 configuration, and preserved diagnostic fixtures in the normal
observer and permission registrations. The usage test entry-point adjustment
is formatting only. The scanner, usage reader, packaged cache fixtures and
usage-correction production source are unchanged from the first candidate.
The inherited PR #9 preparation test and investigation report are also present
in the final source tree. Relative to the merged upstream base, the reader-only
diff remains ten paths: nine build/code/test files listed below and this report.
Resume configuration, preparation tests, the older PR #9 report, the 257-agent
issue and Windows quota cleanup have no changes in this PR.

The final Release configure and build passed, including normal package assembly
with 85 indexed files. Compilation used the same queued slots and compiler
cache; 108 cacheable calls comprised 82 hits and 26 misses, excluding configure
probes. The seven focused CTests passed **7/7 in 54.84 seconds** with unchanged
registration timeouts and native request budgets:

| Registration | Result | Elapsed time |
|---|---|---:|
| `kimi_observer_tests` | Passed. | 2.91 s |
| `kimi_contract_tests` | Passed. | 0.56 s |
| `kimi_permission_tests` | Passed. | 35.40 s |
| `kimi_wire_cancellation_tests` | Passed. | 0.00 s |
| `kimi_usage_tests` | Passed. | 1.74 s |
| `kimi_usage_cache_tests_observation` | Passed. | 5.78 s |
| `kimi_usage_cache_tests_reader` | Passed. | 8.43 s |

The final usage probe completed in 0.279 seconds and again printed success with
`total_tokens=18` for all five variants. Its diagnostic exit code remains
separate from the asserting normal usage test. The final permission run covers
the formerly failing near-limit cross-wire case, unchanged consumed bytes
across acquisitions, and rejection of a same-size rewrite before the history
anchor. Observer, usage, and packaged reader runs repeat the actual receiver,
physical-offset/hash, strict-required-record and accepted-accounting checks on
the final package. The observation phase again reports temporary unsettled
state followed by total 150 in the same generation and process.

The remaining six registrations from the first 13/13 selection were not
repeated because their reader/accounting implementation and fixture inputs
were unchanged. Their results remain attributed to that first snapshot, not
credited as final-snapshot executions. The final selected gates cover the
interaction refinement, reconciled normal entry points and packaged consumers.

Independent review inspected the refined source, exact near-limit regression,
both consumed-record reread variants, raw CTest and probe output, and immutable
input verification. It found no blocking defect in reader isolation and
confirmed that the prior shared pending-byte interference is closed. This is
a scoped code/fixture/execution verdict; the environment limits below still
apply.

The complete frozen manifest `gate-logs/refined-source-inputs.json` has SHA-256
`b48e3796217cf26fdf3e165073a8462d1823b435260e10b1e3d3992a1ad1f6de`.
Changed build, production and test input SHA-256 values are:

| File | SHA-256 |
|---|---|
| `CMakeLists.txt` | `aca010b9a934836604333f28190c4ae92c40e8ff16339b36254a7c37364c433f` |
| `src/kimi_interactions.cpp` | `7b74c1e0a0f1ef94c4f040ccfd7baaeb0f6b0145c5bec1118a637fd2b5de486e` |
| `src/kimi_usage.cpp` | `e85bdade467b95a4b0c12ffc812dd656e9ac5dfd0b2026fad6d8cee685492a20` |
| `src/kimi_wire_record.cpp` | `5b1867c52ad65778be1604dba4a195ccd16de27a58d5fba42c9144a66e4ca745` |
| `src/kimi_wire_record.h` | `4b4276d9eef9c9d2691875fc752f6a2f305e3469af00c1760139127b57623d45` |
| `tests/kimi_observer_tests.cpp` | `c9ed7b6c9fe882114f7b5617b6847ad14c1b314e72031cb862527a64f509bc02` |
| `tests/kimi_permission_tests.cpp` | `c58abdd572972edfb7b77caa5e04319676e079a7df33cb33a7bc13a058c494c9` |
| `tests/kimi_usage_cache_tests.cpp` | `1b26d75735e3502aec9c885fdc9379850615b104790d0a36dce70a576bd9612f` |
| `tests/kimi_usage_tests.cpp` | `44e5627e6af1e90253a469569319a4ed84547badedb0d16629c529bde095543e` |

## Source and toolchain attribution

The original provider base is the same production reader source reviewed in PR #9.
This build resolves the owned dependency source tuple afresh rather than
reusing that PR's earlier product binaries or crediting its test results.
The hashes below describe the inputs to this execution, not future dependency
pins or an approval requirement.

During this task, upstream merged PR #9 as
`d135e0c`. The early reader-only checkpoint
was published on that updated base after reconciling its pre-existing
diagnostic fixtures. The upstream merge owns the resume/configuration change;
it is not a reader-PR correction. The original baseline and first candidate
results above retain their actual immutable-snapshot attribution. The final
integrated snapshot and its focused gates are recorded separately above.

| Source input | Revision |
|---|---|
| Kimi provider production base | `567363dd2450deb3265141a6be485f379ed99d86` |
| Merged upstream base of published reader PR | `d135e0c` |
| Final compiled reader implementation checkpoint | `256b2ad2b52f9b398fcfcbf94015a4fcf01d10c9` |
| Logonomic add-on SDK source | `25db6999238a1cf3de907d3b844e6b6ee81dc35e` |
| Framework filesystem observation source | `75c8db7fb1cd2b01f161894e31e3188052e88dc2` |
| Qt dispatch source | `9244c66fa483830269e98dd37fc842c960253809` |
| CMake support source | `66d5ee14f4f69c952daa58a95a4759ad77a636f4` |
| tomlplusplus | `3c71044e1e1bcfc50391d0d93e63609295ad3e18` |
| Matching Qt base notice source | `025bdad181de241e81bf853c8a2d7bf3d19261a9` |

Baseline fixture SHA-256 values:

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

For the first implementation selection, configure a separate immutable
candidate source/build pair using the same options above, then build and run:

```bash
queued-build --slots 4 -- cmake --build "$kimi_build" --parallel 4 --target \
  kimi_permission_tests kimi_observer_tests kimi_usage_tests kimi_usage_cache_tests \
  kimi_usage_correction_tests kimi_contract_tests kimi_wire_cancellation_tests
sdk-run-headless "$kimi_build/kimi_usage_tests" --wire-envelope-probe
sdk-run-headless ctest --test-dir "$kimi_build" --output-on-failure \
  --no-tests=error -V -j 1 \
  -R '^kimi_(observer_tests|permission_tests|usage_tests|usage_cache_tests_(cache|temporal|refresh|capacity|owned|observation|reader)|usage_correction_tests|contract_tests|wire_cancellation_tests)$'
```

The final refined snapshot used another separate source/build pair, the same
configure and build commands, and the same probe. Its focused CTest command was:

```bash
sdk-run-headless ctest --test-dir "$kimi_build" --output-on-failure \
  --no-tests=error -V -j 1 \
  -R '^kimi_(observer_tests|permission_tests|usage_tests|wire_cancellation_tests|contract_tests|usage_cache_tests_(reader|observation))$'
```

The task workspace retains `candidate-*.log` and `refined-*.log`, exact commands
and exit statuses in their matching `.status.json` files, per-registration
results in `candidate-ctest-results.json` and `refined-ctest-results.json`, and
the before/after source-verification JSON files. The diagnostic probe runs were
recorded independently from each CTest selection. No test timeout override was
used.

## Evidence limits

The reproduced reader failures and additional cross-wire case are closed by
the final source and executed component/package fixtures. Generated records
exercise the relevant native shapes and chunk boundaries; they are not an
observation of an installed Kimi process producing these particular extremes.

No private hosted CI, authenticated native account campaign, live-user UI,
Windows execution or release qualification was performed. The separate
inventory-coverage issue remains excluded, as do resume configuration changes
and Windows quota cleanup.

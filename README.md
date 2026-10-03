# Kimi CLI add-on for Logonomic

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

It uses the public Logonomic CLI add-on SDK, the filesystem observation leaf of
vnm_framework, QtCore, platform APIs and toml++ for configuration parsing.

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
values are taken to mean unchanged content, the check the pre-cutover scanner
used, because wire logs are append-only and repair rereads every log.

Credentials and session history stay in the selected Kimi profile. Hooks invoke
the package helper through the host-provided helpers directory.

Continuous observation binds the exact profile and native session from an
authenticated SessionStart. The host's transition nonce correlates observation
and polling. `lifecycle-correlation-lifetime@1` reserves that nonce before
classification and retires it when the host abandons the chain. Ordinary watch
windows preserve unread pages; the final closed page remains replayable until
the host acknowledges its exact consumption cursor.
Agent stops are delivered before the final SessionEnd, including
background completion and file replay. Ambiguous session paths are rejected.

Delegated execution requires the native ACP contract and a supported observed
version (at least 0.39.1); future releases are not rejected by an upper version
ceiling. Interactive resume additionally verifies native `--session [id]` support
and the installed SessionStart hook. The quota helper reads managed usage over
HTTPS when the selected official Kimi endpoint and file credential are known
and native token renewal is not due. Kimi remains the sole owner of credential
renewal and storage. Proxy/TLS overrides, unsupported configuration, and failed
direct requests use the native route within the same supervised operation.
That route starts an owned loopback server, accepts only that process's registry
entry, and shuts it down after the
request. Authentication absence remains distinct from a failed quota request.

## Building and qualification

CMake fetches the SDK from Logonomic `master` by default. An explicit
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` path overrides that dependency for coordinated
local development. Observation reuses the embedding build's
`vnm_framework::vnm_filesystem_observation` target. A standalone build accepts
`KIMI_FRAMEWORK_SOURCE`, otherwise uses the installed `vnm_filesystem_observation`
leaf package or acquires framework `master`. The leaf selects its owned
`vnm_qt_dispatch` dependency; source overrides use
`FETCHCONTENT_SOURCE_DIR_VNM_QT_DISPATCH` and `FETCHCONTENT_SOURCE_DIR_VNM_CMAKE`.
Only QtCore >=6.11.1 is required; the aggregate GUI/QML framework is not imported.
Use a fresh out-of-source tree. Windows and Linux x86-64 are
the currently packaged targets; Windows builds follow the workstation's queued
Visual Studio 2026 / MSVC v145 build policy.

The `kimi_package` target produces `package/logonomic.cli.kimi` beneath a
single-configuration build directory, or `package/<config>/logonomic.cli.kimi`
with a multi-configuration generator, including its integrity index.
The assembler inspects each executable's actual runtime closure and places its
non-system libraries beside it. Linux executables use a package-local `$ORIGIN`
loader path; staged ELF libraries are normalized with `patchelf` (override its
location with `KIMI_PATCHELF_EXECUTABLE`). Windows resolves its DLLs from the executable directory. SONAME
files are regular copies because the package contract excludes symlinks. The
package does not depend on Qt next to Logonomic or on a developer search path.
A no-op build retains the package and index; changes to dependency bytes or
membership rebuild both before the package is exported.

Runtime notices come from the selected Qt's matching qtbase source, supplied by
`KIMI_QTBASE_SOURCE` or discovered at `<Qt-prefix>/../Src/qtbase`. Otherwise the
build acquires Qt's official qtbase source at the selected runtime's version tag
for notices only. Its licenses,
bundled third-party notices and available installation SPDX metadata accompany
the actual closure. Debian-family runtime dependencies use their installed
package copyright files. Other external libraries require a notice file named
`<library-filename>.txt` in `KIMI_RUNTIME_NOTICES_DIRECTORY`; missing notices fail
assembly. The selected MSVC redistributables carry Microsoft's runtime notice.
MSVC Debug packages instead select Debug CRT and Debug UCRT from the active
toolchain and Windows SDK. Their separate notice and resolved-source provenance
identify private debugging/testing deployment only; they are not production
redistributables.
The index marks every copied runtime as third-party and points to its notices
and resolved-source provenance. The optional `kimi_package_membership` recipe
forwards these selected sources into its isolated nested build.
CTest registers focused contract checks,
package validation, and SDK conformance. Implementation fixtures cover ACP
admission, hook ownership, Agent correlation/replay, Unicode labels, and quota
normalization. Qualification is a separate step; source availability does not
establish a successful build or test run.

The package recipe proves the Windows/Linux x86-64 compiler target during
configuration. `cmake --install` exports a fresh artifact beneath its install-time
prefix and preserves executable permissions. An existing per-package destination,
including a dangling symlink, is refused. POSIX `DESTDIR` staging is supported;
nonempty `DESTDIR` with a drive-letter prefix is refused. Export is not atomic:
a failure may leave a partial artifact directory. Choose a fresh destination or
manually remove only that identified disposable export before retrying; never
remove an installed generation this way. The application's package ingestion
owns atomic installation and updates.

The application chooses activation. Replacing the built-in Kimi provider removes
that implementation; this package does not retain a native fallback.

## Structured and delegated sessions

The paired structured-session and delegated-session contracts use version 2.
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

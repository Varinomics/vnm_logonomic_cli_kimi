# Kimi CLI add-on for Logonomic

This standalone C++20 package supplies Kimi launcher discovery, account selection,
interactive launch and exact resume, delegated ACP implementation sessions, quota,
historical token usage, hook delivery, and continuous Agent lifecycle observation.
It uses the public Logonomic CLI add-on SDK, platform APIs and toml++ for
configuration parsing.

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
and the installed SessionStart hook. The quota helper starts an owned loopback
server, accepts only that process's registry entry, and shuts it down after the
request. Authentication absence remains distinct from a failed quota request.

## Building and qualification

CMake fetches the SDK from Logonomic `master` by default. An explicit
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` path overrides that dependency for coordinated
local development. Use a fresh out-of-source tree. Windows and Linux x86-64 are
the currently packaged targets; Windows builds follow the workstation's queued
Visual Studio 2026 / MSVC v145 build policy.

The `kimi_package` target produces `package/logonomic.cli.kimi` beneath a
single-configuration build directory, or `package/<config>/logonomic.cli.kimi`
with a multi-configuration generator, including its integrity index.
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

Delegated preparation refuses nonempty required control-action lists with
`required_control_admission_unavailable` before native probes or process
proposals. Because the host requires nonempty actions, this disables all valid
host delegation. Interactive launch and resume remain independent. ACP mode
acknowledgment does not establish native action admission.

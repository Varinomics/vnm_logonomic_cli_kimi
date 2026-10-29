"""Prepare Kimi's QtCore and filesystem-observation inputs for provider CI."""

import argparse
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.request


QT_VERSION = "6.12.0"
QT_HOSTS = {
    "windows-x86_64": ("windows", "win64_msvc2022_64"),
    "linux-x86_64": ("linux", "linux_gcc_64"),
    "macos-arm64": ("mac", "clang_64"),
}


def run(arguments, **options):
    subprocess.run(arguments, check=True, **options)


def dependency_sources(root, token):
    environment = os.environ.copy()
    config_index = int(environment.get("GIT_CONFIG_COUNT", "0"))
    authorization = base64.b64encode(("x-access-token:" + token).encode()).decode()
    environment["GIT_CONFIG_COUNT"] = str(config_index + 1)
    environment[f"GIT_CONFIG_KEY_{config_index}"] = "http.https://github.com/.extraheader"
    environment[f"GIT_CONFIG_VALUE_{config_index}"] = "AUTHORIZATION: basic " + authorization
    sources = {}
    for repository in ("vnm_framework", "vnm_qt_dispatch", "vnm_cmake"):
        source = root / repository
        if not source.exists():
            run(["git", "clone", "--depth", "1", "--branch", "master",
                 f"https://github.com/Varinomics/{repository}.git", str(source)], env=environment)
        else:
            run(["git", "-C", str(source), "fetch", "--depth", "1", "origin", "master"],
                env=environment)
            run(["git", "-C", str(source), "checkout", "-B", "master", "FETCH_HEAD"])
        sources[repository] = source
    return sources


def qt_prefix(root):
    candidates = list((root / QT_VERSION).glob("*/lib/cmake/Qt6Core/Qt6CoreConfig.cmake"))
    if len(candidates) != 1:
        raise RuntimeError("Expected one installed QtCore kit")
    return candidates[0].parents[3]


def prepare_qt(root, platform):
    qt_root = root / "qt"
    marker = root / f"qt-{QT_VERSION}-{platform}.ready"
    if not marker.is_file():
        tools = root / "aqt"
        # The upstream metadata fix is needed for Qt's per-architecture Windows repositories.
        run([sys.executable, "-m", "pip", "install", "--upgrade", "--target", str(tools),
             "--cache-dir", str(root / "pip-cache"),
             "git+https://github.com/miurahr/aqtinstall.git@master"])
        environment = os.environ.copy()
        environment["PYTHONPATH"] = str(tools)
        host, architecture = QT_HOSTS[platform]
        archives = ["qtbase", "icu"] if platform == "linux-x86_64" else ["qtbase"]
        run([sys.executable, "-m", "aqt", "install-qt", host, "desktop", QT_VERSION,
             architecture, "--outputdir", str(qt_root), "--archives", *archives], env=environment)
        qt_prefix(qt_root)
        marker.touch()
    prefix = qt_prefix(qt_root)
    notices = root / f"qtbase-{QT_VERSION}"
    if not notices.exists():
        run(["git", "clone", "--depth", "1", "--filter=blob:none", "--sparse",
             "--branch", f"v{QT_VERSION}", "https://github.com/qt/qtbase.git", str(notices)])
    run(["git", "-C", str(notices), "sparse-checkout", "set", "LICENSES", "src/3rdparty"])
    return prefix, notices


def prepare_icu_notices(prefix, destination):
    destination.mkdir(parents=True, exist_ok=True)
    licenses = {}
    for runtime in sorted((prefix / "lib").glob("libicu*.so*")):
        match = re.fullmatch(r"libicu\w+\.so\.(\d+)\.(\d+)", runtime.resolve().name)
        if not match:
            raise RuntimeError(f"Cannot identify ICU release for {runtime.name}")
        version = "-".join(match.groups())
        if version not in licenses:
            notice = destination / f"icu-{version}-LICENSE"
            if not notice.is_file():
                url = f"https://raw.githubusercontent.com/unicode-org/icu/release-{version}/icu4c/LICENSE"
                with urllib.request.urlopen(url, timeout=60) as response:
                    notice.write_bytes(response.read())
            licenses[version] = notice.read_bytes()
        (destination / (runtime.name + ".txt")).write_bytes(licenses[version])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=QT_HOSTS, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    token = os.environ.pop("VARINOMICS_REPO_READ_TOKEN", "")
    if not token:
        raise RuntimeError("VARINOMICS_REPO_READ_TOKEN is required to read Kimi's owned dependencies")
    root = args.workspace.resolve() / ".ci-dependencies" / "kimi"
    root.mkdir(parents=True, exist_ok=True)
    sources = dependency_sources(root, token)
    prefix, qt_notices = prepare_qt(root, args.platform)
    cmake_args = [
        f"-DCMAKE_PREFIX_PATH={prefix.as_posix()}",
        f"-DKIMI_FRAMEWORK_SOURCE={sources['vnm_framework'].as_posix()}",
        f"-DFETCHCONTENT_SOURCE_DIR_VNM_QT_DISPATCH={sources['vnm_qt_dispatch'].as_posix()}",
        f"-DFETCHCONTENT_SOURCE_DIR_VNM_CMAKE={sources['vnm_cmake'].as_posix()}",
        f"-DKIMI_QTBASE_SOURCE={qt_notices.as_posix()}",
        "-DVNM_FILESYSTEM_OBSERVATION_BUILD_TESTS=OFF",
        "-DVNM_QT_DISPATCH_BUILD_TESTING=OFF",
    ]
    if args.platform == "linux-x86_64":
        runtime_notices = root / "runtime-notices"
        prepare_icu_notices(prefix, runtime_notices)
        cmake_args.append(f"-DKIMI_RUNTIME_NOTICES_DIRECTORY={runtime_notices.as_posix()}")
    with open(os.environ["GITHUB_PATH"], "a", encoding="utf-8") as output:
        output.write(str(prefix / "bin") + "\n")
    args.output.write_text(json.dumps(cmake_args) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

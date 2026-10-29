"""Check Kimi's CI dependency preparation without downloading or compiling."""

import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location("prepare_ci", Path(__file__).with_name("prepare_ci.py"))
PREPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARE)


class Preparation_tests(unittest.TestCase):
    def test_cached_owned_sources_refresh_master_without_persisting_token(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for repository in ("vnm_framework", "vnm_qt_dispatch", "vnm_cmake"):
                (root / repository).mkdir()
            with patch.object(PREPARE, "run") as run:
                PREPARE.dependency_sources(root, "private-read-token")
            fetches = [call for call in run.call_args_list if "fetch" in call.args[0]]
            self.assertEqual(len(fetches), 3)
            for call in fetches:
                self.assertEqual(call.args[0][-2:], ["origin", "master"])
                self.assertNotIn("private-read-token", " ".join(call.args[0]))
                self.assertIn("http.https://github.com/.extraheader", call.kwargs["env"].values())

    def test_cached_qt_reuses_binaries_and_selects_matching_notice_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / "qt" / PREPARE.QT_VERSION / "kit/lib/cmake/Qt6Core/Qt6CoreConfig.cmake"
            config.parent.mkdir(parents=True)
            config.touch()
            (root / f"qt-{PREPARE.QT_VERSION}-linux-x86_64.ready").touch()
            notices = root / f"qtbase-{PREPARE.QT_VERSION}"
            notices.mkdir()
            with patch.object(PREPARE, "run") as run:
                prefix, selected_notices = PREPARE.prepare_qt(root, "linux-x86_64")
            self.assertEqual(prefix, root / "qt" / PREPARE.QT_VERSION / "kit")
            self.assertEqual(selected_notices, notices)
            self.assertEqual(len(run.call_args_list), 1)
            self.assertIn("sparse-checkout", run.call_args.args[0])

    def test_icu_notice_comes_from_the_runtime_release_and_preserves_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "lib").mkdir()
            for name in ("libicuuc.so.73.2", "libicui18n.so.73.2"):
                (root / "lib" / name).touch()
            license_bytes = b"Copyright ICU contributors\nLicense bytes: \xa9\n"
            with patch.object(PREPARE.urllib.request, "urlopen", return_value=io.BytesIO(license_bytes)) as request:
                PREPARE.prepare_icu_notices(root, root / "notices")
            self.assertEqual(request.call_count, 1)
            self.assertIn("/release-73-2/icu4c/LICENSE", request.call_args.args[0])
            for name in ("libicuuc.so.73.2", "libicui18n.so.73.2"):
                self.assertEqual((root / "notices" / (name + ".txt")).read_bytes(), license_bytes)

    def test_callback_emits_source_overrides_and_qt_runtime_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "arguments.json"
            github_path = root / "github-path"
            sources = {name: root / name for name in ("vnm_framework", "vnm_qt_dispatch", "vnm_cmake")}
            arguments = ["prepare_ci.py", "--platform", "macos-arm64", "--workspace", str(root),
                         "--output", str(output)]
            with patch.object(PREPARE.sys, "argv", arguments), \
                 patch.dict(os.environ, {"VARINOMICS_REPO_READ_TOKEN": "private-read-token",
                                         "GITHUB_PATH": str(github_path)}), \
                 patch.object(PREPARE, "dependency_sources", return_value=sources), \
                 patch.object(PREPARE, "prepare_qt", return_value=(root / "qt", root / "qtbase")):
                PREPARE.main()
                self.assertNotIn("VARINOMICS_REPO_READ_TOKEN", os.environ)
            cmake_args = json.loads(output.read_text(encoding="utf-8"))
            self.assertIn(f"-DKIMI_FRAMEWORK_SOURCE={sources['vnm_framework'].as_posix()}", cmake_args)
            self.assertIn(f"-DKIMI_QTBASE_SOURCE={(root / 'qtbase').as_posix()}", cmake_args)
            self.assertIn("-DVNM_FILESYSTEM_OBSERVATION_BUILD_TESTS=OFF", cmake_args)
            self.assertEqual(github_path.read_text(encoding="utf-8"), str(root / "qt/bin") + "\n")
            self.assertNotIn("private-read-token", output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()

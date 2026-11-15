"""Exercise the assembler's metadata handoff with native operations stubbed."""

import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


DEPLOYER = Path(sys.argv.pop(1)).resolve()
CMAKE = sys.argv.pop(1)
SDK = Path(sys.argv.pop(1)).resolve()
SOURCE = Path(__file__).resolve().parents[1]


class Linux_runtime_provenance(unittest.TestCase):
    def test_real_assembler_records_deployed_bytes_before_index_and_preserves_noop(self):
        with tempfile.TemporaryDirectory(prefix="kimi-linux-metadata-") as temporary:
            root = Path(temporary)
            source, package = root / "source", root / "package"
            (source / "cmake").mkdir(parents=True)
            (source / "package/notices").mkdir(parents=True)
            (source / "package/notices/license.txt").write_text("Supplier license\n")
            (root / "generated").mkdir()
            (root / "generated/logonomic-cli-addon.json").write_text("{}")
            (root / "LICENSE").write_text("toml++ license")
            library = root / "libQt6Core.so.6"
            library.write_bytes(b"\x7fELFvendor-input")
            (source / "cmake/qt_runtime_dependencies.cmake").write_text(
                f'function(qt_resolve_runtime_dependencies output)\n'
                f'  set(${{output}} "{library.as_posix()}" PARENT_SCOPE)\nendfunction()\n')
            programs = ("kimi_adapter", "kimi_helper", "kimi_launch_bridge", "kimi_quota_probe")
            for name in programs:
                (root / name).write_bytes(b"\x7fELFcontrolled-input")
            wrapper = root / "record.py"
            wrapper.write_text(
                'import importlib.util, sys\n'
                f'spec = importlib.util.spec_from_file_location("runtime", {str(DEPLOYER)!r})\n'
                'runtime = importlib.util.module_from_spec(spec)\nspec.loader.exec_module(runtime)\n'
                'runtime.controlled_release = lambda *args: None\n'
                'runtime.verify = lambda *args: None\n'
                'runtime.runtime_material = lambda *args: ("Supplier license", '
                '{"binary_package": "qtbase", "binary_version": "6.12.0"})\n'
                'runtime.main()\n')
            indexer = root / "index.py"
            indexer.write_text(
                'import hashlib, json, pathlib, sys\n'
                'root = pathlib.Path(sys.argv[1])\n'
                'report = json.loads((root / "provenance/linux-runtime.json").read_text())\n'
                'assert len(report["files"]) == 5\n'
                'for row in report["files"]:\n'
                '    assert row["sha256"] == hashlib.sha256((root / row["path"]).read_bytes()).hexdigest()\n'
                '    assert row["source_sha256"] != row["sha256"]\n'
                'triples = sys.argv[2:]\n'
                'assert ["--third-party", "adapter/libQt6Core.so.6", "notices/runtime.txt", '
                '"provenance/linux-runtime.json"] == triples[-4:]\n'
                'entries = []\n'
                'for row in report["files"]:\n'
                '    resource = {"kind": "executable", "origin": "package"}\n'
                '    if row["owner"] == "supplied-runtime":\n'
                '        resource.update(origin="third-party", provenance="provenance/linux-runtime.json")\n'
                '    entries.append({"path": row["path"], "sha256": row["sha256"], "resource": resource})\n'
                '(root / "package.files.json").write_text(json.dumps({"files": entries}))\n')
            patchelf = root / "patchelf-input"
            patchelf.write_text("Native tool fixture")
            replacements = {
                "qt_package_directory": package.as_posix(), "CMAKE_CURRENT_SOURCE_DIR": source.as_posix(),
                "CMAKE_CURRENT_BINARY_DIR": root.as_posix(), "CMAKE_SYSTEM_NAME": "Linux",
                "qt_runtime_directory": (source / "cmake").as_posix(), "qt_recipe_directory": root.as_posix(),
                "qt_extra_copies": (root / "LICENSE").as_posix() + "|notices/tomlplusplus-LICENSE.txt",
                "qt_executables": ";".join((root / name).as_posix() for name in programs),
                "patchelf_executable": patchelf.as_posix(),
                "linux_runtime_deployer": wrapper.as_posix(), "Python3_EXECUTABLE": Path(sys.executable).as_posix(),
                "qt_package_indexer": f"{Path(sys.executable).as_posix()};{indexer.as_posix()}",
            }
            text = (SDK / "cmake/qt_runtime_assemble.cmake.in").read_text()
            for key, value in replacements.items():
                text = text.replace("@" + key + "@", value)
            text = re.sub(r"@[A-Za-z0-9_]+@", "", text)
            for name in programs:
                text = text.replace("$<TARGET_FILE:" + name + ">", (root / name).as_posix())
            text = text.replace("$<TARGET_FILE:logonomic_cli_addon_index>", indexer.as_posix())
            text = text.replace("$<TARGET_FILE_DIR:Qt6::Core>", root.as_posix()).replace("$<CONFIG>", "Release")
            assembler = root / "assemble.cmake"
            assembler.write_text(text)
            driver = root / "driver.cmake"
            driver.write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                'macro(file operation)\n'
                '  if("${operation}" STREQUAL "RPATH_CHANGE")\n'
                '    _file(APPEND "${ARGV2}" "deployed-rpath")\n'
                '  else()\n    _file(${ARGV})\n  endif()\nendmacro()\n'
                'function(execute_process)\n'
                f'  if(ARGV1 STREQUAL "{patchelf.as_posix()}")\n'
                '    _file(APPEND "${ARGV4}" "deployed-rpath")\n'
                '  else()\n    _execute_process(${ARGV})\n  endif()\nendfunction()\n'
                f'include("{assembler.as_posix()}")\n')
            result = subprocess.run([CMAKE, "-P", str(driver)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads((package / "provenance/linux-runtime.json").read_text())
            self.assertEqual(sum(row["owner"] == "controlled" for row in report["files"]), 4)
            self.assertEqual(sum(row["owner"] == "supplied-runtime" for row in report["files"]), 1)
            index = package / "package.files.json"
            stamp, contents = index.stat().st_mtime_ns, index.read_bytes()
            result = subprocess.run([CMAKE, "-P", str(driver)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(index.stat().st_mtime_ns, stamp)
            self.assertEqual(index.read_bytes(), contents)
            layout = [CMAKE, "-DPACKAGE=" + package.as_posix(), "-P",
                      str(SOURCE / "tests/package_runtime_layout_tests.cmake")]
            result = subprocess.run(layout, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            (package / "adapter/libQt6Core.so.6").write_bytes(b"changed after recording")
            result = subprocess.run(layout, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("provenance differs from packaged bytes", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

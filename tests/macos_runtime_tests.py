"""Check Mach-O symbol inspection independently of an installed Apple SDK."""

import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest


specification = importlib.util.spec_from_file_location(
    "macos_runtime", Path(sys.argv.pop(1)))
runtime = importlib.util.module_from_spec(specification)
specification.loader.exec_module(runtime)


def image(symbol_type=0x0E, cpu=0x0100000C):
    command = struct.pack("<6I", 2, 24, 56, 1, 72, 1)
    header = struct.pack("<8I", 0xFEEDFACF, cpu, 0, 2, 1, len(command), 0, 0)
    symbol = struct.pack("<IBBHQ", 0, symbol_type, 1, 0, 0)
    return header + command + symbol + b"\0"


class Macos_runtime_tests(unittest.TestCase):
    def inspect(self, data):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "native-image"
            path.write_bytes(data)
            return runtime.inspect_image(path)

    def test_operational_symbols_are_not_debugging_data(self):
        self.assertFalse(self.inspect(image())[0]["debug_data"])

    def test_stabs_source_records_are_debugging_data(self):
        self.assertTrue(self.inspect(image(symbol_type=0x64))[0]["debug_data"])

    def test_universal_image_inspects_every_supplied_slice(self):
        intel = image(symbol_type=0x64, cpu=0x01000007)
        arm = image()
        header = struct.pack(">2I", 0xCAFEBABE, 2)
        header += struct.pack(">5I", 0x01000007, 0, 48, len(intel), 0)
        header += struct.pack(">5I", 0x0100000C, 0, 48 + len(intel), len(arm), 0)
        slices = self.inspect(header + intel + arm)
        self.assertEqual([row["debug_data"] for row in slices], [True, False])

    def test_truncated_symbol_table_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "outside its image"):
            self.inspect(image()[:60])


if __name__ == "__main__":
    unittest.main()

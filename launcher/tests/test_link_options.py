# ps5-native-app-boilerplate - Extension SDK option validation regression.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

from pathlib import Path
import subprocess
import tempfile
import unittest


class LinkOptionsTests(unittest.TestCase):
    def test_imports_and_wrappers(self):
        source = (Path(__file__).resolve().parents[1] / "tools/build.sh").read_text()
        block = source.split("# Extension SDK imports", 1)[1]
        block = block[block.index("stub_paths=()"):].split("ninja_inputs=", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "graphics.so").touch()

            def run(stub, symbol):
                return subprocess.run(
                    ["bash", "-c", 'set -eu; root=$1; import_stubs=("$2"); '
                     'wrap_symbols=("$3");\n' + block +
                     '\nprintf "%s\\n" "${stub_paths[@]}" "${stub_options[@]}" "${wrap_options[@]}"',
                     "test", directory, stub, symbol], capture_output=True, text=True)

            valid = run("graphics.so", "malloc")
            self.assertEqual(valid.returncode, 0, valid.stderr)
            self.assertEqual(valid.stdout.splitlines(),
                             [directory + "/graphics.so", "--stub",
                              directory + "/graphics.so", "--wrap=malloc"])
            for stub, symbol in [("missing.so", "malloc"), ("/graphics.so", "malloc"),
                                 ("graphics.a", "malloc"), ("graphics.so", "--bad"),
                                 ("graphics.so", "malloc;false")]:
                self.assertEqual(run(stub, symbol).returncode, 2)

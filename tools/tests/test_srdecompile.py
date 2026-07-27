"""Tests for tools/srdecompile.py -- reading somebody else's .scr.

This tool is not in the build path: RScript.exe remains the only compiler
rson -> scr. It goes the other way, so a stock or reference script can be read
without driving the editor by hand.

No .scr is committed here (they are other people's work and the game's), so
the round trip is exercised only when one happens to sit under references/.
What runs everywhere are the format primitives the parser is built on and the
CLI itself.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "srdecompile.py"
SPEC = importlib.util.spec_from_file_location("srdecompile", TOOL)
assert SPEC is not None and SPEC.loader is not None
SCR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SCR)


def _reference_scripts() -> list[Path]:
    references = ROOT / "references"
    if not references.is_dir():
        return []
    return sorted(references.rglob("*.scr"))[:1]


class StringPrimitiveTests(unittest.TestCase):
    def test_reads_a_utf16_string_to_its_double_null(self) -> None:
        data = "Mod_CE_Core".encode("utf-16-le") + b"\0\0" + b"tail"
        text, cursor = SCR.read_wstr(data, 0)
        self.assertEqual(text, "Mod_CE_Core")
        self.assertEqual(data[cursor:], b"tail")

    def test_reads_a_null_terminated_string(self) -> None:
        data = "CE_Item_TwinHomeAnchor".encode("utf-16-le") + b"\0\0" + b"\x01\x00"
        text, cursor = SCR.read_wstr_nt(data, 0)
        self.assertEqual(text, "CE_Item_TwinHomeAnchor")
        self.assertEqual(cursor, len(data) - 2)

    def test_reads_cyrillic(self) -> None:
        data = "Второй Дом".encode("utf-16-le") + b"\0\0"
        self.assertEqual(SCR.read_wstr(data, 0)[0], "Второй Дом")


class LangTableTests(unittest.TestCase):
    def test_loads_ct_keys_out_of_a_lang_table(self) -> None:
        text = (
            "Script {\n"
            "  Mod_CE_Core {\n"
            "    1=Якорь Двух Домов\n"
            "    2=Первая строка<br>вторая строка\n"
            "  }\n"
            "}\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Lang.txt"
            path.write_bytes(b"\xff\xfe" + text.encode("utf-16-le"))
            lang = SCR.load_lang(str(path))
        self.assertEqual(lang["Script.Mod_CE_Core.1"], "Якорь Двух Домов")
        self.assertEqual(lang["Script.Mod_CE_Core.2"], "Первая строка\r\nвторая строка")

    def test_missing_lang_file_is_not_fatal(self) -> None:
        self.assertEqual(SCR.load_lang("nowhere/Lang.txt"), {})


class CommandLineTests(unittest.TestCase):
    def test_cli_reports_its_usage(self) -> None:
        result = subprocess.run(
            [sys.executable, str(TOOL), "--help"],
            capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("decompiler", result.stdout.lower())

    @unittest.skipUnless(_reference_scripts(), "no .scr under references/")
    def test_decompiles_a_reference_script(self) -> None:
        source = _reference_scripts()[0]
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "out.rson"
            SCR.decompile(str(source), str(out))
            self.assertTrue(out.exists(), f"no output for {source.name}")
            rson = json.loads(out.read_text(encoding="utf-8"))
        for key in ("FileID", "FileVersion", "ScriptName", "Visual.Objects"):
            self.assertIn(key, rson)


if __name__ == "__main__":
    unittest.main()

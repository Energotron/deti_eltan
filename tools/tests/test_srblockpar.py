"""Tests for tools/srblockpar.py -- the batch wrapper around BlockParEditor.exe.

BlockParEditor is not committed (it lives under references/, which is not in
the repository), so the editor itself is stood in for by a fake launcher that
records the command line it was handed and writes the file the real one would.
That is enough to pin down everything this wrapper actually owns: which files
a batch picks up, which direction each goes, where the result lands, what the
command line looks like, and how a failure is reported.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("srblockpar", ROOT / "tools" / "srblockpar.py")
assert SPEC is not None and SPEC.loader is not None
BP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BP)

FAKE_EXE = "fake-BlockParEditor.exe"


class FakeEditor:
    """Stands in for BlockParEditor.exe: writes the output, logs the call."""

    def __init__(self, fail_on=()):
        self.calls = []
        self.fail_on = set(fail_on)

    def __call__(self, args, timeout):
        self.calls.append(list(args))
        source = Path(args[3])
        dest = Path(args[4]) if len(args) > 4 else BP.swapped_suffix(source)
        if source.name in self.fail_on:
            return "Error open dat"
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(b"BPAR" + source.read_bytes())
        return ""


def _tree(root: Path) -> None:
    (root / "Rus").mkdir(parents=True, exist_ok=True)
    (root / "Main.txt").write_bytes(b"Main")
    (root / "CacheData.txt").write_bytes(b"Cache")
    (root / "Rus" / "Lang.txt").write_bytes(b"Lang")
    (root / "leave-me.dat").write_bytes(b"already binary")


class PlanTests(unittest.TestCase):
    def test_batch_picks_up_the_whole_tree_in_one_direction(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _tree(root)
            jobs = BP.plan_batch(root, to_dat=True)
            self.assertEqual(
                sorted(str(source.relative_to(root)) for source, _ in jobs),
                [str(Path("CacheData.txt")), str(Path("Main.txt")), str(Path("Rus/Lang.txt"))],
            )
            for source, dest in jobs:
                self.assertEqual(dest.suffix, ".dat")
                self.assertEqual(dest.parent, source.parent, "results land beside the source")

    def test_default_direction_is_dat_to_txt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _tree(root)
            jobs = BP.plan_batch(root)
            self.assertEqual([source.name for source, _ in jobs], ["leave-me.dat"])
            self.assertEqual(jobs[0][1].name, "leave-me.txt")

    def test_direction_follows_the_extension(self) -> None:
        self.assertEqual(BP.swapped_suffix(Path("CFG/Main.dat")).name, "Main.txt")
        self.assertEqual(BP.swapped_suffix(Path("CFG/Main.txt")).name, "Main.dat")


class ConversionTests(unittest.TestCase):
    def test_batch_converts_every_file_and_reports_them(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _tree(root)
            editor = FakeEditor()
            lines: list[str] = []
            ok, failures = BP.run_batch(FAKE_EXE, root, to_dat=True,
                                        launcher=editor, echo=lines.append)
            self.assertEqual((ok, failures), (3, []))
            self.assertEqual((root / "Main.dat").read_bytes(), b"BPARMain")
            self.assertEqual((root / "CacheData.dat").read_bytes(), b"BPARCache")
            self.assertEqual((root / "Rus" / "Lang.dat").read_bytes(), b"BPARLang")
            self.assertEqual(len(lines), 3)
            self.assertTrue(all(line.startswith("  OK") for line in lines))

    def test_the_command_line_is_the_documented_one(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "Main.txt"
            source.write_bytes(b"Main")
            editor = FakeEditor()
            ok, message = BP.convert_one(FAKE_EXE, source, launcher=editor)
            self.assertTrue(ok, message)
            self.assertEqual(editor.calls, [[FAKE_EXE, "--cli", "--convert", str(source)]])

    def test_explicit_destination_is_passed_through(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "CE_MapSmoke.Main.txt"
            source.write_bytes(b"Main")
            dest = root / "out" / "Main.dat"
            editor = FakeEditor()
            ok, message = BP.convert_one(FAKE_EXE, source, dest, launcher=editor)
            self.assertTrue(ok, message)
            self.assertEqual(Path(message), dest)
            self.assertEqual(editor.calls[0][-1], str(dest))
            self.assertEqual(dest.read_bytes(), b"BPARMain")

    def test_a_modal_error_is_a_failure_not_a_hang(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _tree(root)
            editor = FakeEditor(fail_on={"Main.txt"})
            ok, failures = BP.run_batch(FAKE_EXE, root, to_dat=True,
                                        launcher=editor, echo=lambda _line: None)
            self.assertEqual(ok, 2)
            self.assertEqual([source.name for source, _ in failures], ["Main.txt"])
            self.assertIn("Error open dat", failures[0][1])
            self.assertFalse((root / "Main.dat").exists())

    def test_missing_source_is_reported(self) -> None:
        ok, message = BP.convert_one(FAKE_EXE, Path("nowhere/Main.txt"),
                                     launcher=FakeEditor())
        self.assertFalse(ok)
        self.assertIn("source not found", message)

    def test_a_stale_output_does_not_count_as_success(self) -> None:
        """An editor that writes nothing must not be masked by last build's file."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "Main.txt"
            source.write_bytes(b"Main")
            stale = root / "Main.dat"
            stale.write_bytes(b"from a previous build")
            import os
            os.utime(stale, (0, 0))
            ok, message = BP.convert_one(FAKE_EXE, source,
                                         launcher=lambda args, timeout: "")
            self.assertFalse(ok)
            self.assertIn("no output written", message)


class DiscoveryTests(unittest.TestCase):
    def test_missing_editor_is_reported_rather_than_guessed(self) -> None:
        self.assertIsNone(BP.find_blockpar("nowhere/BlockParEditor.exe"))

    def test_default_location_is_the_repository_reference_copy(self) -> None:
        self.assertEqual(
            BP.DEFAULT_BLOCKPAR,
            ROOT / "references" / "tools" / "BlockParEditor_1.9" / "BlockParEditor.exe",
        )


if __name__ == "__main__":
    unittest.main()

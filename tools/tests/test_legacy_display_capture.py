from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools" / "native" / "ce_legacy_display_capture.cpp"


class LegacyDisplayCaptureSourceTests(unittest.TestCase):
    def test_has_dxgi_and_gdi_capture_paths(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")
        self.assertIn("DuplicateOutput", text)
        self.assertIn("AcquireNextFrame", text)
        self.assertIn("BitBlt", text)
        self.assertIn("ce_frame_is_near_black", text)

    def test_does_not_inject_or_mutate_the_game_process(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")
        for name in (
            "OpenProcess",
            "WriteProcessMemory",
            "CreateRemoteThread",
            "VirtualAllocEx",
            "SetWindowsHookEx",
        ):
            with self.subTest(name=name):
                self.assertNotIn(name, text)


if __name__ == "__main__":
    unittest.main()

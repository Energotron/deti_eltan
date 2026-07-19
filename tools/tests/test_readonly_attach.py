from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools" / "native" / "ce_galaxy_attach.c"


class ReadOnlyAttachSourceTests(unittest.TestCase):
    def test_process_handle_is_query_and_read_only(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")
        call = re.search(r"OpenProcess\((.*?)\);", text, re.DOTALL)
        self.assertIsNotNone(call)
        self.assertIn("PROCESS_QUERY_INFORMATION | PROCESS_VM_READ", call.group(1))
        self.assertNotIn("PROCESS_ALL_ACCESS", call.group(1))

    def test_mutating_and_injection_apis_are_absent(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")
        forbidden = (
            "WriteProcessMemory",
            "CreateRemoteThread",
            "VirtualAllocEx",
            "VirtualProtectEx",
            "SetThreadContext",
            "QueueUserAPC",
        )
        for name in forbidden:
            with self.subTest(name=name):
                self.assertNotIn(name, text)

    def test_stable_profile_is_intersection_of_observed_masks(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")
        zero = int(re.search(r"CE_STABLE_ZERO_MASK UINT64_C\(0x([0-9A-F]+)\)", text).group(1), 16)
        pointer = int(re.search(r"CE_STABLE_POINTER_MASK UINT64_C\(0x([0-9A-F]+)\)", text).group(1), 16)
        self.assertEqual(zero, 0xEABCE07DC0090400 & 0x0D58404000000400)
        self.assertEqual(pointer, 0x000300003000F801 & 0x00070001F000F801)
        self.assertEqual(zero & pointer, 0)

    def test_pointer_targets_are_bounded_and_address_free(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")
        self.assertIn("#define CE_TARGET_SAMPLE_BYTES 64u", text)
        self.assertIn('"{\\"schema\\":2', text)
        self.assertIn("--root-hashes", text)
        self.assertNotIn('\\"address\\"', text)


if __name__ == "__main__":
    unittest.main()

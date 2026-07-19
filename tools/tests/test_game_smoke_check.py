import json
from pathlib import Path
import tempfile
import unittest

from tools import game_smoke_check


class GalaxyLayoutAnalysisTests(unittest.TestCase):
    def make_record(self, process_id: int, zero_mask: str = "F000000000000001") -> dict:
        return {
            "abi": game_smoke_check.EXPECTED_ABI,
            "process_id": process_id,
            "sample_tag": game_smoke_check.EXPECTED_MARKER,
            "sample_bytes": 256,
            "block_fnv1a32": [11, 22, 33, process_id],
            "zero_mask": zero_mask,
            "readable_pointer_mask": "0000000000000002",
            "raw_values_included": False,
            "read_only": True,
        }

    def test_requires_independent_processes(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "samples.jsonl"
            records = [self.make_record(10), self.make_record(10), self.make_record(20)]
            path.write_text("\n".join(json.dumps(record) for record in records), encoding="utf-8")
            self.assertEqual(game_smoke_check.analyze_layouts(path, 3), 3)

    def test_accepts_three_processes_and_ignores_host_samples(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "samples.jsonl"
            host = self.make_record(1)
            host["sample_tag"] = 0x1234ABCD
            records = [host, self.make_record(10), self.make_record(20), self.make_record(30)]
            path.write_text("\n".join(json.dumps(record) for record in records), encoding="utf-8")
            self.assertEqual(len(game_smoke_check.layout_records(path)), 3)
            self.assertEqual(game_smoke_check.analyze_layouts(path, 3), 0)

    def test_skips_malformed_jsonl_lines(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "samples.jsonl"
            path.write_text("not-json\n" + json.dumps(self.make_record(10)), encoding="utf-8")
            self.assertEqual(len(game_smoke_check.layout_records(path)), 1)


if __name__ == "__main__":
    unittest.main()

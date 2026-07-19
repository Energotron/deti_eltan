import json
from pathlib import Path
import tempfile
import unittest

from tools import game_smoke_check


class GalaxyLayoutAnalysisTests(unittest.TestCase):
    def make_record(
        self,
        process_id: int,
        zero_mask: str = "F000000000000001",
        observation_tag: int = 100,
        sequence: int = 1,
    ) -> dict:
        return {
            "abi": game_smoke_check.EXPECTED_ABI,
            "process_id": process_id,
            "sample_tag": game_smoke_check.EXPECTED_MARKER,
            "observation_tag": observation_tag,
            "sequence": sequence,
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

    def test_timeline_accepts_turn_advance_and_cross_process_reload(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "samples.jsonl"
            before = self.make_record(10, observation_tag=100, sequence=1)
            before["phase"] = "before"
            after = self.make_record(10, observation_tag=101, sequence=2)
            after["phase"] = "after"
            after["block_fnv1a32"] = [41, 42, 43, 44]
            reloaded = self.make_record(20, observation_tag=101, sequence=1)
            reloaded["phase"] = "reload"
            reloaded["block_fnv1a32"] = [51, 52, 43, 54]
            path.write_text(
                "\n".join(json.dumps(record) for record in (before, after, reloaded)),
                encoding="utf-8",
            )
            self.assertEqual(game_smoke_check.analyze_timeline(path), 0)

    def test_timeline_requires_reload_in_a_second_process(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "samples.jsonl"
            records = [
                self.make_record(10, observation_tag=100, sequence=1),
                self.make_record(10, observation_tag=101, sequence=2),
            ]
            records[0]["phase"] = "before"
            records[1]["phase"] = "after"
            path.write_text("\n".join(json.dumps(record) for record in records), encoding="utf-8")
            self.assertEqual(game_smoke_check.analyze_timeline(path), 3)

    def test_capture_archives_only_named_milestones(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            latest = Path(temp_dir) / "latest.json"
            timeline = Path(temp_dir) / "timeline.jsonl"
            latest.write_text(json.dumps(self.make_record(10)), encoding="utf-8")
            self.assertEqual(game_smoke_check.capture_layout(latest, timeline, "before"), 0)
            captured = json.loads(timeline.read_text(encoding="utf-8"))
            self.assertEqual(captured["phase"], "before")
            self.assertIn("captured_ns", captured)

    def test_capture_rejects_stale_process_or_turn(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            latest = Path(temp_dir) / "latest.json"
            timeline = Path(temp_dir) / "timeline.jsonl"
            latest.write_text(json.dumps(self.make_record(10, observation_tag=301)), encoding="utf-8")
            self.assertEqual(
                game_smoke_check.capture_layout(
                    latest, timeline, "reload", expected_process_id=20, expected_turn=301
                ),
                3,
            )
            self.assertEqual(
                game_smoke_check.capture_layout(
                    latest, timeline, "reload", expected_process_id=10, expected_turn=302
                ),
                3,
            )
            self.assertFalse(timeline.exists())


if __name__ == "__main__":
    unittest.main()

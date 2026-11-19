import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import partybot_trace_report as report


class PartyBotTraceReportTests(unittest.TestCase):
    def test_parse_structured_trace_line(self):
        line = (
            "2026-11-19T05:20:17.123456Z INFO [PartyBotEngineTrace] "
            "trace_version=1 encounter_id=123 seq=7 event=death "
            "bot=55 elapsed_ms=900 alive_before=1 alive_after=0"
        )
        parsed = report.parse_trace_line(line, "server.log", 4)
        self.assertIsNotNone(parsed)
        self.assertEqual(parsed.encounter_id, 123)
        self.assertEqual(parsed.sequence, 7)
        self.assertEqual(parsed.event, "death")
        self.assertEqual(parsed.fields["alive_after"], "0")
        self.assertEqual(parsed.timestamp.isoformat(), "2026-11-19T05:20:17.123456+00:00")

    def test_malformed_or_non_trace_lines_are_ignored(self):
        self.assertIsNone(report.parse_trace_line("ordinary mangosd log line"))
        self.assertIsNone(report.parse_trace_line(
            "[PartyBotEngineTrace] trace_version=1 encounter_id=x seq=1 event=death"
        ))
        self.assertIsNone(report.parse_trace_line(
            "[PartyBotEngineTrace] trace_version=1 encounter_id=0 seq=1 event=death"
        ))

    def test_exact_encounter_selection_and_raw_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "server.log"
            output = root / "reports"
            source.write_text(
                "2026-11-19T05:00:00Z INFO [PartyBotEngineTrace] trace_version=1 encounter_id=10 seq=1 event=encounter_start elapsed_ms=0 reason=combat_detected\n"
                "2026-11-19T05:00:01Z INFO [PartyBotEngineTrace] trace_version=1 encounter_id=10 seq=3 event=trace_limit elapsed_ms=100 category=cast_lifecycle\n"
                "2026-11-19T05:00:02Z INFO [PartyBotEngineTrace] trace_version=1 encounter_id=10 seq=4 event=party_snapshot truncated=1 members_omitted=1 elapsed_ms=200\n"
                "2026-11-19T05:00:03Z INFO [PartyBotEngineTrace] trace_version=1 encounter_id=10 seq=5 event=encounter_end elapsed_ms=300 reason=combat_settled\n"
                "2026-11-19T05:00:04Z INFO [PartyBotEngineTrace] trace_version=1 encounter_id=11 seq=1 event=encounter_start elapsed_ms=0\n",
                encoding="utf-8",
            )
            files = report.iter_input_files([source], output)
            records, scanned, malformed = report.load_trace_records(files)
            selected, missing = report.select_encounters(records, 10, None, None)
            report_path, raw_path = report.write_encounter_report(
                output, 10, selected[10], scanned, malformed, missing
            )
            rendered = report_path.read_text(encoding="utf-8")
            raw = raw_path.read_text(encoding="utf-8")
            self.assertIn("Sequence gaps: **1**", rendered)
            self.assertIn("trace_limit", rendered)
            self.assertIn("party_snapshot payload was truncated", rendered)
            self.assertIn("encounter_end", rendered)
            self.assertNotIn("encounter_id=11", raw)
            self.assertEqual(len(raw.splitlines()), 4)

    def test_time_window_discovers_encounters_and_expands_full_trace(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "mangosd.log"
            source.write_text(
                "2026-11-19T04:59:00Z [PartyBotEngineTrace] encounter_id=22 seq=1 event=encounter_start\n"
                "2026-11-19T05:05:00Z [PartyBotEngineTrace] encounter_id=22 seq=2 event=death\n"
                "2026-11-19T05:10:00Z [PartyBotEngineTrace] encounter_id=23 seq=1 event=encounter_start\n"
                "2026-11-19T05:30:00Z [PartyBotEngineTrace] encounter_id=22 seq=3 event=encounter_end\n",
                encoding="utf-8",
            )
            records, _, _ = report.load_trace_records([source])
            since = report.parse_datetime_argument("2026-11-19T05:00:00Z")
            until = report.parse_datetime_argument("2026-11-19T05:08:00Z")
            selected, missing = report.select_encounters(records, None, since, until)
            self.assertEqual(sorted(selected), [22])
            self.assertEqual([record.event for record in selected[22]],
                             ["encounter_start", "death", "encounter_end"])
            self.assertEqual(missing, 0)

    def test_naive_and_aware_time_comparison_is_rejected(self):
        record = report.parse_trace_line(
            "2026-11-19T05:00:00Z [PartyBotEngineTrace] encounter_id=30 seq=1 event=encounter_start"
        )
        with self.assertRaises(ValueError):
            report.select_encounters(
                [record], None,
                report.parse_datetime_argument("2026-11-19T04:00:00"),
                report.parse_datetime_argument("2026-11-19T06:00:00"),
            )


if __name__ == "__main__":
    unittest.main()

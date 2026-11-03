# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
import importlib.util
from pathlib import Path
import unittest

source = Path(__file__).resolve().parents[2] / "scripts/macos/presentation.py"
spec = importlib.util.spec_from_file_location("presentation", source)
presentation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(presentation)


class PresentationTest(unittest.TestCase):
    def test_matches_drawable_identity_and_maps_clock_epoch(self):
        event = {"drawable_sequence": 2, "event_trace_time_ns": 1_000_000_000, "prepared_frame_callback_ns": 1_010_000_000}
        frame = {"sequence": 2, "trace_now_ns": 1_035_000_000, "ca_now_seconds": 10.035, "presented_seconds": 10.025}
        pairs, missing, skipped = presentation.correlate([event], [{**frame, "sequence": 1, "presented_seconds": 10.030}, frame])
        self.assertEqual((missing, skipped), (0, 0))
        self.assertAlmostEqual(pairs[0]["event_to_presented_ms"], 25)

    def test_missing_or_skipped_response_is_not_a_zero_latency_pass(self):
        events = [{"drawable_sequence": 1}, {"drawable_sequence": 2}]
        pairs, missing, skipped = presentation.correlate(events, [{"sequence": 2, "presented_seconds": 0}])
        self.assertEqual((pairs, missing, skipped), ([], 1, 1))
        self.assertIsNone(presentation.percentile([], .99))

    def test_background_window_does_not_hide_foreground_frame_gap(self):
        events = [{"drawable_sequence": 1}, {"drawable_sequence": 4}]
        drawables = [{"sequence": 1, "layer_id": 10, "presented_seconds": 1},
                     {"sequence": 2, "layer_id": 20, "presented_seconds": 1.0167},
                     {"sequence": 3, "layer_id": 20, "presented_seconds": 1.0333},
                     {"sequence": 4, "layer_id": 10, "presented_seconds": 1.050}]
        frames, layer = presentation.response_frames(events, drawables)
        self.assertEqual(layer, 10)
        self.assertEqual([frame["sequence"] for frame in frames], [1, 4])
        self.assertAlmostEqual((frames[1]["presented_seconds"] - frames[0]["presented_seconds"]) * 1000, 50)

    def test_multiple_response_layers_are_a_protocol_error(self):
        events = [{"drawable_sequence": 1}, {"drawable_sequence": 2}]
        with self.assertRaisesRegex(ValueError, "multiple presentation layers"):
            presentation.response_frames(events, [{"sequence": 1, "layer_id": 10}, {"sequence": 2, "layer_id": 20}])

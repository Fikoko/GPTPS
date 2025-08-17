# tests/test_tracker.py
import unittest
import time
import json
from pathlib import Path
from datetime import datetime
from src.work_process_types.background_process.tracker.tracker import Tracker

class TestTracker(unittest.TestCase):
    def setUp(self):
        # Temporary files for testing
        self.analytics_file = Path("test_analytics.json")
        self.session_file = Path("test_session.json")

        # Ensure clean start
        for file in [self.analytics_file, self.session_file]:
            if file.exists():
                file.unlink()

        self.tracker = Tracker(
            analytics_path=self.analytics_file,
            session_path=self.session_file,
            max_records=3,  # smaller number for easier testing
            worker_poll_interval=0.1
        )

        # Capture broadcast messages
        self.broadcast_events = []
        self.tracker.register_broadcast_callback(
            lambda event_type, task_name, ts: self.broadcast_events.append((event_type, task_name, ts))
        )

    def tearDown(self):
        self.tracker.stop()
        for file in [self.analytics_file, self.session_file]:
            if file.exists():
                file.unlink()

    def test_start_and_end_task(self):
        self.tracker.start_task("TaskAlpha")
        time.sleep(0.2)
        self.tracker.end_task("TaskAlpha")

        # Check broadcast events
        events = [e[0] for e in self.broadcast_events]
        self.assertEqual(events, ["task_started", "task_finished"])

        # Check analytics.json
        with open(self.analytics_file, "r", encoding="utf-8") as f:
            data = json.load(f)
        self.assertIn("TaskAlpha", data)
        self.assertEqual(len(data["TaskAlpha"]), 1)

        # Check session_config.json
        with open(self.session_file, "r", encoding="utf-8") as f:
            session_data = json.load(f)
        self.assertIn("TaskAlpha", session_data["tasks"])
        self.assertEqual(len(session_data["tasks"]["TaskAlpha"]), 1)

    def test_push_provider_event(self):
        self.tracker.push_provider_event("start", "TaskBeta")
        time.sleep(0.1)
        self.tracker.push_provider_event("end", "TaskBeta")
        time.sleep(0.2)  # allow worker thread to process

        # Validate analytics.json
        with open(self.analytics_file, "r", encoding="utf-8") as f:
            data = json.load(f)
        self.assertIn("TaskBeta", data)
        self.assertEqual(len(data["TaskBeta"]), 1)

if __name__ == "__main__":
    unittest.main()

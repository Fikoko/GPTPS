import json
import threading
from datetime import datetime
from pathlib import Path
from queue import Queue, Empty
from typing import Callable, Optional, Dict, List
from fast_logger import AnalyticsLoggerCPP  # C++ logger


class AnalyticsLogger:
    def __init__(self, analytics_file: Path = Path("analytics.json"), max_records: int = 5, task_types: Optional[list] = None):
        self.analytics_file = Path(analytics_file)
        self.max_records = max_records
        self.task_types = task_types or []
        self._logger = AnalyticsLoggerCPP(str(analytics_file), max_records)

        if not self.analytics_file.exists():
            with open(self.analytics_file, "w", encoding="utf-8") as f:
                json.dump({}, f, indent=2)

        # Startup cleanup (remove dummy entries)
        self._cleanup_file()

    def _cleanup_file(self):
        try:
            with open(self.analytics_file, "r+", encoding="utf-8") as f:
                data = json.load(f)
                clean_data = {}
                for task, records in data.items():
                    # Keep only valid records (with end_time and numeric duration)
                    valid = [
                        r for r in records
                        if r.get("end_time") and isinstance(r.get("duration_seconds"), (int, float))
                    ]
                    if valid:
                        clean_data[task] = valid[-self.max_records:]
                f.seek(0)
                json.dump(clean_data, f, indent=2)
                f.truncate()
        except Exception as e:
            print(f"[AnalyticsLogger] Cleanup failed: {e}")

    def log(self, task_name: str, start_time: datetime, end_time: datetime):
        if self.task_types and task_name not in self.task_types:
            return

        duration = (end_time - start_time).total_seconds()
        self._logger.log(task_name, start_time.isoformat(), end_time.isoformat(), duration)

        # Batch append
        with open(self.analytics_file, "r+", encoding="utf-8") as f:
            data = json.load(f)
            if task_name not in data:
                data[task_name] = []
            data[task_name].append({
                "start_time": start_time.isoformat(),
                "end_time": end_time.isoformat(),
                "duration_seconds": duration
            })
            data[task_name] = data[task_name][-self.max_records:]
            f.seek(0)
            json.dump(data, f, indent=2)
            f.truncate()


class Tracker:
    def __init__(self,
                 analytics_path: Path = Path("analytics.json"),
                 session_path: Path = Path("session_config.json"),
                 max_records: int = 5,
                 worker_poll_interval: float = 0.5,
                 task_types: Optional[list] = None):
        self.logger = AnalyticsLogger(analytics_path, max_records, task_types)
        self.session_file = Path(session_path)
        self.max_records = max_records
        self.task_types = task_types or []

        # Session file init
        if not self.session_file.exists():
            with open(self.session_file, "w", encoding="utf-8") as f:
                json.dump({"session_start": datetime.now().isoformat(), "tasks": {}}, f, indent=2)

        # Startup cleanup (remove dummy entries)
        self._cleanup_session_file()

        # Worker thread
        self._active: Dict[str, datetime] = {}
        self._lock = threading.Lock()
        self._queue: Queue = Queue()
        self._stop_event = threading.Event()
        self._worker_poll_interval = worker_poll_interval
        self._broadcast_cb: Optional[Callable[[str, str, datetime], None]] = None
        self._worker = threading.Thread(target=self._worker_loop, daemon=True)
        self._worker.start()

    def _cleanup_session_file(self):
        try:
            with open(self.session_file, "r+", encoding="utf-8") as f:
                data = json.load(f)
                tasks = data.get("tasks", {})
                clean_tasks = {}
                for task, records in tasks.items():
                    valid = [
                        r for r in records
                        if r.get("end_time") and isinstance(r.get("duration_seconds"), (int, float))
                    ]
                    if valid:
                        clean_tasks[task] = valid[-self.max_records:]
                data["tasks"] = clean_tasks
                f.seek(0)
                json.dump(data, f, indent=2)
                f.truncate()
        except Exception as e:
            print(f"[Tracker] Session cleanup failed: {e}")

    # ---------------- Broadcast ----------------
    def register_broadcast_callback(self, cb: Callable[[str, str, datetime], None]):
        self._broadcast_cb = cb

    def _broadcast(self, event_type: str, task_name: str, timestamp: datetime):
        if self._broadcast_cb:
            try:
                self._broadcast_cb(event_type, task_name, timestamp)
            except Exception as e:
                print(f"[Tracker] broadcast callback error: {e}")

    # ---------------- Task Methods ----------------
    def start_task(self, task_name: str):
        if self.task_types and task_name not in self.task_types:
            return
        start_time = datetime.now()
        with self._lock:
            self._active[task_name] = start_time
        self._broadcast("task_started", task_name, start_time)

    def end_task(self, task_name: str):
        if self.task_types and task_name not in self.task_types:
            return
        end_time = datetime.now()
        with self._lock:
            start_time = self._active.pop(task_name, None)
        if start_time is None:
            print(f"[Tracker] end_task called but no start recorded for '{task_name}'")
            start_time = end_time

        self.logger.log(task_name, start_time, end_time)

        # Append to session
        with open(self.session_file, "r+", encoding="utf-8") as f:
            data = json.load(f)
            tasks = data.setdefault("tasks", {})
            if task_name not in tasks:
                tasks[task_name] = []
            tasks[task_name].append({
                "start_time": start_time.isoformat(),
                "end_time": end_time.isoformat(),
                "duration_seconds": (end_time - start_time).total_seconds()
            })
            tasks[task_name] = tasks[task_name][-self.max_records:]
            f.seek(0)
            json.dump(data, f, indent=2)
            f.truncate()

        self._broadcast("task_finished", task_name, end_time)

    # ---------------- External Events ----------------
    def push_provider_event(self, event_type: str, task_name: str):
        self._queue.put((event_type, task_name))

    def _handle_provider_event(self, event_type: str, task_name: str):
        if event_type.lower() in ("start", "task_started"):
            self.start_task(task_name)
        elif event_type.lower() in ("end", "task_finished"):
            self.end_task(task_name)
        else:
            print(f"[Tracker] unknown provider event_type '{event_type}' for task '{task_name}'")

    # ---------------- Worker Loop ----------------
    def _worker_loop(self):
        while not self._stop_event.is_set():
            try:
                event_type, task_name = self._queue.get(timeout=self._worker_poll_interval)
                self._handle_provider_event(event_type, task_name)
            except Empty:
                continue
            except Exception as e:
                print(f"[Tracker] worker exception: {e}")

    def stop(self, timeout: float = 2.0):
        self._stop_event.set()
        self._worker.join(timeout)

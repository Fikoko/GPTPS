# tracker.py
import json
import threading
import time
from datetime import datetime
from pathlib import Path
from queue import Queue, Empty
from typing import Callable, Optional, Dict

class AnalyticsLogger:
    def __init__(self, file_path: Path = Path("analytics.json"), max_records: int = 5):
        self.file_path = file_path
        self.max_records = max_records
        self._lock = threading.Lock()
        if not self.file_path.exists():
            self._atomic_save({})

    def _atomic_load(self) -> Dict:
        with self._lock:
            with open(self.file_path, "r", encoding="utf-8") as f:
                return json.load(f)

    def _atomic_save(self, data: Dict):
        with self._lock:
            with open(self.file_path, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)

    def log(self, task_name: str, start_time: datetime, end_time: datetime):
        duration = (end_time - start_time).total_seconds()
        rec = {
            "start_time": start_time.isoformat(),
            "end_time": end_time.isoformat(),
            "duration_seconds": duration
        }

        data = self._atomic_load()
        data.setdefault(task_name, [])
        # Insert newest record at the front (latest -> oldest)
        data[task_name].insert(0, rec)
        # Keep only last max_records entries
        data[task_name] = data[task_name][: self.max_records]
        self._atomic_save(data)


class Tracker:
    """
    Handles:
    - start_task(task_name)
    - end_task(task_name)
    - push_provider_event(event_type, task_name)  # for external event providers
    - Optional broadcast callback: broadcast(event_type, task_name, timestamp)
    Runs a background thread to process events from providers.
    """
    def __init__(
        self,
        analytics_path: Path = Path("analytics.json"),
        max_records: int = 5,
        worker_poll_interval: float = 0.5,
    ):
        self.logger = AnalyticsLogger(analytics_path, max_records)
        self._active: Dict[str, datetime] = {}    # task_name -> start_time
        self._lock = threading.Lock()             # protects _active
        self._queue: Queue = Queue()
        self._stop_event = threading.Event()
        self._worker_poll_interval = worker_poll_interval
        self._broadcast_cb: Optional[Callable[[str, str, datetime], None]] = None

        self._worker = threading.Thread(target=self._worker_loop, daemon=True)
        self._worker.start()

    # Register a callback to broadcast start/end events externally (e.g. to Ably/Firebase)
    def register_broadcast_callback(self, cb: Callable[[str, str, datetime], None]):
        """
        cb(event_type, task_name, timestamp)  
        event_type is 'task_started' or 'task_finished'
        """
        self._broadcast_cb = cb

    def _broadcast(self, event_type: str, task_name: str, timestamp: datetime):
        if self._broadcast_cb:
            try:
                self._broadcast_cb(event_type, task_name, timestamp)
            except Exception as e:
                print(f"[Tracker] broadcast callback error: {e}")

    # Called by your main AI or other processes when a task starts
    def start_task(self, task_name: str):
        with self._lock:
            self._active[task_name] = datetime.now()
        self._broadcast("task_started", task_name, self._active[task_name])

    # Called by your main AI or other processes when a task ends
    def end_task(self, task_name: str):
        with self._lock:
            start = self._active.pop(task_name, None)
        if start is None:
            print(f"[Tracker] end_task called but no start recorded for '{task_name}'")
            return
        end = datetime.now()
        self.logger.log(task_name, start, end)
        self._broadcast("task_finished", task_name, end)

    # For external message providers to push events into the tracker asynchronously
    def push_provider_event(self, event_type: str, task_name: str):
        self._queue.put((event_type, task_name))

    # Internal handler for provider events
    def _handle_provider_event(self, event_type: str, task_name: str):
        if event_type.lower() in ("start", "task_started"):
            self.start_task(task_name)
        elif event_type.lower() in ("end", "task_finished"):
            self.end_task(task_name)
        else:
            print(f"[Tracker] unknown provider event_type '{event_type}' for task '{task_name}'")

    # Worker thread that consumes the provider events queue
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


# ----------------- Example usage -----------------
if __name__ == "__main__":
    def dummy_broadcast(event_type, task_name, ts):
        print(f"[BROADCAST] {event_type} - {task_name} @ {ts.isoformat()}")

    tracker = Tracker()
    tracker.register_broadcast_callback(dummy_broadcast)

    # Simulate starting and ending some tasks dynamically
    tracker.start_task("task_alpha")
    time.sleep(1)
    tracker.end_task("task_alpha")

    tracker.push_provider_event("start", "task_beta")
    time.sleep(0.5)
    tracker.push_provider_event("end", "task_beta")

    tracker.push_provider_event("start", "task_gamma")
    time.sleep(0.2)
    tracker.push_provider_event("end", "task_gamma")

    # Give some time for the worker thread to process events
    time.sleep(1)

    tracker.stop()

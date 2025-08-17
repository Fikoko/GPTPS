import json
import asyncio
from pathlib import Path
from datetime import datetime
from queue import Queue

# Optional C++ detector
try:
    from detector_logger import CPPDetector
except ImportError:
    CPPDetector = None
    print("Warning: detector_logger module not found. C++ acceleration disabled.")

# Load global configuration
from work_process_types.pre_process.load_config import GLOBAL_CONFIG_PATH

class Detector:
    def __init__(self):
        self.config = self._load_global_config()
        self.worker_id = str(self.config["globals"].get("worker_id", ""))
        self.group_id = str(self.config["globals"].get("group_id", ""))

        # Performance parameters
        perf = self.config.get("performance", {})
        self.max_parallel_jobs = perf.get("max_parallel_jobs", 1)
        self.max_memory_gb = perf.get("max_memory_gb", 0)
        self.timeout_seconds = perf.get("timeout_seconds", 0)

        # C++ detector if available
        if CPPDetector:
            self._cpp_detector = CPPDetector()
        else:
            self._cpp_detector = None

        # Queue for parsed tasks to be processed by run_main.py
        self.task_queue = Queue()

    def _load_global_config(self):
        if not GLOBAL_CONFIG_PATH.exists():
            raise FileNotFoundError(f"{GLOBAL_CONFIG_PATH} not found")
        with open(GLOBAL_CONFIG_PATH, "r", encoding="utf-8") as f:
            return json.load(f)

    async def listen_messages(self, message_source):
        """
        Async listener for incoming messages from server service.
        Each message is parsed into a task and pushed into task_queue.
        """
        async for msg_json in message_source:
            task = self._parse_message(msg_json)
            if task:
                self.task_queue.put(task)

    def _parse_message(self, msg_json: str):
        """
        Convert server message into a task object with specifics.
        Returns None if message is irrelevant to this worker.
        """
        # Use C++ detector if available
        if self._cpp_detector:
            if not self._cpp_detector.is_relevant(msg_json, self.worker_id, self.group_id):
                return None
        else:
            # Fallback Python relevance check
            try:
                msg = json.loads(msg_json)
            except json.JSONDecodeError:
                return None
            if msg.get("worker_id") not in [self.worker_id, "all"] and msg.get("group_id") != self.group_id:
                return None

        # Parse task specifics
        msg = json.loads(msg_json)
        task = {
            "task_name": msg.get("task", "unknown_task"),
            "payload": msg.get("payload", {}),
            "received_at": datetime.now().isoformat(),
            "max_parallel_jobs": self.max_parallel_jobs,
            "max_memory_gb": self.max_memory_gb,
            "timeout_seconds": self.timeout_seconds
        }
        return task

# ---------------- Example usage ----------------
async def simulate_messages():
    msgs = [
        '{"worker_id": "42", "group_id": "1", "task": "web_extractor", "payload": {"url": "https://example.com"}}',
        '{"worker_id": "all", "group_id": "1", "task": "general", "payload": {}}',
        '{"worker_id": "99", "group_id": "2", "task": "other_task", "payload": {}}'
    ]
    for m in msgs:
        await asyncio.sleep(1)
        yield m

if __name__ == "__main__":
    detector = Detector()
    asyncio.run(detector.listen_messages(simulate_messages()))
    # Example: print queued tasks
    while not detector.task_queue.empty():
        print(detector.task_queue.get())

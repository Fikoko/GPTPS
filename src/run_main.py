import asyncio
from pathlib import Path
from queue import Queue, Empty
from threading import Thread
from datetime import datetime
import uuid
import time
import heapq
import psutil
from concurrent.futures import ProcessPoolExecutor, as_completed

from work_process_types.pre_process.load_config.load_config import main as load_config_main
from work_process_types.background_process.tracker.tracker import Tracker
from work_process_types.background_process.async_updates.detector import Detector


class MainRunner:
    def __init__(self, max_wait_seconds: float = 1.0):
        # ----------------- Config Choice -----------------
        choice = input("Load configuration automatically? (yes/no): ").strip().lower()
        if choice == "yes":
            self.config = load_config_main(auto_fill=True, interactive=False)
        else:
            self.config = load_config_main(auto_fill=False, interactive=True)

        # ----------------- Tracker -----------------
        self.max_records = self.config.get("performance", {}).get("max_records", 5)
        self.tracker = Tracker(
            analytics_path=Path("analytics.json"),
            session_path=Path("session_config.json"),
            max_records=self.max_records,
            task_types=self.config.get("performance", {}).get("task_types", [])
        )
        print("[Runner] Tracker initialized (analytics.json + session_config.json ready)")

        # ----------------- Detector -----------------
        self.detector = Detector()

        # ----------------- Queues -----------------
        self.seq_queue = Queue()
        self.par_heap = []  # heap: (priority_value, queued_at, task)

        # ----------------- Task Management -----------------
        self._canceled_tasks = set()
        self._task_map = {}  # task_id -> task

        # ----------------- Flags -----------------
        self._stop = False
        self._shutdown_in_progress = False
        self.poll_interval = 0.05  # faster polling

        # ----------------- Executor -----------------
        self.executor = ProcessPoolExecutor(max_workers=psutil.cpu_count())
        self.max_wait_seconds = max_wait_seconds

        # ----------------- Priority Mapping -----------------
        self.priority_order = {"high": 0, "normal": 1, "low": 2}

    # ----------------- Task Execution -----------------
    def process_task(self, task):
        task_id = task.get("task_id")
        task_name = task.get("task_name", "unnamed_task")

        if task_id in self._canceled_tasks:
            print(f"[{task_name}] Task canceled before start")
            return

        mem_limit_gb = task.get("max_memory_gb", 0)
        while mem_limit_gb > 0 and psutil.virtual_memory().available / (1024 ** 3) < mem_limit_gb:
            print(f"[{task_name}] Waiting for memory: {mem_limit_gb:.2f} GB required")
            time.sleep(0.1)
            if self._stop or task_id in self._canceled_tasks:
                print(f"[{task_name}] Task canceled during wait")
                return

        timeout = task.get("timeout_seconds", 0)
        start_time = time.time()
        self.tracker.start_task(task_name)

        print(f"[{task_name}] Processing payload: {task.get('payload')}")
        while True:
            if task_id in self._canceled_tasks:
                print(f"[{task_name}] Task canceled mid-processing")
                self.tracker.end_task(task_name)
                return
            if timeout > 0 and (time.time() - start_time) > timeout:
                print(f"[{task_name}] Task timed out after {timeout} seconds")
                self.tracker.end_task(task_name)
                return
            # Mark task done immediately (no chunk simulation)
            break

        self.tracker.end_task(task_name)
        self._task_map.pop(task_id, None)

    # ----------------- Worker Loop -----------------
    def worker_loop(self):
        while not self._stop:
            # Sequential tasks
            try:
                task = self.seq_queue.get_nowait()
                self.process_task(task)
            except Empty:
                pass

            # Parallel tasks
            batch = []
            temp_heap = []
            now = datetime.now()

            while self.par_heap and len(batch) < psutil.cpu_count():
                priority_value, queued_at, task = heapq.heappop(self.par_heap)
                task_id = task.get("task_id")
                if task_id in self._canceled_tasks:
                    continue

                # Aging check
                if (now - queued_at).total_seconds() > self.max_wait_seconds:
                    priority_value = -1  # promote to highest priority

                mem_required = task.get("max_memory_gb", 0)
                if mem_required > 0 and mem_required > psutil.virtual_memory().available / (1024 ** 3):
                    temp_heap.append((priority_value, queued_at, task))
                    continue

                batch.append(task)

            # push back deferred tasks
            for item in temp_heap:
                heapq.heappush(self.par_heap, item)

            # Submit tasks in parallel
            futures = [self.executor.submit(self.process_task, t) for t in batch]
            for f in as_completed(futures):
                f.result()  # wait for completion

            time.sleep(self.poll_interval)

    # ----------------- Detector Listener -----------------
    async def listen_detector(self):
        async def push_task_to_queue(msg_json):
            import json
            msg = json.loads(msg_json)

            if msg.get("action") == "cancel" and "task_id" in msg:
                self.cancel_task(msg["task_id"])
                return

            if msg.get("action") == "update_priority" and "task_id" in msg and "priority" in msg:
                self.update_task_priority(msg["task_id"], msg["priority"])
                return

            if "priority" not in msg:
                msg["priority"] = "normal"
            queued_at = datetime.now()
            msg["queued_at"] = queued_at
            msg["task_id"] = msg.get("task_id", str(uuid.uuid4()))

            perf_defaults = self.config.get("performance", {})
            msg.setdefault("max_parallel_jobs", perf_defaults.get("max_parallel_jobs", 1))
            msg.setdefault("max_memory_gb", perf_defaults.get("max_memory_gb", 0))
            msg.setdefault("timeout_seconds", perf_defaults.get("timeout_seconds", 0))

            self._task_map[msg["task_id"]] = msg

            if msg.get("max_parallel_jobs", 1) == 1:
                self.seq_queue.put(msg)
            else:
                heapq.heappush(self.par_heap, (self.priority_order.get(msg["priority"], 1), queued_at, msg))

        await self.detector.listen_messages(push_task_to_queue)

    # ----------------- Cancel/Update -----------------
    def cancel_task(self, task_id: str):
        print(f"[Runner] Cancelling task {task_id}")
        self._canceled_tasks.add(task_id)
        self._task_map.pop(task_id, None)

    def update_task_priority(self, task_id: str, new_priority: str):
        print(f"[Runner] Updating priority of task {task_id} to {new_priority}")
        task = self._task_map.get(task_id)
        if task:
            task["priority"] = new_priority

    # ----------------- Shutdown -----------------
    def shutdown(self):
        if self._shutdown_in_progress:
            return
        self._shutdown_in_progress = True
        print("[Runner] Graceful shutdown initiated...")

        self._stop = True

        while not self.seq_queue.empty():
            task = self.seq_queue.get_nowait()
            self.cancel_task(task.get("task_id"))

        while self.par_heap:
            _, _, task = heapq.heappop(self.par_heap)
            self.cancel_task(task.get("task_id"))

        self.executor.shutdown(wait=True)
        print("[Runner] All tasks completed, shutdown finished.")

    # ----------------- Run -----------------
    def run(self):
        worker_thread = Thread(target=self.worker_loop, daemon=True)
        worker_thread.start()

        try:
            asyncio.run(self.listen_detector())
        except KeyboardInterrupt:
            print("KeyboardInterrupt received, stopping runner...")
            self.shutdown()
            worker_thread.join()


if __name__ == "__main__":
    runner = MainRunner(max_wait_seconds=1.0)
    runner.run()

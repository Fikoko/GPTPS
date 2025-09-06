import asyncio
from queue import Queue, Empty
from pathlib import Path
from multiprocessing import Pool, cpu_count
import psutil
from threading import Thread
from datetime import datetime
import uuid

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
            max_records=self.max_records
        )

        # ----------------- Detector -----------------
        self.detector = Detector()

        # ----------------- Queues -----------------
        self.seq_queue = Queue()
        self.par_queue = Queue()

        # ----------------- Task Management -----------------
        self._canceled_tasks = set()
        self._task_map = {}  # task_id -> task

        # ----------------- Flags -----------------
        self._stop = False
        self._shutdown_in_progress = False
        self.poll_interval = 0.1

        # ----------------- Multiprocessing Pool -----------------
        self.pool = Pool(processes=cpu_count())
        self.max_wait_seconds = max_wait_seconds

    # ----------------- Task Execution -----------------
    def process_task(self, task):
        task_id = task.get("task_id")
        task_name = task.get("task_name", "unnamed_task")

        if task_id in self._canceled_tasks:
            print(f"[{task_name}] Task canceled before start")
            return

        # Respect task-specific memory limit
        mem_limit_gb = task.get("max_memory_gb", 0)
        if mem_limit_gb > 0 and psutil.virtual_memory().available / (1024 ** 3) < mem_limit_gb:
            print(f"[{task_name}] Not enough memory ({mem_limit_gb} GB required). Skipping task.")
            return

        self.tracker.start_task(task_name)

        try:
            # Actual task processing goes here
            print(f"[{task_name}] Task started...")
            # If an error occurs, you can raise an exception here
        except Exception as e:
            print(f"[{task_name}] Error during task: {e}")
        finally:
            self.tracker.end_task(task_name)
            self._task_map.pop(task_id, None)
            print(f"[{task_name}] Task ended.")

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
            available_cpus = cpu_count()
            max_batch_size = max(1, available_cpus)
            temp_queue = []

            while len(batch) < max_batch_size:
                try:
                    task = self.par_queue.get_nowait()
                    task_id = task.get("task_id")
                    if task_id in self._canceled_tasks:
                        continue
                    mem_required = task.get("max_memory_gb", 0)
                    if mem_required > 0 and mem_required > psutil.virtual_memory().available / (1024 ** 3):
                        temp_queue.append(task)
                        continue
                    batch.append(task)
                except Empty:
                    break

            for task in temp_queue:
                self.par_queue.put(task)

            if batch:
                self._apply_aging_priority()
                for _ in self.pool.imap(self.process_task, batch):
                    pass

            psutil.sleep(self.poll_interval)

    # ----------------- Aging + Priority Sorting -----------------
    def _apply_aging_priority(self):
        now = datetime.now()
        temp = []
        aged_tasks = []

        while not self.par_queue.empty():
            try:
                task = self.par_queue.get_nowait()
                if task.get("task_id") in self._canceled_tasks:
                    continue
                queued_at = task.get("queued_at", now)
                if isinstance(queued_at, str):
                    queued_at = datetime.fromisoformat(queued_at)
                if (now - queued_at).total_seconds() > self.max_wait_seconds:
                    aged_tasks.append(task)
                else:
                    temp.append(task)
            except Empty:
                break

        combined = aged_tasks + temp
        # Sort: high-priority first, then normal, then low
        priority_order = {"high": 0, "normal": 1, "low": 2}
        combined.sort(key=lambda t: priority_order.get(t.get("priority", "normal"), 1))

        for task in combined:
            self.par_queue.put(task)

    # ----------------- Detector Listener -----------------
    async def listen_detector(self):
        async def push_task_to_queue(msg_json):
            import json
            msg = json.loads(msg_json)

            # Cancellation
            if msg.get("action") == "cancel" and "task_id" in msg:
                self.cancel_task(msg["task_id"])
                return

            # Priority update
            if msg.get("action") == "update_priority" and "task_id" in msg and "priority" in msg:
                self.update_task_priority(msg["task_id"], msg["priority"])
                return

            if "priority" not in msg:
                msg["priority"] = "normal"
            msg["queued_at"] = datetime.now().isoformat()
            msg["task_id"] = msg.get("task_id", str(uuid.uuid4()))

            # Fill in missing performance params from global config defaults
            perf_defaults = self.config.get("performance", {})
            msg.setdefault("max_parallel_jobs", perf_defaults.get("max_parallel_jobs", 1))
            msg.setdefault("max_memory_gb", perf_defaults.get("max_memory_gb", 0))
            msg.setdefault("timeout_seconds", perf_defaults.get("timeout_seconds", 0))

            self._task_map[msg["task_id"]] = msg

            if msg.get("max_parallel_jobs", 1) == 1:
                self.seq_queue.put(msg)
            else:
                self.par_queue.put(msg)

        await self.detector.listen_messages(push_task_to_queue)

    # ----------------- Cancel Task -----------------
    def cancel_task(self, task_id: str):
        print(f"[Runner] Cancelling task {task_id}")
        self._canceled_tasks.add(task_id)
        self._task_map.pop(task_id, None)

    # ----------------- Update Task Priority -----------------
    def update_task_priority(self, task_id: str, new_priority: str):
        print(f"[Runner] Updating priority of task {task_id} to {new_priority}")
        task = self._task_map.get(task_id)
        if task:
            task["priority"] = new_priority

    # ----------------- Graceful Shutdown -----------------
    def shutdown(self):
        if self._shutdown_in_progress:
            return
        self._shutdown_in_progress = True
        print("[Runner] Graceful shutdown initiated...")

        # Stop accepting new tasks
        self._stop = True

        # Cancel queued tasks
        while not self.seq_queue.empty():
            task = self.seq_queue.get_nowait()
            self.cancel_task(task.get("task_id"))

        while not self.par_queue.empty():
            task = self.par_queue.get_nowait()
            self.cancel_task(task.get("task_id"))

        # Close multiprocessing pool
        self.pool.close()
        print("[Runner] Waiting for running tasks to complete...")
        self.pool.join()
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

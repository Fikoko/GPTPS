import asyncio
from queue import Queue, Empty
from threading import Thread
from pathlib import Path
import psutil
import time

# ----------------- Imports -----------------
from work_process_types.pre_process.load_config.load_config import main as load_config_main
from work_process_types.background_process.tracker.tracker import Tracker
from work_process_types.background_process.async_updates.detector import Detector

# ----------------- Main Run Class -----------------
class MainRunner:
    def __init__(self):
        # ----------------- Config Choice -----------------
        choice = input("Load configuration automatically? (yes/no): ").strip().lower()
        if choice == "yes":
            self.config = load_config_main(auto_fill=True, interactive=False)
        else:
            self.config = load_config_main(auto_fill=False, interactive=True)

        # Initialize tracker
        self.tracker = Tracker(
            analytics_path=Path("analytics.json"),
            session_path=Path("session_config.json"),
            max_records=5
        )

        # Initialize detector
        self.detector = Detector()

        # Task queue
        self.task_queue = Queue()

        # Load performance constraints
        self.max_parallel_jobs = self.config["performance"].get("max_parallel_jobs", 1)
        self.max_memory_gb = self.config["performance"].get("max_memory_gb", 0)
        self.timeout_seconds = self.config["performance"].get("timeout_seconds", 0)

        # Graceful stop flag
        self._stop = False

    # ----------------- Task Execution -----------------
    def process_task(self, task):
        """
        Process a single task.
        Hook this to your main_process files later.
        """
        task_name = task.get("task_name", "unnamed_task")

        # Check memory before starting task
        mem_usage_gb = psutil.Process().memory_info().rss / (1024 ** 3)
        if self.max_memory_gb > 0 and mem_usage_gb >= self.max_memory_gb:
            print(f"[Warning] Memory limit reached ({mem_usage_gb:.2f} GB), waiting...")
            while psutil.Process().memory_info().rss / (1024 ** 3) >= self.max_memory_gb:
                time.sleep(0.5)

        self.tracker.start_task(task_name)

        # Placeholder for actual task processing
        print(f"Processing task: {task_name} with payload: {task.get('payload')}")

        self.tracker.end_task(task_name)

    # ----------------- Worker Loop -----------------
    def worker_loop(self):
        """
        Worker loop to process tasks from the queue with batch/sequential logic.
        """
        while not self._stop:
            if self.task_queue.empty():
                time.sleep(0.1)
                continue

            if self.max_parallel_jobs > 1:
                # Batch processing
                batch = []
                for _ in range(self.max_parallel_jobs):
                    try:
                        batch.append(self.task_queue.get_nowait())
                    except Empty:
                        break

                threads = []
                for task in batch:
                    t = Thread(target=self.process_task, args=(task,))
                    t.start()
                    threads.append(t)
                for t in threads:
                    t.join()
            else:
                # Sequential processing
                task = self.task_queue.get()
                self.process_task(task)

            # Delay between batches/tasks
            if self.timeout_seconds > 0:
                time.sleep(self.timeout_seconds)

    # ----------------- Detector Listener -----------------
    async def listen_detector(self):
        async def push_task_to_queue(msg_json):
            import json
            task = json.loads(msg_json)
            self.task_queue.put(task)

        await self.detector.listen_messages(push_task_to_queue)


    # ----------------- Run -----------------
    def run(self):
        # Start worker loop in a separate thread
        worker_thread = Thread(target=self.worker_loop, daemon=True)
        worker_thread.start()

        # Start async detector listener
        try:
            asyncio.run(self.listen_detector())
        except KeyboardInterrupt:
            print("Stopping runner...")
            self._stop = True
            worker_thread.join()

# ----------------- Entry Point -----------------
if __name__ == "__main__":
    runner = MainRunner()
    runner.run()

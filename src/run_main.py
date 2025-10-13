import asyncio
from pathlib import Path
from queue import Queue, Empty
from threading import Thread, Lock
from datetime import datetime
import uuid
import time
import heapq
import psutil
from multiprocessing import Process, Manager

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
        self.manager = Manager()
        self._task_map = self.manager.dict()  # task_id -> {"task": task, "process": Process, "immediate": bool}
        self._canceled_tasks = self.manager.list()
        self._task_lock = Lock()  # Added synchronization

        # ----------------- Flags -----------------
        self._stop = False
        self._shutdown_in_progress = False
        self.poll_interval = 0.05  # faster polling

        # ----------------- Priority Mapping -----------------
        self.priority_order = {"high": 0, "normal": 1, "low": 2}
        self.max_wait_seconds = max_wait_seconds

    # ----------------- Task Execution -----------------
    def process_task(self, task):
        task_id = task.get("task_id")
        task_name = task.get("task_name", "unnamed_task")

        if task_id in self._canceled_tasks:
            print(f"[{task_name}] Task canceled before start")
            return

        mem_limit_gb = task.get("max_memory_gb", 0)
        # Fixed memory check with proper error handling
        while mem_limit_gb > 0:
            try:
                available_gb = psutil.virtual_memory().available / (1024 ** 3)
                if available_gb >= mem_limit_gb:
                    break
                print(f"[{task_name}] Waiting for memory: {mem_limit_gb:.2f} GB required, {available_gb:.2f} GB available")
                time.sleep(0.05)
                if self._stop or task_id in self._canceled_tasks:
                    print(f"[{task_name}] Task canceled during memory wait")
                    return
            except Exception as e:
                print(f"[{task_name}] Memory check failed: {e}")
                break

        timeout = task.get("timeout_seconds", 0)
        start_time = time.time()
        
        try:
            self.tracker.start_task(task_name)
            print(f"[{task_name}] Processing payload: {task.get('payload')}")
            
            # Simulate actual processing - replace with real task execution
            processing_time = min(1.0, timeout if timeout > 0 else 1.0)  # Max 1 second for demo
            time.sleep(processing_time)
            
            # Check for cancellation during processing
            if task_id in self._canceled_tasks:
                print(f"[{task_name}] Task canceled mid-processing")
                self.tracker.end_task(task_name)
                return
            if timeout > 0 and (time.time() - start_time) > timeout:
                print(f"[{task_name}] Task timed out after {timeout} seconds")
                self.tracker.end_task(task_name)
                return

            print(f"[{task_name}] Task completed successfully")
            self.tracker.end_task(task_name)
            
        except Exception as e:
            print(f"[{task_name}] Task processing error: {e}")
            self.tracker.end_task(task_name)
        finally:
            # Only remove from map if not immediate cancel
            with self._task_lock:
                info = self._task_map.get(task_id)
                if info and not info.get("immediate"):
                    self._task_map.pop(task_id, None)

    # ----------------- Worker Loop -----------------
    def worker_loop(self):
        while not self._stop:
            # Sequential tasks
            try:
                task = self.seq_queue.get_nowait()
                try:
                    p = Process(target=self.process_task, args=(task,))
                    task_id = task["task_id"]
                    
                    with self._task_lock:
                        self._task_map[task_id] = {"task": task, "process": p, "immediate": False}
                    
                    p.start()
                    p.join()
                    
                    with self._task_lock:
                        if task_id in self._task_map:
                            self._task_map.pop(task_id, None)
                            
                except Exception as e:
                    print(f"Sequential task process error: {e}")
                    
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
                try:
                    available_gb = psutil.virtual_memory().available / (1024 ** 3)
                    if mem_required > 0 and mem_required > available_gb:
                        temp_heap.append((priority_value, queued_at, task))
                        continue
                except Exception as e:
                    print(f"Memory check error for parallel task: {e}")

                batch.append(task)

            # push back deferred tasks
            for item in temp_heap:
                heapq.heappush(self.par_heap, item)

            # Start parallel tasks
            procs = []
            for t in batch:
                task_id = t["task_id"]
                try:
                    p = Process(target=self.process_task, args=(t,))
                    
                    with self._task_lock:
                        self._task_map[task_id] = {
                            "task": t, 
                            "process": p, 
                            "queued_at": t["queued_at"], 
                            "priority": t["priority"], 
                            "immediate": False
                        }
                    
                    p.start()
                    procs.append((task_id, p))
                    
                except Exception as e:
                    print(f"Parallel task process error: {e}")

            # Wait for parallel tasks to finish
            for task_id, p in procs:
                try:
                    p.join()
                    with self._task_lock:
                        info = self._task_map.get(task_id)
                        if info and not info.get("immediate"):
                            self._task_map.pop(task_id, None)
                except Exception as e:
                    print(f"Process join error: {e}")

            time.sleep(self.poll_interval)

    # ----------------- Message Processing -----------------
    async def listen_server_messages(self):
        """
        Connect to server service and process incoming messages using detector.
        """
        service_name = self.config["globals"].get("server_service_connection")
        service_mode = self.config["globals"].get("server_service_mode")
        
        if service_name and service_mode:
            try:
                from work_process_types.pre_process.server_service.server_service_connector import ServerServiceConnector
                connector = ServerServiceConnector(service_name, service_mode)
                
                if await connector.connect():
                    print(f"Connected to {service_name} in {service_mode} mode")
                    try:
                        # Get supported formats for this service
                        formats = connector.get_supported_formats()
                        primary_format = formats[0] if formats else "json"
                        
                        async for raw_message in connector.listen_messages():
                            if self._stop:
                                break
                                
                            # Use detector to parse and filter the message
                            task = self.detector.parse_and_filter(raw_message, primary_format)
                            
                            if task:
                                await self._process_task_message(task)
                                
                    except Exception as e:
                        print(f"Error processing messages: {e}")
                    finally:
                        await connector.disconnect()
                else:
                    print(f"Failed to connect to {service_name}, using test mode")
                    await self._run_test_mode()
                    
            except Exception as e:
                print(f"Error with server service: {e}")
                print("Falling back to test mode...")
                await self._run_test_mode()
        else:
            print("No server service configured, using test mode...")
            await self._run_test_mode()

    async def _run_test_mode(self):
        """
        Run in test mode with dummy messages.
        """
        test_counter = 0
        while not self._stop:
            await asyncio.sleep(3)
            test_counter += 1
            
            # Create test task
            test_task = {
                "task_name": f"test_task_{test_counter}",
                "payload": {
                    "mode": "testing", 
                    "timestamp": datetime.now().isoformat(),
                    "counter": test_counter
                },
                "priority": ["normal", "high", "low"][test_counter % 3]
            }
            await self._process_task_message(test_task)

    async def _process_task_message(self, task: dict):
        """
        Process a parsed task message from the detector.
        """
        try:
            # Handle control messages
            if task.get("action") == "cancel" and "task_id" in task:
                self.cancel_task(task["task_id"], immediate=False)
                return
            if task.get("action") == "cancel_immediate" and "task_id" in task:
                self.cancel_task(task["task_id"], immediate=True)
                return
            if task.get("action") == "update_priority" and "task_id" in task and "priority" in task:
                self.update_task_priority(task["task_id"], task["priority"])
                return

            # Process regular task
            if "priority" not in task:
                task["priority"] = "normal"
            
            queued_at = datetime.now()
            task["queued_at"] = queued_at
            task["task_id"] = task.get("task_id", str(uuid.uuid4()))

            # Apply performance defaults from config
            perf_defaults = self.config.get("performance", {})
            task.setdefault("max_parallel_jobs", perf_defaults.get("max_parallel_jobs", 1))
            task.setdefault("max_memory_gb", perf_defaults.get("max_memory_gb", 0))
            task.setdefault("timeout_seconds", perf_defaults.get("timeout_seconds", 0))

            # Store task info
            with self._task_lock:
                self._task_map[task["task_id"]] = {
                    "task": task, 
                    "process": None, 
                    "queued_at": queued_at, 
                    "priority": task["priority"], 
                    "immediate": False
                }

            # Queue task based on parallelism
            if task.get("max_parallel_jobs", 1) == 1:
                print(f"[Runner] Queuing sequential task: {task['task_name']}")
                self.seq_queue.put(task)
            else:
                print(f"[Runner] Queuing parallel task: {task['task_name']} (priority: {task['priority']})")
                heapq.heappush(self.par_heap, (self.priority_order.get(task["priority"], 1), queued_at, task))
                
        except Exception as e:
            print(f"Task processing error: {e}")

    # ----------------- Cancel / Priority -----------------
    def cancel_task(self, task_id: str, immediate: bool = False):
        print(f"[Runner] Cancelling task {task_id}, immediate={immediate}")
        
        with self._task_lock:
            info = self._task_map.get(task_id)
            if info:
                proc = info.get("process")
                if immediate and proc and proc.is_alive():
                    try:
                        proc.terminate()
                        proc.join(timeout=1.0)
                        if proc.is_alive():
                            proc.kill()
                    except Exception as e:
                        print(f"Error terminating process: {e}")
                else:
                    # mark task to stop after finish
                    info["immediate"] = True
                    
            if task_id not in self._canceled_tasks:
                self._canceled_tasks.append(task_id)
                
            if immediate and task_id in self._task_map:
                self._task_map.pop(task_id, None)

        # Remove from parallel heap if pending
        self.par_heap = [(pri, qt, t) for pri, qt, t in self.par_heap if t["task_id"] != task_id]
        heapq.heapify(self.par_heap)

    def update_task_priority(self, task_id: str, new_priority: str):
        print(f"[Runner] Updating priority of task {task_id} to {new_priority}")
        
        with self._task_lock:
            info = self._task_map.get(task_id)
            if info:
                info["priority"] = new_priority

        # Update pending tasks in heap
        for i, (pri, qt, t) in enumerate(self.par_heap):
            if t["task_id"] == task_id:
                self.par_heap[i] = (self.priority_order.get(new_priority, 1), qt, t)
        heapq.heapify(self.par_heap)

    # ----------------- Shutdown -----------------
    def shutdown(self):
        if self._shutdown_in_progress:
            return
        self._shutdown_in_progress = True
        print("[Runner] Graceful shutdown initiated...")

        self._stop = True

        while not self.seq_queue.empty():
            try:
                task = self.seq_queue.get_nowait()
                self.cancel_task(task.get("task_id"))
            except Empty:
                break

        while self.par_heap:
            _, _, task = heapq.heappop(self.par_heap)
            self.cancel_task(task.get("task_id"))

        print("[Runner] All tasks completed, shutdown finished.")

    # ----------------- Run -----------------
    def run(self):
        worker_thread = Thread(target=self.worker_loop, daemon=True)
        worker_thread.start()

        try:
            asyncio.run(self.listen_server_messages())
        except KeyboardInterrupt:
            print("KeyboardInterrupt received, stopping runner...")
            self.shutdown()
            worker_thread.join(timeout=5.0)


if __name__ == "__main__":
    runner = MainRunner(max_wait_seconds=1.0)
    runner.run()
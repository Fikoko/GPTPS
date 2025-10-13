import json
from pathlib import Path
from datetime import datetime
from typing import Optional
import io

# Optional C++ detector
try:
    from cpp_detector import CPPDetector
except ImportError:
    CPPDetector = None
    print("Warning: cpp_detector module not found. C++ acceleration disabled.")

# Python fallbacks
try:
    import msgpack
except ImportError:
    msgpack = None

try:
    import fastavro
except ImportError:
    fastavro = None

try:
    from google.protobuf.message import DecodeError
except ImportError:
    DecodeError = Exception

# Load global configuration
from work_process_types.pre_process.load_config.load_config import GLOBAL_CONFIG_PATH


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

    def _load_global_config(self):
        try:
            if not GLOBAL_CONFIG_PATH.exists():
                print(f"Warning: {GLOBAL_CONFIG_PATH} not found, using default config")
                return self._get_default_config()
            
            with open(GLOBAL_CONFIG_PATH, "r", encoding="utf-8") as f:
                config = json.load(f)
                # Ensure required structure exists
                config.setdefault("globals", {})
                config.setdefault("performance", {})
                return config
        except Exception as e:
            print(f"Error loading config: {e}, using defaults")
            return self._get_default_config()

    def _get_default_config(self):
        return {
            "globals": {
                "worker_id": "",
                "group_id": ""
            },
            "performance": {
                "max_parallel_jobs": 1,
                "max_memory_gb": 0,
                "timeout_seconds": 0
            }
        }

    def parse_and_filter(self, message: str, fmt: str = "json", 
                        schema=None, proto_cls=None) -> Optional[dict]:
        """
        Parse and filter a single message. Returns task dict if relevant, None if not.
        This is now a synchronous method that just processes one message at a time.
        """
        return self._parse_message(message, fmt, schema, proto_cls)

    def _parse_message(self, message, fmt: str, schema=None, proto_cls=None):
        """
        Convert server message into a task object.
        Returns None if message is irrelevant to this worker.
        """
        try:
            # ---- C++ accelerated formats ----
            if self._cpp_detector and fmt in {"json", "yaml", "toml", "xml", "msgpack"}:
                if not self._cpp_detector.is_relevant(message, self.worker_id, self.group_id, fmt):
                    return None
                msg = self._cpp_detector.extract(message, fmt)

            # ---- Python fallbacks ----
            else:
                if fmt == "json":
                    try:
                        msg = json.loads(message)
                    except json.JSONDecodeError as e:
                        print(f"JSON decode error: {e}")
                        return None

                elif fmt == "msgpack":
                    if not msgpack:
                        print("msgpack not available")
                        return None
                    try:
                        msg = msgpack.unpackb(message, raw=False)
                    except Exception as e:
                        print(f"msgpack decode error: {e}")
                        return None

                elif fmt == "avro":
                    if not fastavro or not schema:
                        print("fastavro not available or schema missing")
                        return None
                    try:
                        msg = fastavro.schemaless_reader(io.BytesIO(message), schema)
                    except Exception as e:
                        print(f"avro decode error: {e}")
                        return None

                elif fmt == "protobuf":
                    if not proto_cls:
                        print("protobuf message class missing")
                        return None
                    try:
                        msg_obj = proto_cls()
                        msg_obj.ParseFromString(message)
                        msg = {
                            "worker_id": getattr(msg_obj, "worker_id", ""),
                            "group_id": getattr(msg_obj, "group_id", ""),
                            "task": getattr(msg_obj, "task", ""),
                            "payload": getattr(msg_obj, "payload", {}),
                        }
                    except (DecodeError, Exception) as e:
                        print(f"protobuf decode error: {e}")
                        return None

                else:
                    print(f"Unsupported format: {fmt}")
                    return None

                # Python relevance check
                msg_worker = msg.get("worker_id", "")
                msg_group = msg.get("group_id", "")
                if (msg_worker not in [self.worker_id, "all"] and 
                    (not self.group_id or msg_group != self.group_id)):
                    return None

            # ---- Build task ----
            task = {
                "task_name": msg.get("task", "unknown_task"),
                "payload": msg.get("payload", {}),
                "received_at": datetime.now().isoformat(),
                "max_parallel_jobs": self.max_parallel_jobs,
                "max_memory_gb": self.max_memory_gb,
                "timeout_seconds": self.timeout_seconds,
                "priority": msg.get("priority", "normal")
            }
            return task
            
        except Exception as e:
            print(f"Error parsing message: {e}")
            return None
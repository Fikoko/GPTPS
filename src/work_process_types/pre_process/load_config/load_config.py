import json
import platform
import psutil
from pathlib import Path
from datetime import datetime
import asyncio
import os
import tempfile

# Optional GPU detection
try:
    import torch
except ImportError:
    torch = None

# Import C++ extension
try:
    import config_helper
except ImportError:
    config_helper = None
    print("Warning: config_helper module not found. C++ acceleration disabled.")

# Server service connector
try:
    from work_process_types.pre_process.server_service import server_service_connector
except ImportError:
    server_service_connector = None
    print("Warning: server_service_connector module not found.")

GLOBAL_CONFIG_PATH = Path(__file__).parent / "global_config.json"
HARDWARE_PATTERNS_PATH = Path(__file__).parent / "hardware_patterns.json"
RESOURCE_DIR = Path(__file__).parent.parent / "resource"
ANALYTICS_PATH = Path("analytics.json")
SESSION_PATH = Path("session_config.json")

# ---------------- Safe JSON save ----------------
def safe_save_json(path: Path, data: dict):
    """Write JSON safely using an atomic replace."""
    temp_fd, temp_path = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(temp_fd, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=2)
        os.replace(temp_path, path)  # atomic replace
    except Exception:
        os.remove(temp_path)
        raise

# ---------------- Hardware Detection ----------------
def detect_hardware():
    cpu = platform.processor() or "unknown"
    ram_gb = round(psutil.virtual_memory().total / (1024**3))
    gpu_list = []
    if torch and torch.cuda.is_available():
        gpu_list = [torch.cuda.get_device_name(i) for i in range(torch.cuda.device_count())]
    return {"cpu": cpu, "ram_gb": ram_gb, "gpu": gpu_list}

def load_hardware_patterns():
    if not HARDWARE_PATTERNS_PATH.exists():
        return {}
    with open(HARDWARE_PATTERNS_PATH, "r", encoding="utf-8") as f:
        return json.load(f)

def apply_hardware_pattern(hardware_info, patterns):
    if config_helper:
        return config_helper.match_hardware_pattern(hardware_info, patterns)
    for pattern in patterns.get("patterns", []):
        cond = pattern.get("conditions", {})
        match_cpu = not cond.get("cpu") or cond["cpu"].lower() in hardware_info["cpu"].lower()
        match_ram = not cond.get("ram_gb") or hardware_info["ram_gb"] >= cond["ram_gb"]
        match_gpu = not cond.get("gpu") or set(cond["gpu"]).issubset(set(hardware_info["gpu"]))
        if match_cpu and match_ram and match_gpu:
            return pattern.get("defaults", {})
    return {}

# ---------------- User Prompt ----------------
def prompt_user_for_globals(defaults, performance_defaults=None):
    print("\nManual configuration mode: fill values (press Enter to skip, defaults applied)\n")
    globals_conf = {}

    # Workspace paths
    workspace_keys = [
        "input_workspace_path_pic","input_workspace_path_sound","input_workspace_path_text",
        "input_workspace_path_video","input_workspace_path_other",
        "output_workspace_path_pic","output_workspace_path_sound","output_workspace_path_text",
        "output_workspace_path_video","output_workspace_path_other"
    ]
    for key in workspace_keys:
        default_path = defaults.get(key, str(RESOURCE_DIR / key.split("_")[-1]))
        val = input(f"{key} [{default_path}]: ").strip()
        globals_conf[key] = val or default_path

    # Service connection
    service_keys = ["server_service_connection", "server_service_mode"]
    for key in service_keys:
        val = input(f"{key} [{defaults.get(key, '')}]: ").strip()
        globals_conf[key] = val or defaults.get(key, "")

    globals_conf["worker_id"] = input("worker_id []: ").strip() or ""
    globals_conf["group_id"] = input("group_id []: ").strip() or ""

    # LLM
    llm_choice = input("LLM types (comma separated: local_model, cloud_model) [{}]: ".format(
        ",".join(defaults.get("llm_type", []))
    )).strip()
    globals_conf["llm_type"] = [x.strip() for x in llm_choice.split(",") if x.strip()] or defaults.get("llm_type", [])

    # Tasks
    task_input = input("Enter task names (comma separated) [{}]: ".format(
        ",".join(defaults.get("tasks", []))
    )).strip()
    globals_conf["tasks"] = [t.strip() for t in task_input.split(",") if t.strip()] or defaults.get("tasks", [])

    # Performance parameters
    perf = performance_defaults or {}
    # max_records
    default_max_records = perf.get("max_records", 5)
    while True:
        val = input(f"Max records to store in analytics [{default_max_records}]: ").strip()
        if not val:
            globals_conf["max_records"] = default_max_records
            break
        if val.isdigit() and int(val) > 0:
            globals_conf["max_records"] = int(val)
            break
        print("Please enter a positive integer.")

    # max_parallel_jobs
    default_parallel = perf.get("max_parallel_jobs", 1)
    while True:
        val = input(f"Max parallel jobs [{default_parallel}]: ").strip()
        if not val:
            globals_conf["max_parallel_jobs"] = default_parallel
            break
        if val.isdigit() and int(val) >= 0:
            globals_conf["max_parallel_jobs"] = int(val)
            break
        print("Please enter a non-negative integer.")

    # max_memory_gb
    default_mem = perf.get("max_memory_gb", 0)
    while True:
        val = input(f"Max memory per task in GB [{default_mem}]: ").strip()
        if not val:
            globals_conf["max_memory_gb"] = default_mem
            break
        try:
            mem_val = float(val)
            if mem_val >= 0:
                globals_conf["max_memory_gb"] = mem_val
                break
        except:
            pass
        print("Please enter a non-negative number.")

    # timeout_seconds
    default_timeout = perf.get("timeout_seconds", 0)
    while True:
        val = input(f"Timeout per task in seconds [{default_timeout}]: ").strip()
        if not val:
            globals_conf["timeout_seconds"] = default_timeout
            break
        if val.isdigit() and int(val) >= 0:
            globals_conf["timeout_seconds"] = int(val)
            break
        print("Please enter a non-negative integer.")

    return globals_conf

# ---------------- Resource Folders ----------------
def create_resource_subdirs(paths=None):
    if paths is None:
        paths = [
            "input_pic", "input_sound", "input_text", "input_video", "input_other",
            "output_pic", "output_sound", "output_text", "output_video", "output_other"
        ]
        paths = [str(RESOURCE_DIR / p) for p in paths]
    if config_helper:
        config_helper.create_resource_folders(paths)
    else:
        for p in paths:
            Path(p).mkdir(parents=True, exist_ok=True)

# ---------------- Analytics / Session ----------------
def init_analytics(tasks: list, max_records: int = 5, analytics_path=ANALYTICS_PATH):
    analytics_data = {task: [{"start_time": "", "end_time": "", "duration_seconds": ""} 
                             for _ in range(max_records)] for task in tasks}
    safe_save_json(analytics_path, analytics_data)

def init_session_config(tasks: list, session_path=SESSION_PATH):
    session_data = {"session_start": datetime.now().isoformat(), "tasks": {task: [] for task in tasks}}
    safe_save_json(session_path, session_data)

# ---------------- Main ----------------
def main(auto_fill=True, interactive=True):
    hardware_info = detect_hardware()
    patterns = load_hardware_patterns()
    defaults = apply_hardware_pattern(hardware_info, patterns)

    create_resource_subdirs()

    config = {
        "os": platform.system(),
        "hardware": hardware_info,
        "tasks": defaults.get("tasks", []),
        "globals": {},
        "file_handling": defaults.get("file_handling", {}),
        "performance": defaults.get("performance", {}),
        "local_model": defaults.get("local_model", {}),
        "cloud_model": defaults.get("cloud_model", {}),
        "api_keys": {}
    }

    if auto_fill:
        config["globals"] = defaults.get("globals", {}).copy()
        workspace_keys = [
            "input_workspace_path_pic","input_workspace_path_sound","input_workspace_path_text",
            "input_workspace_path_video","input_workspace_path_other",
            "output_workspace_path_pic","output_workspace_path_sound","output_workspace_path_text",
            "output_workspace_path_video","output_workspace_path_other"
        ]
        for k in workspace_keys:
            if k not in config["globals"]:
                config["globals"][k] = str(RESOURCE_DIR / k.split("_")[-1])
        config["globals"]["worker_id"] = ""
        config["globals"]["group_id"] = ""

    performance_defaults = defaults.get("performance", {})
    if interactive:
        user_conf = prompt_user_for_globals(config["globals"], performance_defaults)
        config["globals"].update(user_conf)
        config["tasks"] = user_conf.get("tasks", config["tasks"])
        # set all performance parameters from manual input
        config["performance"]["max_records"] = user_conf.get("max_records", performance_defaults.get("max_records", 5))
        config["performance"]["max_parallel_jobs"] = user_conf.get("max_parallel_jobs", performance_defaults.get("max_parallel_jobs", 1))
        config["performance"]["max_memory_gb"] = user_conf.get("max_memory_gb", performance_defaults.get("max_memory_gb", 0))
        config["performance"]["timeout_seconds"] = user_conf.get("timeout_seconds", performance_defaults.get("timeout_seconds", 0))
    else:
        config["performance"] = performance_defaults

    max_records = config["performance"]["max_records"]
    tasks = config["tasks"]
    init_analytics(tasks, max_records)
    init_session_config(tasks)

    safe_save_json(GLOBAL_CONFIG_PATH, config)
    print(f"\nGlobal configuration saved at {GLOBAL_CONFIG_PATH}")

    # Server service configuration is saved for runtime use
    service_name = config["globals"].get("server_service_connection")
    service_mode = config["globals"].get("server_service_mode")
    if service_name and service_mode:
        print(f"[Server Service] Configuration saved: {service_name} in {service_mode} mode")
        print("[Server Service] Connection will be established at runtime")
    else:
        print("[Server Service] No service configured - will run in test mode")

# ---------------- Entry Point ----------------
if __name__ == "__main__":
    choice = ""
    while choice not in ["1", "2"]:
        print("Choose configuration mode:")
        print("1) Auto-fill (detect hardware and fill defaults)")
        print("2) Manual fill (enter info manually)")
        choice = input("Enter 1 or 2: ").strip()

    if choice == "1":
        main(auto_fill=True, interactive=True)
    else:
        main(auto_fill=False, interactive=True)

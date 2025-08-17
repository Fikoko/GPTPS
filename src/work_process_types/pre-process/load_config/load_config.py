import json
import platform
import psutil
from pathlib import Path
from datetime import datetime
import asyncio

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

# Import server service connector
try:
    from work_process_types.pre_process.server_service import server_service_connector
except ImportError:
    server_service_connector = None
    print("Warning: server_service_connector module not found.")

GLOBAL_CONFIG_PATH = Path(__file__).parent / "global_config.json"
HARDWARE_PATTERNS_PATH = Path(__file__).parent / "hardware_patterns.json"
RESOURCE_DIR = Path(__file__).parent.parent / "resource"

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
    # fallback Python matching
    for pattern in patterns.get("patterns", []):
        cond = pattern.get("conditions", {})
        match_cpu = not cond.get("cpu") or cond["cpu"].lower() in hardware_info["cpu"].lower()
        match_ram = not cond.get("ram_gb") or hardware_info["ram_gb"] >= cond["ram_gb"]
        match_gpu = not cond.get("gpu") or set(cond["gpu"]).issubset(set(hardware_info["gpu"]))
        if match_cpu and match_ram and match_gpu:
            return pattern.get("defaults", {})
    return {}

# ---------------- User Prompt for Globals ----------------
def prompt_user_for_globals(defaults):
    print("\nManual configuration mode: fill values (press Enter to skip, defaults will be applied automatically)\n")
    globals_conf = {}

    workspace_keys = [
        "input_workspace_path_pic", "input_workspace_path_sound", "input_workspace_path_text",
        "input_workspace_path_video", "input_workspace_path_other",
        "output_workspace_path_pic", "output_workspace_path_sound", "output_workspace_path_text",
        "output_workspace_path_video", "output_workspace_path_other"
    ]
    for key in workspace_keys:
        default_path = defaults.get(key, str(RESOURCE_DIR / key.split("_")[-1]))
        val = input(f"{key} [{default_path}]: ").strip()
        globals_conf[key] = val or default_path

    service_keys = ["server_service_connection", "server_service_mode"]
    for key in service_keys:
        val = input(f"{key} [{defaults.get(key, '')}]: ").strip()
        globals_conf[key] = val or defaults.get(key, "")

    # Worker and group IDs (manual input)
    globals_conf["worker_id"] = input("worker_id []: ").strip() or ""
    globals_conf["group_id"] = input("group_id []: ").strip() or ""

    # LLM selection
    llm_choice = input("LLM types (comma separated: local_model, cloud_model) [{}]: ".format(
        ",".join(defaults.get("llm_type", []))
    )).strip()
    globals_conf["llm_type"] = [x.strip() for x in llm_choice.split(",") if x.strip()] or defaults.get("llm_type", [])

    # API keys for cloud models if selected
    globals_conf["api_keys"] = {}
    if "cloud_model" in globals_conf["llm_type"]:
        cloud_key = ""
        while not cloud_key:
            cloud_key = input("Enter API key for cloud model (required): ").strip()
        globals_conf["api_keys"]["cloud_model"] = cloud_key

    # Manual tasks entry
    task_input = input("Enter task names (comma separated) [{}]: ".format(
        ",".join(defaults.get("tasks", []))
    )).strip()
    globals_conf["tasks"] = [t.strip() for t in task_input.split(",") if t.strip()] or defaults.get("tasks", [])

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

    # Auto-fill globals with hardware defaults
    if auto_fill:
        config["globals"] = defaults.get("globals", {}).copy()
        # Ensure all workspace keys exist
        workspace_keys = [
            "input_workspace_path_pic","input_workspace_path_sound","input_workspace_path_text",
            "input_workspace_path_video","input_workspace_path_other",
            "output_workspace_path_pic","output_workspace_path_sound","output_workspace_path_text",
            "output_workspace_path_video","output_workspace_path_other"
        ]
        for k in workspace_keys:
            if k not in config["globals"]:
                config["globals"][k] = str(RESOURCE_DIR / k.split("_")[-1])
        # Always initialize IDs
        config["globals"]["worker_id"] = ""
        config["globals"]["group_id"] = ""

    # Manual override / user prompt
    if interactive:
        user_conf = prompt_user_for_globals(config["globals"])
        config["globals"].update(user_conf)
        config["tasks"] = user_conf.get("tasks", config["tasks"])

    # Save global config
    with open(GLOBAL_CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(config, f, indent=2)
    print(f"\nGlobal configuration saved at {GLOBAL_CONFIG_PATH}")

    # ---------------- Server Service Connection ----------------
    service_name = config["globals"].get("server_service_connection")
    service_mode = config["globals"].get("server_service_mode")
    if server_service_connector and service_name and service_mode:
        try:
            connector = server_service_connector.ServerServiceConnector(service_name, service_mode)
            asyncio.run(connector.connect())
            print("[Server Service] Connector executed successfully.")
        except Exception as e:
            print(f"[Server Service] Error executing connector: {e}")
    else:
        print("[Server Service] Skipped connection (service_name or mode not specified)")

    return config

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

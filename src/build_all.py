
import subprocess
import os

# List of directories containing setup.py
ext_dirs = [
    "work_process_types/pre_process/load_config",
    "work_process_types/background_process/tracker",
    "work_process_types/background_process/async_updates"
]

for d in ext_dirs:
    print(f"Building C++ extension in {d}...")
    subprocess.run(["python", "setup.py", "build_ext", "--inplace"], cwd=d, check=True)

print("All C++ extensions built successfully!")


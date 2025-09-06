
# Worker Task Processing System

## Overview
This project implements a **worker-based task processing system** that listens for incoming tasks from a server service, queues them, and executes them based on performance constraints. The system supports both **sequential** and **batch (parallel)** task execution, tracks analytics, and allows manual or automatic configuration of the worker environment.

---

## Features

- **Automatic hardware detection**  
  Detects CPU, RAM, and GPU to apply pre-defined performance patterns.

- **Flexible task execution**  
  - **Sequential execution**: One task at a time (if `max_parallel_jobs = 1`)  
  - **Batch execution**: Multiple tasks simultaneously (if `max_parallel_jobs > 1`)  

- **Performance control**  
  Tasks respect global configuration parameters:
  - `max_parallel_jobs`
  - `max_memory_gb`
  - `timeout_seconds`

- **Task analytics**  
  Tracks task start and end times, memory usage, and execution statistics via the `Tracker` module.

- **Detector integration**  
  - Listens for messages from the server service
  - Filters messages based on worker/group ID
  - Queues relevant tasks for execution

- **Manual or auto configuration**  
  - Auto-fill based on detected hardware
  - Manual override for tasks, paths, and IDs
  - Supports defining multiple tasks per hardware pattern

---

---

## Getting Started (in demostatics-worker_env)

### 1. Install dependencies (Python dependencies)
```bash
pip install -r requirements.txt
```

### 2. Install builds/extensions
```bash
cd src
python build_all.py
```

## 3. Install submodules
```bash
git clone --recurse-submodules https://github.com/Demostatics/demostatics-worker_env.git
```

If you already cloned the repo before submodules were added use "git submodule update --init --recursive"

Note: The folders `ML/extractnet` and `ML/ache` are submodules. They track external repositories. Updating them can be done via:

"git submodule update --remote --merge"


## ACHE system requirements (not Python packages)

java>=1.8

maven>=3.5

git

docker   # optional so you don’t have to worry about Java/Maven locally

python>=3.8   # optional, for integration




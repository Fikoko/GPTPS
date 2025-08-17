
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

## Getting Started

### 1. Install dependencies
```bash
pip install -r requirements.txt







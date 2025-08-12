
import json
import os
import sys

CONFIG_FILE = 'global_config.json'

def load_config(path=CONFIG_FILE):
    if not os.path.exists(path):
        print(f"Error: Config file '{path}' not found.")
        sys.exit(1)

    with open(path, 'r') as f:
        try:
            return json.load(f)
        except json.JSONDecodeError:
            print(f"Error: Config file contains invalid JSON.")
            sys.exit(1)

def unpack_config(config: dict):
    """Unpacks nested config into flat variables using locals()."""
    
    # Unpack top-level keys directly
    globals().update(config)

if __name__ == "__main__":
    config = load_config()
    unpack_config(config)





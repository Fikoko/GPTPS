import asyncio
import json
from pathlib import Path

# Default config path
CONFIG_PATH = Path(__file__).parent / "server_service_config.json"

class ServerServiceConnector:
    def __init__(self, service_name, mode, config_path=CONFIG_PATH, log_to_console=True):
        self.service_name = service_name.lower()
        self.mode = mode.lower()
        self.config_path = Path(config_path)
        self.log_to_console = log_to_console
        self.config = self._load_config()
        self.active_services = {}
        self.message_queue = asyncio.Queue()

        # Load only the selected service and mode
        if self.service_name not in self.config or self.mode not in self.config[self.service_name]:
            raise ValueError(f"Service '{self.service_name}' or mode '{self.mode}' not found in config")
        self.service_cfg = self.config[self.service_name][self.mode]

        # Prompt user for any missing keys
        self._prompt_missing_keys()

    def _load_config(self):
        if not self.config_path.exists():
            raise FileNotFoundError(f"{self.config_path} not found")
        with open(self.config_path, "r", encoding="utf-8") as f:
            return json.load(f)

    def _prompt_missing_keys(self):
        updated = False
        for key, val in self.service_cfg.items():
            if val in ("", None, [], {}):
                user_input = input(f"Enter {self.service_name} {self.mode} key '{key}': ").strip()
                self.service_cfg[key] = user_input
                updated = True
        if updated:
            # Save updated config back to file
            self.config[self.service_name][self.mode] = self.service_cfg
            with open(self.config_path, "w", encoding="utf-8") as f:
                json.dump(self.config, f, indent=2)
            if self.log_to_console:
                print(f"[{self.service_name}] {self.mode} configuration updated in server_service_config.json")

    async def connect(self):
        """
        Placeholder for actual connection logic.
        Replace this with Ably, MQTT, Firebase, PubNub, Pusher, Socket.IO connections.
        """
        await self.message_queue.put((self.service_name, self.mode, "Connected"))
        if self.log_to_console:
            print(f"[{self.service_name}] {self.mode} connected successfully")

# Example usage
if __name__ == "__main__":
    service = input("Enter server service name (ably/mqtt/firebase/pubnub/pusher/socketio): ").strip()
    mode = input("Enter mode (prod/dev): ").strip()
    connector = ServerServiceConnector(service, mode)
    asyncio.run(connector.connect())

import asyncio
import json
from pathlib import Path
import os
import tempfile
from typing import AsyncGenerator, Optional

# Optional service imports
try:
    import paho.mqtt.client as mqtt
except ImportError:
    mqtt = None

try:
    import firebase_admin
    from firebase_admin import credentials, db
except ImportError:
    firebase_admin = None

try:
    import socketio
except ImportError:
    socketio = None

try:
    import websockets
    import aiohttp
except ImportError:
    websockets = None
    aiohttp = None

# Default config path
CONFIG_PATH = Path(__file__).parent / "server_service_config.json"

# ---------------- Safe JSON save ----------------
def safe_save_json(path: Path, data: dict):
    temp_fd, temp_path = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(temp_fd, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=2)
        os.replace(temp_path, path)
    except Exception:
        os.remove(temp_path)
        raise

# ---------------- Server Connector ----------------
class ServerServiceConnector:
    def __init__(self, service_name, mode, config_path=CONFIG_PATH, log_to_console=True):
        self.service_name = service_name.lower()
        self.mode = mode.lower()
        self.config_path = Path(config_path)
        self.log_to_console = log_to_console
        self.config = self._load_config()
        
        # Connection state
        self.connected = False
        self.message_queue = asyncio.Queue()
        
        if self.service_name not in self.config or self.mode not in self.config[self.service_name]:
            raise ValueError(f"Service '{self.service_name}' or mode '{self.mode}' not found in config")
        
        self.service_cfg = self.config[self.service_name][self.mode]
        
        # Ensure serialization_formats exists
        if "serialization_formats" not in self.service_cfg:
            self.service_cfg["serialization_formats"] = ["json"]
        
        self._prompt_missing_keys()
        
        # Service-specific clients
        self.mqtt_client = None
        self.socketio_client = None
        self.websocket = None
        self.http_session = None

    def _load_config(self):
        if not self.config_path.exists():
            raise FileNotFoundError(f"{self.config_path} not found")
        with open(self.config_path, "r", encoding="utf-8") as f:
            return json.load(f)

    def _prompt_missing_keys(self):
        updated = False
        for key, val in self.service_cfg.items():
            if key == "serialization_formats":
                continue
                
            if val in ("", None, [], {}):
                user_input = input(f"Enter {self.service_name} {self.mode} key '{key}': ").strip()
                
                if key in ["port"]:
                    try:
                        user_input = int(user_input)
                    except ValueError:
                        user_input = 8080
                elif key in ["channels", "topics"]:
                    user_input = [item.strip() for item in user_input.split(",") if item.strip()]
                elif key == "serialization_formats":
                    user_input = [fmt.strip() for fmt in user_input.split(",") if fmt.strip()]
                    
                self.service_cfg[key] = user_input
                updated = True

        if updated:
            self.config[self.service_name][self.mode] = self.service_cfg
            safe_save_json(self.config_path, self.config)
            if self.log_to_console:
                print(f"[{self.service_name}] {self.mode} configuration updated")

    async def connect(self) -> bool:
        """Connect to the configured service"""
        try:
            if self.service_name == "mqtt":
                return await self._connect_mqtt()
            elif self.service_name == "firebase":
                return await self._connect_firebase()
            elif self.service_name == "socketio":
                return await self._connect_socketio()
            elif self.service_name == "custom_server":
                return await self._connect_custom_server()
            else:
                if self.log_to_console:
                    print(f"Service {self.service_name} not implemented yet")
                return False
                
        except Exception as e:
            if self.log_to_console:
                print(f"Connection error: {e}")
            return False

    async def _connect_mqtt(self) -> bool:
        if not mqtt:
            raise ImportError("paho-mqtt not installed")
            
        self.mqtt_client = mqtt.Client()
        
        if self.service_cfg.get("username") and self.service_cfg.get("password"):
            self.mqtt_client.username_pw_set(
                self.service_cfg["username"], 
                self.service_cfg["password"]
            )
            
        self.mqtt_client.on_connect = self._on_mqtt_connect
        self.mqtt_client.on_message = self._on_mqtt_message
        
        host = self.service_cfg.get("host", "localhost")
        port = int(self.service_cfg.get("port", 1883))
        
        await asyncio.get_event_loop().run_in_executor(
            None, self.mqtt_client.connect, host, port, 60
        )
        
        self.mqtt_client.loop_start()
        self.connected = True
        
        if self.log_to_console:
            print(f"Connected to MQTT broker at {host}:{port}")
        return True

    def _on_mqtt_connect(self, client, userdata, flags, rc):
        if rc == 0:
            topics = self.service_cfg.get("topics", ["default"])
            for topic in topics:
                client.subscribe(topic)
                if self.log_to_console:
                    print(f"Subscribed to MQTT topic: {topic}")

    def _on_mqtt_message(self, client, userdata, msg):
        try:
            message = msg.payload.decode('utf-8')
            asyncio.create_task(self.message_queue.put(message))
        except Exception as e:
            if self.log_to_console:
                print(f"Error processing MQTT message: {e}")

    async def _connect_firebase(self) -> bool:
        if not firebase_admin:
            raise ImportError("firebase-admin not installed")
            
        cred_path = self.service_cfg.get("credentials_path")
        if not cred_path or not Path(cred_path).exists():
            raise FileNotFoundError("Firebase credentials file not found")
            
        cred = credentials.Certificate(cred_path)
        firebase_admin.initialize_app(cred, {
            'databaseURL': self.service_cfg["database_url"]
        })
        
        self.db_ref = db.reference('/messages')
        self.db_ref.listen(self._on_firebase_message)
        self.connected = True
        
        if self.log_to_console:
            print("Connected to Firebase")
        return True

    def _on_firebase_message(self, event):
        try:
            if event.data:
                message = json.dumps(event.data) if isinstance(event.data, dict) else str(event.data)
                asyncio.create_task(self.message_queue.put(message))
        except Exception as e:
            if self.log_to_console:
                print(f"Error processing Firebase message: {e}")

    async def _connect_socketio(self) -> bool:
        if not socketio:
            raise ImportError("python-socketio not installed")
            
        self.socketio_client = socketio.AsyncClient()
        
        @self.socketio_client.event
        async def connect():
            self.connected = True
            if self.log_to_console:
                print("Connected to Socket.IO server")
                
        @self.socketio_client.event
        async def message(data):
            message = json.dumps(data) if isinstance(data, dict) else str(data)
            await self.message_queue.put(message)
            
        await self.socketio_client.connect(self.service_cfg["url"])
        return True

    async def _connect_custom_server(self) -> bool:
        protocol = self.service_cfg.get("protocol", "websocket").lower()
        
        if protocol == "websocket":
            if not websockets:
                raise ImportError("websockets not installed")
                
            host = self.service_cfg.get("host", "localhost")
            port = self.service_cfg.get("port", 8080)
            endpoint = self.service_cfg.get("endpoint", "/ws")
            
            ws_url = f"ws://{host}:{port}{endpoint}"
            self.websocket = await websockets.connect(ws_url)
            self.connected = True
            
            if self.log_to_console:
                print(f"Connected to WebSocket server at {ws_url}")
            return True
            
        elif protocol in ["http", "https"]:
            if not aiohttp:
                raise ImportError("aiohttp not installed")
                
            self.http_session = aiohttp.ClientSession()
            self.connected = True
            
            if self.log_to_console:
                print("HTTP polling mode activated")
            return True
            
        return False

    async def listen_messages(self) -> AsyncGenerator[str, None]:
        """Listen for messages from the connected service"""
        if not self.connected:
            raise RuntimeError("Not connected. Call connect() first.")
            
        if self.service_name in ["mqtt", "firebase", "socketio"]:
            # For services that push messages to the queue
            while self.connected:
                try:
                    message = await asyncio.wait_for(self.message_queue.get(), timeout=1.0)
                    yield message
                except asyncio.TimeoutError:
                    continue
                    
        elif self.service_name == "custom_server":
            if self.websocket:
                # WebSocket listening
                async for message in self.websocket:
                    yield str(message)
            elif self.http_session:
                # HTTP polling
                host = self.service_cfg.get("host", "localhost")
                port = self.service_cfg.get("port", 8080)
                endpoint = self.service_cfg.get("endpoint", "/messages")
                url = f"http://{host}:{port}{endpoint}"
                
                while self.connected:
                    try:
                        async with self.http_session.get(url) as response:
                            if response.status == 200:
                                data = await response.json()
                                if data:
                                    yield json.dumps(data)
                    except Exception as e:
                        if self.log_to_console:
                            print(f"HTTP polling error: {e}")
                    await asyncio.sleep(1)

    async def disconnect(self):
        """Disconnect from the service"""
        self.connected = False
        
        if self.mqtt_client:
            self.mqtt_client.loop_stop()
            self.mqtt_client.disconnect()
            
        if self.socketio_client:
            await self.socketio_client.disconnect()
            
        if self.websocket:
            await self.websocket.close()
            
        if self.http_session:
            await self.http_session.close()
            
        if self.log_to_console:
            print(f"Disconnected from {self.service_name}")

    def get_supported_formats(self) -> list:
        """Get supported serialization formats"""
        return self.service_cfg.get("serialization_formats", ["json"])


# ---------------- Entry Point ----------------
if __name__ == "__main__":
    async def main():
        service = input("Enter service (mqtt/firebase/socketio/custom_server): ").strip()
        mode = input("Enter mode (prod/dev): ").strip()
        
        connector = ServerServiceConnector(service, mode)
        
        if await connector.connect():
            print("Connected! Listening for messages...")
            try:
                async for message in connector.listen_messages():
                    print(f"Received: {message}")
            except KeyboardInterrupt:
                print("\nStopping...")
            finally:
                await connector.disconnect()
        else:
            print("Connection failed!")
    
    asyncio.run(main())
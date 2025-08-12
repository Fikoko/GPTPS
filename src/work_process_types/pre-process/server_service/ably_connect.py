
import asyncio
import ably
from ably import AblyRealtime

ABLY_API_KEY = 'JUmndQ.HCyHsw:BMY0kt95t1F61TMKFMNax7fo6tADw5vv6w748H_58BM'

async def main():
    # Initialize Ably Realtime client
    ably = AblyRealtime(ABLY_API_KEY)
    
    # Connect to Ably
    ably.connection.connect()
    print("Connected to Ably")

    # Subscribe to the public-channel
    channel = ably.channels.get('public-channel')

    # Define a callback for the 'message.sent' event
    async def on_message(message):
        print(f"Received message: {message.data}")

    # Attach the callback to the event
    await channel.subscribe('message.sent', on_message)
    print("Subscribed to public-channel for message.sent events")

    # Keep the script running
    try:
        while True:
            await asyncio.sleep(1)
    except KeyboardInterrupt:
        print("Closing connection")
        await ably.close()


if __name__ == "__main__":
    asyncio.run(main())
"""Forward Arduino CSV sensor data to the browser flight game over WebSocket."""
import asyncio
import json
import serial
import websockets

SERIAL_PORT = "COM3"  # Change to the Uno's current port in Device Manager if needed.
BAUD_RATE = 115200
WEBSOCKET_PORT = 8765
connected_clients = set()
serial_port = None
flight_state = 'flying'

async def handle_client(websocket):
    global flight_state
    connected_clients.add(websocket)
    print(f"Browser connected ({len(connected_clients)} total)")
    try:
        async for message in websocket:
            try:
                data = json.loads(message)
            except (TypeError, json.JSONDecodeError):
                continue
            state = data.get("flightState") if isinstance(data, dict) else None
            if state in ("flying", "crashed"):
                flight_state = state
                if serial_port is not None:
                    serial_port.write(b"F" if state == "flying" else b"C")
    finally:
        connected_clients.discard(websocket)
        print(f"Browser disconnected ({len(connected_clients)} total)")

async def broadcast(payload):
    if connected_clients:
        await asyncio.gather(
            *(client.send(payload) for client in tuple(connected_clients)),
            return_exceptions=True,
        )

async def read_serial(loop):
    global serial_port
    with serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1) as port:
        serial_port = port
        port.write(b"F" if flight_state == "flying" else b"C")
        print(f"Listening on {SERIAL_PORT} @ {BAUD_RATE} baud")
        while True:
            raw = await loop.run_in_executor(None, port.readline)
            parts = raw.decode("utf-8", errors="ignore").strip().split(",")
            if len(parts) != 4:
                continue
            try:
                pitch, roll, yaw_rate = map(float, parts[:3])
                button = int(parts[3])
            except ValueError:
                continue
            payload = json.dumps({
                "pitch": pitch,
                "roll": roll,
                "yawRate": yaw_rate,
                "button": button,
            })
            await broadcast(payload)

async def main():
    loop = asyncio.get_running_loop()
    async with websockets.serve(handle_client, "localhost", WEBSOCKET_PORT) as server:
        print(f"WebSocket server running on ws://localhost:{WEBSOCKET_PORT}")
        print("Open flight_game.html in your browser now.")
        await asyncio.gather(server.wait_closed(), read_serial(loop))

if __name__ == "__main__":
    asyncio.run(main())

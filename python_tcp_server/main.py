
from tcp_server import TCPServer

if __name__ == "__main__":

    # Main Program Loop (With Keyboard Input Handling)
    server = TCPServer()
    server.start()

    try:
        while True:
            cmd = input("Enter 'exit' to stop server: ").strip().lower()
            if cmd == "exit":
                server.stop()
                break
            print(f"Active clients: {len(server.clients)}")
    except KeyboardInterrupt:
        print("\nKeyboard Interrupt detected. Stopping server...")
        server.stop()
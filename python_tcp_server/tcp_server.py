import socket
import threading

from tcp_client import Client

class TCPServer(threading.Thread):
    HOST = "172.16.1.160"  # Your Raspberry Pi's IP address
    PORT = 5000  # TCP Port
    MAX_CLIENTS = 10

    def __init__(self):
        super().__init__()

        self.host = TCPServer.HOST
        self.port = TCPServer.PORT

        self.clients = []  # Store client threads
        self.running = True

    def run(self):
        """Starts the server and listens for connections
            ~ overrides the threading running method.
            calling on TCPServer.start()
        """

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server_socket:
            server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server_socket.bind((self.host, self.port))
            server_socket.listen(5)  # <- will handle up to 5 clients.

            # Enable TCP Keepalive
            server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)  # <- add a keepalive option to check for dead clients...
            # Configure Keepalive parameters (Linux/Unix only)
            if hasattr(socket, "TCP_KEEPIDLE") and hasattr(socket, "TCP_KEEPINTVL") and hasattr(socket, "TCP_KEEPCNT"):
                server_socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE,30)  # Idle time before sending probes (seconds)
                server_socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL,5)  # Interval between probes (seconds)
                server_socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT,3)  # Number of probes to send before declaring dead

            """
                - Idle Time (TCP_KEEPIDLE): The time (in seconds) the connection must be idle before TCP starts sending keepalive probes. For example, if set to 10, probes will start after 10 seconds of inactivity.
                - Probe Interval (TCP_KEEPINTVL): The time (in seconds) between successive keepalive probes. For example, if set to 5, probes will be sent every 5 seconds.
                - Probe Count (TCP_KEEPCNT): The number of unacknowledged probes before declaring the connection dead. For example, if set to 3, the connection will be dropped after 3 failed probes.
            """

            print(f"Server running on {self.host}:{self.port}")

            while self.running:
                try:
                    server_socket.settimeout(1)  # Non-blocking accept
                    # ~ reduce this if 1 second is too long time to message/respond

                    client_socket, address = server_socket.accept()

                    # Enable TCP Keepalive on the client socket
                    client_socket.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)

                    print(f"New connection from {address}")

                    client = Client(client_socket, address, self)
                    client.start()
                    self.clients.append(client)

                    print(f"Total connections: {len(self.clients)}")

                except socket.timeout:
                    pass  # Allow loop to continue checking running state

        print("Server SHUTDOWN successful!")

    def remove_client(self, client):
        """ Removes a client from the active list
            ~ note: this removes the instance of the client.
            The method is called by the client itself, on client.stop().
        """
        if client in self.clients:
            self.clients.remove(client)

    def stop(self):
        """Stop the server and disconnect clients"""

        print("[*] Stopping server...")
        self.running = False
        for client in self.clients:
            client.stop()


import socket
import threading
import time
import errno
import struct

from recognizer import Recognizer, Decoder
from speaker import Speach


class Client(threading.Thread):
    def __init__(self, client_socket, address, server):
        super().__init__(daemon=True)
        # ~ note: daemon=True make thread running in a background,
        #         and it is terminated automatically with the program exits.
        #       - if daemon=False, thread still running even if program exits!

        self.client_socket = client_socket
        self.address = address
        self.server = server
        self.running = True

        try:
            self.recognizer = Recognizer()
            self.decoder = Decoder()
            self.speaker = Speach()

        except Exception as e:
            print(f"ERR initializing the voice recognition -> {e}")
            self.recognizer = None

    def run(self):
        """Main class loop, to handle client communication"""

        print(f"[+] New client connected and running -> {self.address}")

        self.client_socket.settimeout(1)  # Prevents blocking forever
        # set to 1.0 second. If is longer, server waits more time for answering, but DELAYS the program and response.

        while self.running:
            """ Handling incoming data and disconnections:
                ~ TCP sockets return b'' (empty bytes) when a client disconnects properly.
                ~ This is not the same as the client sending an empty message (like " " or '\n').
            """
            try:
                # [1]. Receive wake-up-call which is 1 byte, a number between 0 - 255: [101]
                data = self.client_socket.recv(1)

                if not data: # or data == b'':
                    # data is empty or empty byte string received for client disconnected signal
                    print(f"Client {self.address} has disconnected.")
                    break

                # elif len(data) == 1:  # check if exactly 1 byte is available, which means it is a wake-up-call and nothing else. Not used.

                # 1 byte 'hand-shake' message: 'wake-up' call from the client [101] or 'ready' answer from the client [202]
                if data[0] == 101:
                    # wake-up-call received from the client: 'Client has an audio data to send'
                    # 1. Answer back with [202], meaning 'I am ready'
                    self.client_socket.send(bytes([202]))
                    print("Ready signal sent. The client should start sending audio data")

                    # Prepare audio recording.
                    audio_data = bytearray()
                    audio_chunk_size = 512
                    # ~ note: chunk size must match the client (sender) audio buffer size, and the pvrhino frame_length.

                    print("Start recording...")
                    while self.running:
                        try:
                            chunk = self.client_socket.recv(audio_chunk_size)
                            if chunk == b'':
                                print("Connection closed unexpectedly")
                                break
                            else:
                                audio_data.extend(chunk)
                                # TODO: decode the chunk with pvrhino on real time.

                        except socket.timeout:
                            print("The client audio transmission ended.")
                            break

                        except Exception as e:
                            print(f"[ERR] while audio_data receive: {e}")
                            break

                    # recording ready. check and process...
                    if audio_data:
                        print(f"Data Ready, [{len(audio_data)} bytes]. PROCESSING...")
                        if self.recognizer:
                            result = self.recognizer.process_audio_data(audio_data)
                            decoder_respond = self.decoder.decode_rhino(pvRhino_result=result)
                            # --> speak back the respond
                            # Using the Speach.speak_transmit() method, which is designed to 'cal' the ESP, convert the text to audio and send the mp3 data to esp.
                            result = self.speaker.speak_transmit(text=decoder_respond,
                                                                 client=self.client_socket)
                            print(result)

                        else:
                            print("ERR: in audio processing -> recognizer not initialized. Breaking...")
                            break

                    else:
                        print("ERR: audio_data is empty!")

            except socket.error as e:
                if e.errno == errno.ETIMEDOUT:  # [Errno 110] Connection timed out
                    print(f"Client {self.address} timed out (TCP Keepalive detected disconnection).")
                    break
                elif e.errno == errno.ECONNRESET:  # [Errno 104] Connection reset by peer
                    print(f"Client {self.address} disconnected unexpectedly (Connection reset).")
                    break

            except Exception as e:
                print(f"ERR in client_socket.recv() for client {self.address} -> {e}")
                print("Continue... ")

        self.stop()

    @staticmethod
    def print_last_audio_samples(data):
        """ Printing the last ~ half second of audio data. """
        if len(data) % 2 != 0:
            data = data[:-1]  # Drop the last byte if the length is odd

        int16_values = struct.unpack(f"<{len(data) // 2}H", data)  # '<' for little-endian, 'H' for 16-bit unsigned int
        # Print the last 100 values in one line, separated by spaces, in 0x... format
        print(" ".join(f"0x{value:04X}" for value in int16_values[-8192:]))
        print("")

    def stop(self):
        """Stops the client thread and removes it from server list"""
        self.running = False
        self.client_socket.close()
        self.server.clients.remove(self)
        print(f"[-] Client {self.address} disconnected | Client thread stopped.")

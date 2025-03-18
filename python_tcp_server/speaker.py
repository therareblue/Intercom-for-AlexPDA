from google.cloud import texttospeech_v1
import os
import re
from datetime import datetime
import time

class Speach:
    PITCH = 1.5  # voice pitch
    RATE = 0.9     # voice speed rate

    SAMPLE_RATE = 16000
    CHANNELS = 1
    SAMPLE_WIDTH = 2  # 16-bit audio (2 bytes per sample)

    def __init__(self):
        self._is_error = False
        self.client = None
        try:
            # 1. init the client
            os.environ['GOOGLE_APPLICATION_CREDENTIALS'] = "tts/gtts_accnt.json"
            self.client = texttospeech_v1.TextToSpeechClient()

            # 2. configurate the voice  and response configurations

            self.voice0 = texttospeech_v1.VoiceSelectionParams(language_code='en-US', name='en-US-Wavenet-F', ssml_gender=texttospeech_v1.SsmlVoiceGender.FEMALE)

            self.audio_config_mp3 = texttospeech_v1.AudioConfig(audio_encoding=texttospeech_v1.AudioEncoding.MP3, speaking_rate=Speach.RATE, pitch=Speach.PITCH)
            self.audio_config_wav = texttospeech_v1.AudioConfig(audio_encoding=texttospeech_v1.AudioEncoding.LINEAR16, sample_rate_hertz=16000, speaking_rate=Speach.RATE, pitch=Speach.RATE)
            # ~ note: LINEAR16 is a PCM (with a sample_rate_hertz=16000, channel_count=1, and sample_width=2 (16-bit audio))
            #          ... but with a WAV header included, ensuring that the data can be played on more devices, like Arduino Audio library.

            print("TTS account is now active.")
            self._is_error = False

        except Exception as e:
            print(f"An exception raised during TTS init: {e}")
            self._is_error = True

    @staticmethod
    def _play_sound(audio_data=None, mp3_file=None):
        """
        A method to play audio, from audio data, or file (mp3)
        ~ note: this method will implement pygame to play sound, on later stage of development.
        """
        if audio_data:
            # stream the audio data
            ...
        elif mp3_file:
            # open and play local mp3 file
            ...
        else:
            print("ERR: playing sound needs audio_data or mp3_file.")

    def _speak_offline(self, text):
        """
        Used in speak_local() method to search for already spoken and recorded respond.
        This greatly reduces the latency and usage of the TTS API
        """
        try:
            # check if a file to speak (with name "text") is available offline
            encoded = Tools.encode_str(text)
            filename = f"tts/offline_audio/{encoded}.mp3"

            is_exist = os.path.exists(filename)
            if is_exist:
                self._play_sound(mp3_file=filename)
                return True
            else:
                return False

        except Exception as e:
            print(f"ERR: in _speak_offline(): {e}")
            return False

    @staticmethod
    def _transmit_offline(text):
        """
        Check if an audio file for the given text exists locally.
        If it exists, read and return the audio content.
        If not, return None.

        Args:
            text (str): The text to search for in the offline audio files.

        Returns:
            bytes: The audio content if the file exists, otherwise None.
        """
        try:
            # check if a file to speak (with name "text") is available offline
            encoded = Tools.encode_str(text)
            filename = f"tts/offline_audio/{encoded}.mp3"
            # print(f"try to speak: {filename}")
            # path = f"/home/alex/lab/alex2.0/{filename}"
            if os.path.exists(filename):
                # Read the file content
                with open(filename, "rb") as audio_file:
                    audio_content = audio_file.read()
                    print(f"Found offline audio file: {filename}. Read {len(audio_content)} bytes")
                return audio_content

            else:
                print(f"No offline audio file found for text: {text}")
                return None

        except Exception as e:
            print(f"ERR: in _offline_transmit: {e}")
            return None

    def speak_local(self, text, try_offline=True, save_it=True):
        """
        Method to speak on local machine, using some engine to generate sound from audio_data or to play mp3 file.
        """
        if not self._is_error and text:  # TODO: and if is_online...
            if try_offline and self._speak_offline(text):
                # offline respond spoken.
                pass
            else:
                # speak online:
                try:
                    synthesis_input = texttospeech_v1.SynthesisInput(text=text)
                    response = self.client.synthesize_speech(input=synthesis_input, voice=self.voice0, audio_config=self.audio_config_mp3)

                except Exception as e:
                    print(f"ERR while speak: {e}")

                else:
                    # Note: The Ubuntu max file_name is 255. So with the encoding it should not exceed that.
                    if save_it and len(text) < 60:
                        encoded = Tools.encode_str(text)
                        filename = f"tts/offline_audio/{encoded}.mp3"
                    else:
                        filename = "tts/speak.mp3"

                    with open(filename, 'wb') as output:
                        output.write(response.audio_content)

                    self._play_sound(mp3_file="tts/speak.mp3")

    def speak_transmit(self, text, client, save_it=False):
        # --> check if the text is already spoken (mp3 file available in the offline_audio/)
        #     and send the audio file to the esp32
        if not self._is_error and text:

            # 1. Get the audio data:
            audio_content = self._transmit_offline(text)
            if audio_content is None:  # TODO: and if is_online...
                # Generate new audio content using Google Cloud TTS
                try:
                    synthesis_input = texttospeech_v1.SynthesisInput(text=text)
                    response = self.client.synthesize_speech(input=synthesis_input, voice=self.voice0,
                                                             audio_config=self.audio_config_mp3)

                    audio_content = response.audio_content

                    if save_it and len(text) < 60:
                        encoded = Tools.encode_str(text)
                        mp3_filename = f"tts/offline_audio/{encoded}.mp3"
                    else:
                        mp3_filename = "tts/speak.mp3"

                    # ->  save the file
                    with open(mp3_filename, 'wb') as output:
                        output.write(response.audio_content)
                    # ~ note: we save the file to local, no mater save_it flag. But if False, we overwrite the speak.mp3 file.

                except Exception as e:
                    print(f"ERR while generating online GTTS respond: {e}")
                    print("Transmit to ESP32 terminated..")

            # 2. Send the audio data over TCP Wifi:
            if audio_content:
                try:

                    # a. sending 'server-call' to the client - the file name of the spoken text, encoded to binary, max 29 chars, to 29 bytes.:
                    server_call_data = Tools.mp3name_to_bin(text)
                    client.send(server_call_data)
                    print("Server-Call signal sent")

                    # b. waiting for client to answer...
                    # ~ note: timeout is set to 1.0 second for fast response. But there are 3 check tries if client delays...
                    attempts = 0

                    while attempts < 3:
                        try:
                            response = client.recv(1)  # Expecting 1 byte response

                            # total_time = (time.time() - start_time) * 1000
                            # operation_time = (time.time() - last_record) * 1000
                            # last_record = time.time()

                            if response[0] == 22:  # 1 byte with value of 22

                                # print(f"ESP32 ready to receive audio data. -> {total_time:.1f} | {operation_time:.2f} ms")
                                print(f"ESP32 ready to receive audio data...")

                                # sand the audio data to the client:
                                print(f"Streaming audio [{len(audio_content)} bytes] --TCP--> to ESP32...")
                                client.sendall(audio_content)

                                # At the end:
                                print("Audio data sent successfully.")
                                break

                            elif response[0] == 11:
                                # print(f"ESP32 has the audio data pre-recorded. Do not send. -> {total_time:.5f} | {operation_time:.2f} ms")
                                print(f"ESP32 has the audio data pre-recorded. Do not send.")
                                break
                            else:
                                attempts += 1
                                print(f"Unexpected response from ESP32, try more {2 - attempts} times.")
                        except Exception as e:
                            attempts += 1
                            print(f"err or mo response from client. Try more {2 - attempts} times.")
                            print(e)

                except Exception as e:
                    print(f"ERR while speak_transmit: {e}")
                    return False

            else:
                print("No audio content collected. Transmit terminated.")


class Tools:

    @staticmethod
    def formatted_time():
        now = datetime.now()
        if now.hour < 12:
            return now.strftime("%-I:%M")  # No AM for morning times
        return now.strftime("%-I:%M%p")  # AM/PM for afternoon/evening

    @staticmethod
    def encode_str(text):
        """ used in local machine to store the frequently spoken responds,
            and replay them instead of regenerate them. """

        enc = [".", "?", "!", " ", "'", ",", ":", "-"]
        dec = ["_a_", "_b_", "_c_", "_d_", "_e_", "_f_", "_g", "_h_"]

        text2 = text
        for c in text:
            if c in enc:
                index = enc.index(c)
                n = dec[index]
                text2 = text2.replace(c, n)
        return text2

    @staticmethod
    def decode_str(text):
        enc = [".", "?", "!", " ", "'", ",", ":", "-"]
        dec = ["_a_", "_b_", "_c_", "_d_", "_e_", "_f_", "_g", "_h_"]

        text2 = text
        for code in dec:
            if text2.find(code) != -1:
                index = dec.index(code)
                n = enc[index]
                text2 = text2.replace(code, n)
        return text2

    @staticmethod
    def mp3name_to_bin(text):
        """ Used to create a short .mp3 file name from raw text and encode it to BINARY data.
            Decoding a frequently used speech responds,
            to be recorded and use it locally (ak. 'yes', 'may I help you', etc.)
        """
        # 1. strip to only letters and numbers
        text_strip = re.sub(r'[^a-zA-Z0-9]', '', text)

        # 2. If length > 26 or any number is present, return default 'mp3respond.mp3'
        # ~ note: numbers means this is non frequent used respond (ak. 'temperature is 32 degrees')
        if len(text_strip) > 24 or any(char.isdigit() for char in text_strip):
            print("Text has more than 24 chars or has a number. 'mp3respond.mp3' returned instead.")
            filename = "/mp3respond.mp3"
        else:
            filename = f"/{text_strip.lower()}.mp3"

        # 3. Encode to binary data of  bytes fixed lenght. Fill the empty with zeroes.
        binary_data = filename.encode('utf-8')  # Convert text to bytes
        return binary_data.ljust(29, b'\x00')  # Pad with zeroes to make it 29 bytes





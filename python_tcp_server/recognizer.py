import pvrhino
import wave
import numpy as np
import struct


class Recognizer:
    SAMPLE_RATE = 16000     # Audio Sample Rate. 16 Khz
    CHANNELS = 1            # 1 channel (MONO)
    SAMPLE_WIDTH = 2        # 16-bit audio (2 bytes per sample)
    CHUNK_SIZE = 512        # Data received in chunks from ESP32-S3. ESP buffer is 512 byte, so the server needs to capture exactly this.

    GAIN_FACTOR = 10        # used to boost the gain of the audio file, for clear capture.
    QUIET_BYTE = 0x000F     # we replace/ or add/ the last part of the collected audio_data with some quiet time, for the pvRhino to work
    # this assures data will have a quiet end, even in a noisy environment.

    _PV_ACCESS_KEY = 'your_api_key_here'
    _WAKEWORD_PATH = ['sr/wakeword/picovoice_porcupine_wake_word.ppn']
    _RHINO_MODEL_PATH = 'sr/rhino/rhino_speach_to_intent_model.rhn'
    # - NOTE: generate your model and api key in the Picovoice Console.

    def __init__(self):

        self.rhino = pvrhino.create(access_key=Recognizer._PV_ACCESS_KEY,
                                    context_path=Recognizer._RHINO_MODEL_PATH,
                                    sensitivity=0.35,
                                    endpoint_duration_sec=0.8,
                                    require_endpoint=True)

    @staticmethod
    def _read_file(file_name, sample_rate):
        wav_file = wave.open(file_name, mode="rb")
        channels = wav_file.getnchannels()
        sample_width = wav_file.getsampwidth()
        num_frames = wav_file.getnframes()

        if wav_file.getframerate() != sample_rate:
            raise ValueError(
                "Audio file should have a sample rate of %d. got %d" % (sample_rate, wav_file.getframerate()))
        if sample_width != 2:
            raise ValueError("Audio file should be 16-bit. got %d" % sample_width)
        if channels == 2:
            print("Picovoice processes single-channel audio but stereo file is provided. Processing left channel only.")

        samples = wav_file.readframes(num_frames)

        wav_file.close()

        print(f"wave file readed, total of {len(samples)} bytes")

        # frames = struct.unpack('h' * num_frames * channels, samples)
        # return frames[::channels]

        frames = struct.unpack('<' + 'h' * (len(samples) // 2), samples)
        return frames

    @staticmethod
    def _amplify_audio(audio_data:bytearray, gain_factor:int):
        print("Applying amplification...")
        # Convert audio data to numpy array -> 16 bit lineary encoded income (signed integer)
        audio_data_np = np.frombuffer(audio_data, dtype=np.int16)
        audio_data_np = (audio_data_np * gain_factor).clip(-32768, 32767).astype(np.int16)
        # audio_data_amplified = audio_data_np.tobytes()
        return bytearray(audio_data_np.tobytes())

    @staticmethod
    def _quieting_audio_end(audio_data:bytearray, quiet_duration, quiet_value=0x000F, insert_mode='split', sample_rate=16000, sample_width=2, channels=1):
        """
        Insert or/and replace the last part of the audio data with a quiet sound.
        Used for preparing the audio data to be more reliable for recognition,
        helping the recognizer to finalize the result more easily.

        Args:
            audio_data (bytearray): The audio data to modify.
            quiet_duration (float): Duration of quiet sound to add/replace (in seconds).
            quiet_value (int): Small non-zero value representing 'quiet' sound (default: 0x000F).
            insert_mode (str): 'replace' the end of the audio;  'extend': add the quiet bytes to the audio end; 'split': (half replace, half extend)
            sample_rate (int): Audio sample rate (default: 16000 Hz).
            sample_width (int): Audio sample width in bytes (default: 2 for 16-bit audio).
            channels (int): Number of audio channels (default: 1 for mono).
        """
        # Calculate the total number of bytes for the quiet duration
        quiet_samples_num = int(sample_rate * quiet_duration)
        total_quiet_bytes = quiet_samples_num * sample_width * channels

        # Trim the total quiet bytes to the nearest multiple of 1024.
        # ~ note: by trimming to x1024 (not to x512) we assure that, both, full append and split append/insert will be a multiple of 512
        frame_size_to_trim = 1024  # Rhino required frame length
        trimmed_quiet_bytes_num = (total_quiet_bytes // frame_size_to_trim) * frame_size_to_trim

        # create the full array of bytes for the quiet part, to be used in all cases:
        quiet_samples = np.full(trimmed_quiet_bytes_num // (sample_width * channels), quiet_value, dtype=np.int16)
        quiet_bytes = quiet_samples.tobytes()
        quiet_bytes_len = len(quiet_bytes)

        final_quiet_time = quiet_bytes_len / (sample_rate * sample_width)
        # ~ note: for 16khz 1 channel, duration for 1 sample of 512 bytes = 0.032 sec.
        print(f"Requested quiet time was trimmed ({quiet_duration} -> {final_quiet_time:.3f} sec | {quiet_bytes_len} bytes)...")

        if insert_mode == 'split':
            # split the quiet part on half and do both - insert and append to the audio_data

            half_quiet_bytes = quiet_bytes_len // 2
            # Replace the last part of the audio data with the first half of quiet bytes
            if len(audio_data) >= half_quiet_bytes:
                audio_data[-half_quiet_bytes:] = quiet_bytes[:half_quiet_bytes]
            else:
                # If the audio data is too short, replace as much as possible
                replace_bytes = len(audio_data)
                audio_data[-replace_bytes:] = quiet_bytes[:replace_bytes]

            # Append the second half of quiet bytes
            audio_data.extend(quiet_bytes[half_quiet_bytes:])

            print(f"Operation done. The resulted audio_data length is {len(audio_data)} bytes")

        elif insert_mode == 'extend':
            # Append quiet bytes to the end of the audio data
            audio_data.extend(quiet_bytes)
            print(f"Operation done! Extended with [{quiet_bytes_len} bytes] -> total: [{len(audio_data)} bytes]")

        elif insert_mode == 'replace':
            # Replace the last part of the audio data with full lenght of the quiet_bytes
            if len(audio_data) > quiet_bytes_len:
                # Replace the last part of the audio data with quiet bytes
                audio_data[-quiet_bytes_len:] = quiet_bytes
                print(f"Quieting done! Replaced [{quiet_bytes_len} bytes] -> total: [{len(audio_data)} bytes]")
            else:
                print(f"Audio data is too short to replace the last [{quiet_duration}] seconds. Passed.")

        else:
            raise ValueError("insert_mode should be a str equal to 'replace', 'extend' or 'split'.")

    @staticmethod
    def save_wav_file(audio_data, sample_rate, sample_width, channels, filename="audio_data.wav"):
        with wave.open(filename, "wb") as wf:
            wf.setnchannels(channels)
            wf.setsampwidth(sample_width)
            wf.setframerate(sample_rate)
            wf.writeframes(audio_data)
        print(f"Audio saved as {filename}")

    def process_audio_data(self, audio_data):
        # 1. amplify the audio_data:
        audio_data_amplified = self._amplify_audio(audio_data, self.GAIN_FACTOR)
        print(f"Audio amplified by factor of {self.GAIN_FACTOR}. Amplified: [{len(audio_data_amplified)} bytes]")

        # 2. quieting the last part of the audio, for recognition purposes
        self._quieting_audio_end(audio_data_amplified, 1.0)
        # 512 bytes per frame, 16000 bytes per sec --> 0.032 sec per frame. (31.25 frames x 512)/16000 = 1.0 sec
        # ~ note: the quiet duration will be trimmed to fit the x512 rule multiple (31 frames -> 0.992 s)

        print(f"Audio data ready for save and recognition.")

        # self.save_wav_file(audio_data_amplified,
        #                    sample_rate=self.SAMPLE_RATE,
        #                    sample_width=self.SAMPLE_WIDTH,
        #                    channels=self.CHANNELS,
        #                    filename="audio_data.wav")

        # recognizing...
        audio_frames = struct.unpack('<' + 'h' * (len(audio_data_amplified) // 2), audio_data_amplified)

        frame_length = self.rhino.frame_length  # the value is 512.
        num_frames = len(audio_frames) // frame_length

        print(f"Start decoding [{num_frames} frames]... rhino frame length = {self.rhino.frame_length}")
        try:
            # frames_scanned = 0
            for i in range(num_frames):
                frame = audio_frames[i * frame_length:(i + 1) * frame_length]
                is_finalized = self.rhino.process(frame)
                if is_finalized:
                    print("Finalized!")
                    # print(f"Finalized! -> {frames_scanned} frames scanned, total of {frames_scanned*frame_length} bytes.")

                    inference = self.rhino.get_inference()
                    if inference.is_understood:
                        return inference.intent, inference.slots
                    else:
                        # print(f"Didn't understand the command. -> {frames_scanned} frames scanned, total of {frames_scanned*frame_length} bytes.")
                        return None

                # frames_scanned += len(frame)
            # print(f"pvRhino finished -> {frames_scanned} frames scanned, total of {frames_scanned*frame_length} bytes.")

        except Exception as e:
            print(f"ERR in pvrhino decode -> {e}")

    def clear_res(self):
        print("clearing PicoVoice...")

        if self.rhino is not None:
            self.rhino.delete()

        if not self.rhino:
            print("Picovoice resources cleared successfully")


class Decoder:
    """
    Decode the intent and slots returned by
    picovoice Rhino speach-to-intent,
    giving back a human friendly text response.

    This is a bridge between the action methods (light-on, door-close...) and the pvRhino model.
    """
    def __init__(self):
        # TODO: this class may inherit the action / skills class and its executing methods.
        #       or the possible Engage class may inherit this class, and skills class too,
        #       and call something like engage.decode_rhino(), for example...
        ...

    def decode_rhino(self, pvRhino_result:tuple):
        """
        The method decodes the incoming request and return / run the functions that should be ran.
        It also returns a text response, for the user to be spoken back.

        ~ note: When the used pvRhino model is changed, only this part should be reworked.
        """
        if pvRhino_result is not None:
            intent, slots = pvRhino_result

            # -- check the result...
            print(f"YOU: intent={intent} | slots={slots}")

            print('{')
            print("  intent : '%s'" % intent)
            print('  slots : {')
            for slot, value in slots.items():
                print("    %s : '%s'" % (slot, value))
            print('  }')
            print('}')

            ...
            test_respond = "Command received and understood!"
        else:
            test_respond = "Sorry, I did not understand that."

        return test_respond






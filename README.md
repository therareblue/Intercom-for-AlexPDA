Intercom_v01 code:

Demo code for the intercom device.
Code written with Arduino IDE 2.3.2
To be used with ESP32-S3 Super Mini board. 
~ note: the board has integrated addressed RGB led WS2818, connected to pin 48

Uses:
- 1 button (connected to GPIO 12 and ground)
- i2S microphone MSM261S4030H0
- i2s MAX98357A amplifier
- 2W speaker

The functionalities included:
1. Microphone capture and send to TCP server
- Raw microphone data captured and send real time on button pressed. 
- Capture stops when low sound level detected (silence), or button second time press.

2. Audio receive and play from server.
- Tried direct raw audio receive and 'write' to the i2s output driver, but configuring the audio settings were difficult.  
- Used a method with mp3 file exchange, from server to the ESP32, then play it on ESP32 with Audio library (https://github.com/schreibfaul1/ESP32-audioI2S).

3. WiFi and TCP server connection handle.
- implemented connect and reconnect functionalities. 

I2S audio configuration:
Microphone:
  - i2s port: 1
  - bits per sample: 32 bit (capturing the max microphone quality. then convert it to 16 bit. This improves speed and quality)
  - channel used: ONLY LEFT
  - Sample rate: 16000 (16 Khz)
  - buffer: 512

Amlifier:
  - bits per sample: 16 bit
  - channel used: ONLY LEFT
  - Sample rate: 16000 (16 Khz)
  - buffer: 1024

=============

Python TCP Server and receiver code:
Tested on Raspberry Pi 4, 8GB RAM, Raspbian OS

1. Creates and manages the TCP server, over local WiFi network
2. Handles client (intercom_v01) calls, for start and stop communication.
3. Implement audio amplification of the raw microphone data, received from the ESP32
  - Using numpy library, the gain of the incomming audio data was increased by 10 times, without any distortion.
 
4. Implements on-board speech-to-intent recognition, using Picovoice Rhino Speech to intent.
5. Generates respond, using Google.cloud texttospeech_v1 library.
  -- Тhe tts respond generation implements method to store an replay the frequently spoken respond, reducing latency and google.cloud API usage.
  -- Uses mp3 respond with the API. Tested direct PCM raw data respond, but not yet able to transfer and play the audio data from Python server to ESP32-s3 client on real time.

General audio format settings:
Picovoice Rhino: 
  - Audio sample rate: 16 Khz
  - channels: 1
  - sample width: 2 (16-bit audio, 2 bits per sample)
  - max accepted chunk size: 512 bytes
  ~ if the audio receive interupts before filling the 512byte buffer, the rest of the buffer is filled with quied bytes (0x000F)
  - Adding quiet end to the audio data, which helps picovoice Rhino to recognize the audio, even in a noisy environment

Google TTS:
  - Audio sample rate: 16 Khz
  - channels: 1
  - sample width: 2 (16 bit audio, 2 bits ber sample)
  - encodding: mp3




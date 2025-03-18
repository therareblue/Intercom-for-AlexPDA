/*
DESCRIPTION:
InterCOM remote device to communicate with the AI interface.
~ it uses WiFi TCP protocol to send / receive audio data from the interface's python script.

- board: esp32-S3 Super Mini (DF-Robot)
- i2S microphone MSM261S4030H0  on i2s port 1
- i2s MAX98357A amplifier       on i2s port 0
- button 'wake-up-call'
- 2x buttons for device specific selection 
  * allows shorten audio commands to be performed for the most used (or curently used) device

- i2c display, * at later point of the project
- maybe a vibration motor too, if pins are available.

--? GPIO connections:
Button:         GPIO 12
LED:            GPIO 48 (default on-board WS2818 rgb led)

MICROPHONE: 
  - CK:         GPIO 2
  - WA:         GPIO 1
  - DA:         GPIO 3

OUTPUT AMPLIFIER:
  - DOUT:       GPIO 4
  - BCLK:       GPIO 5
  - LRC:        GPIO 6
  - SD:         GPIO 7
*/

// ***************** INCLUDE *****************
#include <WiFi.h>
#include <WiFiClient.h>
#include "esp_bt.h"

#include <SPIFFS.h>

#include <driver/i2s.h>
#include "Audio.h"

#include <Adafruit_NeoPixel.h>

// --- Configurations ---
#include "config.h"

// ***************** MACROS & CONSTs *****************

// -- microphone --
// I2S port settings
#define I2S_PORT_MIC      I2S_NUM_1  // I2S port for microphone input ( Using I2S0 )
#define I2S_PORT_AUDIO    I2S_NUM_0  // I2S port for audio output ( Using I2S1 )

#define PIN_I2S_BCLK  2     // Bit Clock (CK)
#define PIN_I2S_LRCLK 1     // Word Select (WS) - Might not be needed
#define PIN_I2S_DATA  3     // Data from mic (DA)
// ~ note: microphone pin LR is connected to GND.


#define SAMPLE_RATE_MIC 16000
#define BUFFER_MIC 512         // max buffer used. note: esp32-s3 i2s max buffer is 1024 bytes

#define SAMPLE_RATE_OUT 16000
#define BUFFER_OUT 1024

// -- amplifier --
#define MAX_I2S_DOUT 4
#define MAX_I2S_BCLK 5
#define MAX_I2S_LRC 6
#define MAX_SD 7

// #define MAX_SAMPLES 100  // Number of samples to track for max value
// #define SCALE_FACTOR 255 // Max value for scaling (for plot consistency)

#define BTN0 12   // wake-up call signal
/// TODO: 2 more buttons.

#define BAT 13

#define LED_PIN 48  // on-board ws2818 rgb led
#define NUM_LEDS 1


// ***************** VARS *****************

// audio - microphone
// uint8_t i2s_buffer[BUFFER_SIZE];  // <-- moved as local variable inside the stream function.
File audioFile;
Audio audio(0);  // init the  Audio library for easily working with output audio, port 0
// - NOTE: audio library by default takes over the port 0 i2s, event explicity reconfigure the ports

// - addressed rgb led on board -
Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// led (Neopixel colors):
static uint32_t RED = strip.Color(255, 0, 0);
static uint32_t GREEN = strip.Color(0, 255, 0);
static uint32_t BLUE = strip.Color(0, 0, 255);
static uint32_t SKYBLUE = strip.Color(0, 50, 255);
static uint32_t AQUA = strip.Color(0, 255, 255);
static uint32_t YELLOW = strip.Color(255, 255, 0);
static uint32_t ORANGE = strip.Color(255, 187, 0);
static uint32_t PURPLE = strip.Color(170, 0, 255);
static uint32_t BLUERED = strip.Color(20, 0, 180);

// network -- 
WiFiClient client;

// operation --
uint8_t network_state = 0;
/*
0: not connected to wifi
1: wifi-on, no tcp 
2: online and ready. iddle mode waiting for wake-up-call btn press 
*/

// uint8_t program_state = 0;  
/* 
0: non-functional. waiting. (flashing slowly yellow as a warning.) --> used when not connected, but all init ok.
1: all functional. Iddle.   (fade-in/out slowly, green)
2: speaking (fast flashing blue during speak)

3: call-btn pressed.  expecting confirmation answer from the server. 
4: start capturing from microphone and send to TCP
   ~ the microphone reading is checking level. if talk_thresshold is bellow minimum (no speeak detected) for more than 3 seconds, stop sending
44: init error. not initialized. 

--> Short LED signals:
0: program restarting...                        -> flash 2 times slow cyan
1: success, not errors                          -> flash 2 times fast green
2: warning (for example, when storage is full)  -> flash slow yellow during operation (press call button. )
44: error. operation not performed.             -> flash 3 times fast red 
*/

int device_to_control = 11; // 11 : no device selected.  used to all devices; 
// - uses device mqtt codes (look in config.h), for sending device number to TCP on wake-up-call.
// - on speciffic device selected, for example 102 (garage door) alex won't need the device name for the command, for example, 'open-up'; 
// - if device name included 'open-up the hidden door', it will trigger hidden door even if garage door is selected.

// -- for audio send purpouses. --
// ~ checks for 'silent' during sending. If no speak for more than 1 second, or maximum command time (7 sec) finish, stop sending and go to iddle.
const uint32_t max_recording_time = 10000;  // Max recording time (7 sec)
const uint32_t silence_timeout = 1000;  // Stop sending if silent for 800ms
const int silence_thresshold = 5000000; // Adjust based on microphone sensitivity. // for 16 bit is 100. to about 900... for 32 bit is 2331649 min --> ~ 102015231 max

// -- flags --
uint8_t memory_state = 0;  
// 0: SPIFFS storage partition does not initialize, or files are corupted or not found
// 1: memory perfect.
// 2: low storage space (< 20%)

bool is_online = false;
bool is_audio = false;
bool silent = false;
bool led_speak = false; // when speaking starts, a different LED method is used, ledOnSpeak(), updated in the speaking function. The ledOnMode() then is not updating.
bool led_state = false;  // 0 - off; 1 - on.

// -- timings --
uint32_t last_btn = 0;
uint32_t last_rstBtn = 0;
uint32_t last_led = 0;
uint32_t last_reconnect_attempt = 0;

const uint32_t btn_update_interval = 500;
const uint32_t reconnect_interval = 5000;


// ***************** FUNCS *****************

// ------------------
// 1. ------------ TOOLS: ---------------

size_t getFlashSize(){
  /* Function to get the remaininhg flash size, for diferent purpouses.
     - mainly used to refresh the memory_state, for low-storage warnings,
     - but is also used to return the actual storage available (for a display to be shown)
  */
  if (memory_state){  // if SPIFFS is not initialized, the state stays 0;
    size_t total = SPIFFS.totalBytes();
    size_t used = SPIFFS.usedBytes();
    size_t freeSpace = total - used;

    if (freeSpace < total * 0.20){
      Serial.println("Warning: SPIFFS storage low (<20%)!");
      memory_state = 2;
    }
    return freeSpace / 1024;  // Converts bytes to KiB;
  }else{
    Serial.println("No SPIFFS initialized, or audio files are not found.");   
    return 0;
  }
}

bool isValidCharName(const char *filename) { 
    //1.  Check for .mp3 extension
    int len = strlen(filename);
    if (len < 5 || len > 30 || strcmp(filename + len - 4, ".mp3") != 0) {
        return false;
    }
    
    //2. Check for invalid characters. only chars, _  and . is accepted
    for (int i = 0; i < len; i++) {
        if (!isalnum(filename[i]) && filename[i] != '_' && filename[i] != '.') {
            return false;
        }
    }
    return true;
}

void convertBinaryToStr(uint8_t *data, char *output, size_t length_of_data){
  /* function to convert a binary data into char array 
     ~ note: the max char array size will not ecceed 31 symbols; 
     Used to receave an audio data file name from TCP server, to be saved as mp3 file into SPIFFS.
     ~ note: the / symbol (needed for the SPIFFS) is included in the filename data already
     --> use: char char_output[30] -> alocating a buffer for the resulted filename of 29 chars +1 for null terminator
     ~ note: python server creates a short name of max /+ 24 + .mp3 = 29 chars, where the SPIFFS max filename size = 31. So we are good.
     --> convertBinaryToStr(bin_data, char_output, sizeof(bin_data)); Serial.println(char_output)
  */
  memcpy(output, data, length_of_data);  // Copy raw bytes to char array
  // ~ note: out output has length_of_data + 1, of size.
  // output[length] = '\0';  // null-terminated at the end. We does not need that. the 'for loop' will put it, because it starts from index 29, which is the 30th byte.

  // Remove trailing null characters (optional)
  for (int i = length_of_data; i >= 0; i--) {
    if (output[i] == '\0' || output[i] == 0x00) {
        output[i] = '\0'; 
    } else {
        break;
    }
  } 
}

void convertStrToBinary(char * input_text){
  /* converts char array to binary to be send to the server over TCP */
  // ~ note: input text will be checked for size. if > 31 chars, return error.
}
void convertByteToBinary(){
  /* converts uint8_t (0-255) to binary to be send to the server over TCP */
  
}
void convertIntToBinary(){
  /* converts signed integer 32 bit, to binary to be send to the server over TCP */
}
  
uint32_t scale_color(uint32_t color, uint16_t brightness, uint16_t max_brightness=255) {
  // --info: method sets calculates the r,g,b based of given color and brightness...
  uint8_t r = (color >> 16) & 0xFF;
  uint8_t g = (color >> 8) & 0xFF;
  uint8_t b = color & 0xFF;
  r = (r * brightness) / max_brightness;
  g = (g * brightness) / max_brightness;
  b = (b * brightness) / max_brightness;
  return strip.Color(r, g, b);
}

// ------------------
// 2. --------- DEVICES SETUP: ------------

// -- microphone driver --
void setupMicrophone(){
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX), 
    .sample_rate = 16000,  // 16 kHz sample rate
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // Still using 32-bit samples
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // Assuming single-mic setup
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = BUFFER_MIC,  
    .use_apll = false,  
  };
  i2s_pin_config_t pin_config = {
    .mck_io_num = I2S_PIN_NO_CHANGE,  // Master Clock not needed
    .bck_io_num = PIN_I2S_BCLK,       // BCLK
    .ws_io_num = PIN_I2S_LRCLK,       // LRCLK (WS)
    .data_out_num = I2S_PIN_NO_CHANGE,// No Output, only Input
    .data_in_num = PIN_I2S_DATA       // Mic Data
  };
  i2s_driver_install(I2S_PORT_MIC, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT_MIC, &pin_config);
  i2s_zero_dma_buffer(I2S_PORT_MIC);

  Serial.println("I2S Microphone Initialized.");
}

// -- speaker driver --
void setupSpeaker(){
  /* - NOTE: some of the options will be rewrited by the Audio.h library. */

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),  // Output mode
    .sample_rate = SAMPLE_RATE_OUT,  // Audio sample rate
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // Mono output
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = BUFFER_OUT,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    // .fixed_mclk = 0
  };
  i2s_pin_config_t pin_config = {
    .mck_io_num = I2S_PIN_NO_CHANGE,  // Master clock not needed
    .bck_io_num = MAX_I2S_BCLK,       // Bit clock for output
    .ws_io_num = MAX_I2S_LRC,         // Word select (LR)
    .data_out_num = MAX_I2S_DOUT,     // Data output
    .data_in_num = I2S_PIN_NO_CHANGE  // No data input for output
  };
  // Install I2S driver for audio output (I2S_NUM_1)
  i2s_driver_install(I2S_PORT_AUDIO, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT_AUDIO, &pin_config);
  i2s_zero_dma_buffer(I2S_PORT_AUDIO);
  Serial.println("I2S Audio Output Initialized.");
}

void memoryInit(){
  memory_state = 1;

  //1. initialize and test SPIFFS library
  if (!SPIFFS.begin(true)) {
    //SPIFFS Mount Failed
    memory_state = 0;
  }

  if (!SPIFFS.exists("/chc0.mp3")){
    // mount success, but files not found
    memory_state = 0;
  }
  size_t storage_size = getFlashSize();
  if (memory_state){
    // this will change memory_state if there is not enough space.
    Serial.print("SPIFFS initialized successfully. Available space: "); Serial.print(storage_size); Serial.println("Kbyte. ");
  }
}

void respondInit(){
  /* initializing the SPIFFS internal memory and audio library parameters 
    - returns an error flag for detailed information
  */
  
  if (memory_state) {
    // 1. initialize Audio
    audio.setPinout(MAX_I2S_BCLK, MAX_I2S_LRC, MAX_I2S_DOUT); // -> pinout was set in the i2s setup
    audio.setVolume(21); // 0...21
    digitalWrite(MAX_SD, HIGH);
    is_audio = true;
    Serial.println("Audio init success!");
  }else{
    Serial.println("SPIFFS storage is not initialized. Audio library not set.");
    digitalWrite(MAX_SD, LOW);
    is_audio = false;
  }

}

// ------------------
// 3. ------------ RESPOND: ----------------
// --- on board rgb led ---
void led_clear(){
  // led clear, (without refresh/show)
  strip.setPixelColor(0, strip.Color(0, 0, 0)); // Turn off the LED
}
void led_off(){
  // led off. (clear and refresh/show)
  led_clear();
  strip.show();  // Update the strip to turn off the LED
  led_state = false;
}
void led_on(uint32_t color, uint16_t brightness=255){
  // solid color led on
  
  if (brightness < 255){
    uint32_t color_dark = scale_color(color, brightness);
    strip.setPixelColor(0, color_dark);
  }else{
    strip.setPixelColor(0, color);
  }
  strip.show();
  led_state = true;
}

void led_flash(uint32_t color, uint8_t flash_num, uint32_t flash_duration) {
  /* ome time sequence of flashing leds, using delay(flash_duration) in miliseconds. Blocking function.
  */
  for (int i = 0; i < flash_num; i++){
    led_on(color);
    delay(flash_duration);
    led_off();
    delay(flash_duration);
  }
}

void ledOnCmd(uint8_t mode_cmd){
  /* onboard LED modes, gpio 48, acording the function use.
      ~ working everywhere. Non-blocking and blocking. Solid color / fade or flash 
    
    0: OFF. no led
  */
  if (!led_speak){  // <- works only if ledOnSpeak() is not working. 
    switch (mode_cmd) {
      case 0: {
        // -> Led OFF:
        
      }break;
      case 1: {
        // -> IDDLE. GREEN/RED/YELLOW Wmbiend fade (on/off), acording the network state and storage, Working on void loop() with non-blocking with millis():
        //...
      }break;
      case 2: {
        // -> LISTENING: Solid green. Working on btnCheck() on button press.
        if (memory_state == 1){
          led_on(GREEN);
        }else if (memory_state == 2){
          led_on(YELLOW);
        }else{
          led_on(RED);
        }
        
      }break;
      case 3: {
        // -> Stop listening. Sending the recorded message. Flash 2 times fast Aqua.
        led_flash(AQUA, 2, 80);

        
      }break;
      case 4: {
        // -> 
        
      }break;
      case 5: {
        // -> 
        
      }break;
      case 44: {
        led_flash(RED, 3, 80);
      }break;
    }

  }
}
// void ledOnSpeak(){
//   /* fast flash the onboard LED, gpio 48, acording during the speaking / audio respond.
//       working on void speak(). */
//   led_speak = true;
//   // - NOTE: while this method is working, ledOnMode(program_mode) looping in main loop(), does not operate (led_speak flag is true)

// }

// -- audio --
void beep(uint8_t cmd, bool manual_delay=true) {
  if (is_audio && ! silent) {
    uint32_t duration = 0;
    switch(cmd){
      case 0:{
        // -> program start: 
        audio.connecttoFS(SPIFFS, "/beep0.mp3");
        duration = 400;
      }break;
      case 1: {
        // -> Call button pressed. Start listening.
        // ~ note: if server is not reachable, instead of beep(1), a not-connected prompt will be played.
        audio.connecttoFS(SPIFFS, "/beep8.mp3");
        duration = 500;
      }break;
      case 2: {
        // -> data sent to server.
        audio.connecttoFS(SPIFFS, "/beep1.mp3");
        duration = 400;
      }break;
      case 3: {
        // -> click.
        audio.connecttoFS(SPIFFS, "/chc1.mp3");
        duration = 300;
      }break;
      case 44: {
        // err.
        audio.connecttoFS(SPIFFS, "/beep444.mp3");
        duration = 1100;
        uint32_t begin_time = millis();
        uint32_t now = millis();
        while(now - begin_time < duration){
          audio.loop();
          if (now - last_led > 80){
            last_led = now;
            // non-blocking led flash red
            if (led_state){
              led_off();
            }else{
              led_on(RED);
            }
          }
          now = millis();
        }
        led_off(); // ensure led-off at the end
      }break;
    }

    if (manual_delay && cmd!=44){
      // ~ note: this will cause held the program, until the file finish.
      uint32_t begin_time = millis();
      while (millis() - begin_time < duration){
        audio.loop();
      }
    }
  }
}
void say(uint8_t cmd, bool manual_delay=true){
  if (is_audio && !silent){
    uint32_t duration = 0;  // tracks the duration of the file, hard codded. Used for manual delay.
    switch(cmd){
      
      case 0:{
        // -> device online and ready: 
        audio.connecttoFS(SPIFFS, "/activated0.mp3");
        duration = 2000;
      }break;
      case 2:{
        // -> connecting: 
        audio.connecttoFS(SPIFFS, "/connecting0.mp3");
        duration = 1000;
      }break;
      
      // --> error types
      case 45: {
        // no wifi
        audio.connecttoFS(SPIFFS, "/err_wifi.mp3");
        duration = 2000;
      }break;
      case 46: {
        // no server
        audio.connecttoFS(SPIFFS, "/err_server.mp3");
        duration = 3000;
      }break;

      //...

    }

    if (manual_delay){
      // ~ note: this will cause held the program, until the file finish.
      uint32_t begin_time = millis();
      while (millis() - begin_time < duration){
        audio.loop();
      }
    }
    
  }
}
void ask(uint8_t cmd, bool manual_delay=true){
  uint32_t duration = 0;
  switch(cmd){
      case 0:{
        // -> Yes ? : 
        audio.connecttoFS(SPIFFS, "/yes_q.mp3");
        duration = 1000;
      }break;
      case 1: {
        // I'm here ? 
        audio.connecttoFS(SPIFFS, "/imhere_q.mp3");
        duration = 1000;
      }break;
      case 2:{
        // -> Tell me?: 
        audio.connecttoFS(SPIFFS, "/tellme_q.mp3");
        duration = 1000;
      }break;
      case 3: {
        // Sir ?
        audio.connecttoFS(SPIFFS, "/sir_q.mp3");
        duration = 1000;
      }break;
      case 4: {
        // Hey ?
        audio.connecttoFS(SPIFFS, "/hey_q.mp3");
        duration = 1000;
      }break;
      case 10: {
        audio.connecttoFS(SPIFFS, "onyourcommandsir.mp3");
        duration = 2000;
      }
    }

  if (manual_delay){
      // ~ note: this will cause held the program, until the file finish.
      uint32_t begin_time = millis();
      while (millis() - begin_time < duration){
        audio.loop();
      }
    }
}
void speak(const char * filename, bool manual_delay=true){
  /* playing a prerecorded mp3 file, generated from the server text-to-speech engine.
     ~ note: manual_delay calculates the total play time of the file, based of the bytes it contains, 64bps, 8 bits per byte.
   */
  //  ...
}




// ------------------
// 4. --------- NETWORK: ------------

// -- wifi reconnect --
void wifiReconnect() {
  if (WiFi.status() != WL_CONNECTED) {
    is_online = false;
    // WiFi.disconnect();
    /* - NOTE:
    In most cases, just checking WiFi.status() and calling WiFi.begin(...) when disconnected is enough. 
    But if sometimes ESP32 struggles to reconnect, then adding WiFi.disconnect() will assure clearing the old setup and fresh connection create. */

    // Serial.println("WiFi disconnected!");
    // Serial.print("WiFi reconnect to "); Serial.print(creds.network_ssid); Serial.println("...");
    
    WiFi.begin(creds.network_ssid, creds.network_password);
    int wifi_num_attempts = 0;
    while (WiFi.status() != WL_CONNECTED && wifi_num_attempts < 5) {
      delay(500);  // Short delay to yield control
      Serial.print(". ");
      wifi_num_attempts++;
      // - note: we try 5 times in a loop. Then release the controler to cycle through the other functions and then try again.
      //         ... this prevents the controller to became unfunctional if it looses connection.
    }
  }
  // WiFi Connected ?
  // the reconnect loop brakes for 1 cycle, in order not to block other program. Then returns and tries again.
}

void reconnect(){
  if (millis() - last_reconnect_attempt > reconnect_interval || millis() < last_reconnect_attempt) {
    if (!client.connected()){
      if (is_online){
        // it means the device gets offline from previous online state. Signal with ERR:
        beep(44);
      }
      is_online = false;
      network_state = 0;
      wifiReconnect();
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println(". WiFi connected!");
        Serial.print("Connecting to TCP .");
        network_state = 1;
        int tcp_attempts = 0;
        while (!client.connect(creds.server_ip, creds.server_port) && tcp_attempts < 5){
          delay(500);
          Serial.print(" .");
          tcp_attempts ++;
        }
        if (client.connected()){
          
          Serial.println("Connected! Device is now online.");
          is_online = true;
          network_state = 2;
          // WiFi.setTxPower(WIFI_POWER_8_5dBm);  // Reduce power when idle
        }else{
          Serial.println("TCP FAILED!! Trying again in a momment.");
          
        }
      }
      else{
        // wifi not connected.
        Serial.println(". WIFI not in range! Trying again in a momment.");
        
      }
    }
    last_reconnect_attempt = millis();
  }
}


bool debounceButton(uint8_t pin) {
  uint32_t now = millis();
  // ~ note: we first check for btn-down and then check the time. 
  //         This will give an imediate btn response (considering the debounce time) when btn pressed.

  if (digitalRead(pin) == LOW) {
    if (now - last_btn > btn_update_interval || now < last_btn) {  // debounce time
      last_btn = now;
      return true;
    }
  }
  return false;
}


void btnCheck(){
  if (debounceButton(BTN0)){
    
    Serial.println("Button Pressed. Sending wake-up call to pi...");
    // WiFi.setTxPower(WIFI_POWER_19_5dBm); // <<---- Boost power during data transmission

    uint8_t wakeup_call = 101;

    client.write(&wakeup_call, sizeof(wakeup_call));
    Serial.println("WAKE-UP call sent.");

    // Check for "ready" signal from the server:
    
    uint32_t responseTimeout = millis() + 500;
    uint8_t response = 0;
    // int max_level = 0;  // used when determine the max audio level for different noisy environments and audio sample rate
    while (millis() < responseTimeout){
      if (client.available()) {
        client.read(&response, sizeof(response));  // Read the response from server
        if (response == 202) {
          beep(2);
          ledOnCmd(2);
          Serial.println("server answered 'I am ready'. LISTENING...");

          uint8_t i2s_buffer[BUFFER_MIC];
          size_t bytes_read;
          uint32_t start_time = millis();
          uint32_t last_sound_time = millis();

          while (millis() - start_time < max_recording_time && digitalRead(BTN0)) {  // Stream for few seconds (or until silence detected / button pressed)
              /* NOTE: Also checks for pressing the button again, If so, stops the transmitions. 
                      ~ this is usefull in the noisy environments, where 'silence detection' is not applicable.
              */

              // 1. reading a buffer from microphone...
              esp_err_t result = i2s_read(I2S_PORT_MIC, i2s_buffer, BUFFER_MIC, &bytes_read, pdMS_TO_TICKS(100));
              // ~ note: changing 'portMAX_DELAY' to 'pdMS_TO_TICKS(100)' ensures the function does not block for too long.
              
              // 2. Prepare the buffer to be send over TCP
              // variant 2. Convert to 16 bit and send it. Check for silence inside the conversion.
              bool has_sound = false;
              size_t sample_count = bytes_read / 4;  // 32-bit samples -> each sample is 4 bytes
              uint16_t buffer_16bit[sample_count];   // Buffer for 16-bit samples
              for (size_t i = 0; i < sample_count; i++) {
                  int32_t sample_32bit = ((int32_t*)i2s_buffer)[i];  // Read 32-bit sample
                  buffer_16bit[i] = (sample_32bit >> 16) & 0xFFFF;   // Convert to 16-bit (keep sign)
                  
                  // detect silence...
                  if (abs(sample_32bit) > silence_thresshold) {  // If sample is above threshold. still working with 32bit level!
                      has_sound = true;
                  }
              }

              // Send the converted 16-bit buffer
              client.write((uint8_t*)buffer_16bit, sample_count * 2);  // Each 16-bit sample is 2 bytes

              if (has_sound) {
                  last_sound_time = millis();  // Reset silence timer
              } else if (millis() - last_sound_time > silence_timeout) {
                  Serial.println("Silence detected. Stopping transmission.");
                  break;  // Stop streaming if silence persists
                  // note: tried with sending an end marker, but this complicates the code, because we need to put the end marker inside the last BUFFER_MIC bytes sample.
                  // The server always expects BUFFER_MIC bytes, and simply sending 3 bytes causes problems. So we stop the server listening, by simply waiting the timeout to pass.
              }

              /* 2.... variant 1: not compressed. Sending 32 bit data (16000 samples per seconds, 16khz)
              client.write(i2s_buffer, bytes_read);  // Send data
              

              bool has_sound = false;
              // Check if the buffer contains sound
              for (size_t i = 0; i < bytes_read; i += 4) {  // Assuming 32-bit samples
                  
                  int32_t sample = (i2s_buffer[i]) | (i2s_buffer[i + 1] << 8) | (i2s_buffer[i + 2] << 16);
                  // Convert 24-bit to signed 32-bit (extend sign bit)
                  if (sample & 0x00800000) {  // If 24th bit (sign bit) is set
                      sample |= 0xFF000000;  // Sign-extend to 32-bit
                  }

                  // if (abs(sample) > max_level){
                  //   max_level = abs(sample);
                  // }
                  if (abs(sample) > silence_thresshold) {  // If sample is above threshold
                      has_sound = true;
                      break;
                  }
              }
              if (has_sound) {
                  last_sound_time = millis();  // Reset silence timer
              } else if (millis() - last_sound_time > silence_timeout) {
                  Serial.println("Silence detected. Stopping transmission.");
                  break;  // Stop streaming if silence persists
              }*/

          }
          Serial.println("Audio data sent. Going iddle, waiting for another wake-up call button click.");
          // Serial.print(max_level);
          beep(1);
          ledOnCmd(3);
          break;  // to exit the while loop if we sent the data. 
        }
      }
      delay(50); // Avoid CPU hogging.
    }
    if (!response){
      Serial.println("No answer from the server. Microphone data was not sent. Try again later.");
      beep(44);
    }
    // WiFi.setTxPower(WIFI_POWER_8_5dBm);  // <<--- Reduce power when idle
  }
}

void clientLoop(){
  /* function to check if server has something to say... And if so,
    - 1. server send a binary data containing the audio file name. This acts as a 'server-call' to the client. Data is exact 30 bytes.
    - 2. if client.available() and the bytes to read is 30, this is a call. Extract the data.
    - 3. convert to char aray, which is the file name of the audio data
    - 4. check if same file exists in the SPIFFS already, or the name is the default audio-response name ('mp3respond.mp3'). 
    - 5. if exists, send answer to server '11' , meaning 'Do not send me the audio data. I will play it from my memory.'
    - 6. if not exists / or default, send answer '22' meaning 'Send me the audio data. I will save it here and play it after.);
    - 6.1 save the audio data to SPIFFS, using the name from the server-call
    - 7. Play the audio data, using the audio.connecttoFS(), on the saved mp3 file.
  */
  if (client.connected()) {
    // 1, 2.
    if (client.available() == 29){ // expecting exactly 29 bytese, which is a server-call, containing the name of the audio file (including '.mp3').
      uint8_t buffer[29];
      char filename[30] = {0};  // buffer + 1 for the char string null termination
      // ~ note: // filename array is initialized, with assure that all elements will be 0x00 (null characters), to prevent any potential issues.

      client.readBytes(buffer, 29);  // client.readBytes(buffer, sizeof(buffer));
      // 3.
      convertBinaryToStr(buffer, filename, 29);
      Serial.println(filename);

      // 4. 
      uint8_t file_exist_response = 22;  // 1 byte of a value = 22
      // Set the response flag to 22: ('send me the file!'). Later in the if() this will change if filename != '/mp3response.mp3' or file not fount in the SPFFFS...
      if (strcmp(filename, "/mp3respond.mp3")) { // ~ note: strcmp() returns 0 if 2 arrays mach. but we need the situation when they does not much, so we use it direclty...
        // filename is NOT "/mp3respond.mp3". Check if file exists...
        Serial.println("filename is NOT /mp3respond.mp3. Check if file exists...");
        
        if (SPIFFS.exists(filename)) {
          Serial.println("File exists. not need to be send.");
          file_exist_response = 11;
        } 
      }
      // 5, 6:
      if (file_exist_response == 11){
        Serial.println("Sending: [11] - I have the file, do not send.");
        client.write(&file_exist_response, sizeof(file_exist_response));
        // ...
      }else{
        Serial.println("Sending [22] - you can send me the file!");
        client.write(&file_exist_response, sizeof(file_exist_response));
        // ...
      }
      
      // 6.1 
      if (!filename){
        Serial.println("err: filename is empty. Aborting audio receive.");
        return;
      }
      bool data_received = false;
      uint8_t output_buffer[BUFFER_OUT];
      uint32_t total_audio_samples = 0;

      bool file_err = false;
      // audioFile = SPIFFS.open("/response_audio_filename.mp3", "w");
      // ~ note: the received with the server-call file name is already formated to '/some_file_name.mp3' and it can be directly used...
      audioFile = SPIFFS.open(filename, "w");
      if (!audioFile) {
        Serial.println("Failed to create / recreate the audio file");
        file_err = true;
      }

      // start waiting for the audio data received... Wait for 1 seconds if nothing received, pass.
      uint32_t server_response_timeout = millis() + 1000;
      while (millis() < server_response_timeout){
        size_t bytes_available = client.available();
        if (bytes_available > 0){
          // Serial.println(bytes_available);
          if (!file_err){
            size_t bytesRead = client.read(output_buffer, sizeof(output_buffer));
            total_audio_samples += bytesRead;
            audioFile.write(output_buffer, bytesRead);
          }else{
            Serial.print("err.");
          }

          // directly play the audio samples. Not realy working on mp3.
          // size_t bytes_written;
          // esp_err_t err = i2s_write(I2S_PORT_AUDIO, output_buffer, sizeof(output_buffer), &bytes_written, portMAX_DELAY);
        
          // Serial.println(bytes_written);
          // Serial.println();

          data_received = true;
          server_response_timeout = millis() + 100;  // add a small portion of timeout to prevent breaks on wifi jitters.

        }
      }
      audioFile.close();

      // 7.
      Serial.println("File received. Playing...");
      audio.connecttoFS(SPIFFS, filename);

      // ~ note: playing will cause program held, until the file finish.
      uint32_t duration = total_audio_samples * 1000 * 8 / 64000;  // 8 bits per byte, 1000 milis per second, 64 kbps mp3 format.
      Serial.print("duration to play: "); Serial.println(duration);
      uint32_t begin_time = millis();
      while (millis() - begin_time < duration){
        audio.loop();
      }

      if (!data_received){
        Serial.println("ERR: Server doesn't answered back. Going idle.");
      }else{
        Serial.println("Audio played.");
      }
        
      client.flush();
    }
  }
}

/********* MAIN **************/

void setup() {
    Serial.begin(115200);

    // initialize the on-board led
    strip.begin();   // Initialize the LED strip
    // strip.show();    // Initialize all pixels to 'off' 
    
    led_flash(YELLOW, 2, 300);

    pinMode(BTN0, INPUT_PULLUP);

    pinMode(MAX_SD, OUTPUT);
    delay(50);

    // Disable Bluetooth to save power
    btStop();

    // Setup audio drivers:
    setupMicrophone();
    // setupSpeaker();
    
    memoryInit();
    respondInit();
    
    beep(0);
    led_flash(AQUA, 3, 80);

}

void loop(){

  // audio.loop();
  // plotMicrophone();
  reconnect();
  btnCheck();
  clientLoop();
}

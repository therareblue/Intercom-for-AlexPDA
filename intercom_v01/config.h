 // --- OTA settings, if used. ---
const char* ota_host = "esp32";
const char* ota_ssid = "esp-connection";
const char* ota_password = "esp-password123";
const int ota_channel = 11;
const int ota_ssid_hidden = 0;
const int ota_max_connection = 1;


// --- MQTT / esp-now: ---
struct {
  const char * network_ssid =  "MyTestNetwork";   // name of your WiFi network
  const char * network_password =  "pass-for-wifi";

  const char *server_ip = "192.168.1.200";  // IP address of Raspberry Pi
  const int server_port = 5000;  // Port for TCP connection

} creds;

/*
COMMANDS:
open up
open up the garage door / hidden door/ main door
close it --> use the last open-up device and send close comand to it.
lights on stairs / bbq / fireplace/ hot-tub


*/
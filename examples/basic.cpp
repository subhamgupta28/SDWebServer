#include <SDWebServer.h>

const char* ssid = "YourSSID";
const char* password = "YourPASS";

SDWebServer web;

void setup() {
  web.begin(ssid, password);
}

void loop() {
  // nothing needed
}

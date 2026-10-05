#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "index_OCV_ColorTrack.h"

const char* ssid     = "";
const char* password = "";

// AI-Thinker pin map
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

WiFiServer server(80);

String Feedback = "", Command = "", cmd = "", P1 = "", P2 = "";
byte ReceiveState = 0, cmdState = 1, strState = 1, questionstate = 0, equalstate = 0;

// In .cpp files, functions must be declared before use (the .ino did this for you)
void getCommand(char c);
void ExecuteCommand();

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  Serial.println("\nBooting...");   
  Serial.setDebugOutput(true);

  camera_config_t config = {};          // {} zero-fills unused fields (safer than the tutorial)
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk  = XCLK_GPIO_NUM;
  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href  = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    delay(1000);
    ESP.restart();
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_CIF);   // 400x296, matches the page's probe slider ranges

  WiFi.mode(WIFI_STA);                     // station only is enough here
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("ESP IP Address: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi FAILED - check SSID/password and that it is 2.4 GHz");
  }
  server.begin();
}

void loop() {
  Feedback = Command = cmd = P1 = P2 = "";
  ReceiveState = 0; cmdState = 1; strState = 1; questionstate = 0; equalstate = 0;

  WiFiClient client = server.available();
  if (!client) return;

  String currentLine = "";
  while (client.connected()) {
    if (!client.available()) continue;
    char c = client.read();
    getCommand(c);

    if (c == '\n') {
      if (currentLine.length() == 0) {          // blank line = end of request headers
        if (cmd == "colorDetect") {
          camera_fb_t *fb = esp_camera_fb_get();
          if (!fb) {
            Serial.println("Camera capture failed");
            delay(1000);
            ESP.restart();
          }
          client.println("HTTP/1.1 200 OK");
          client.println("Access-Control-Allow-Origin: *");
          client.println("Access-Control-Allow-Headers: Origin, X-Requested-With, Content-Type, Accept");
          client.println("Access-Control-Allow-Methods: GET,POST,PUT,DELETE,OPTIONS");
          client.println("Content-Type: image/jpeg");
          client.println("Content-Length: " + String(fb->len));
          client.println("Connection: close");
          client.println();

          // send the JPEG in 1 KB chunks
          for (size_t n = 0; n < fb->len; n += 1024) {
            size_t chunk = min((size_t)1024, fb->len - n);
            client.write(fb->buf + n, chunk);
          }
          esp_camera_fb_return(fb);
        } else {
          client.println("HTTP/1.1 200 OK");
          client.println("Access-Control-Allow-Origin: *");
          client.println("Content-Type: text/html; charset=utf-8");
          client.println("Connection: close");
          client.println();
          String data = (cmd != "") ? Feedback : String((const char *)INDEX_HTML);
          for (size_t i = 0; i < data.length(); i += 1000) {
            client.print(data.substring(i, i + 1000));
          }
          client.println();
        }
        Feedback = "";
        break;
      } else {
        currentLine = "";
      }
    } else if (c != '\r') {
      currentLine += c;
    }

    // request line complete ("GET /?cmd=... HTTP/1.1") -> run the command
    if (currentLine.indexOf("/?") != -1 && currentLine.indexOf(" HTTP") != -1) {
      if (Command.indexOf("stop") != -1) {
        client.println();
        client.println();
        client.stop();
      }
      currentLine = "";
      Feedback = "";
      ExecuteCommand();
    }
  }
  delay(1);
  client.stop();
}

void ExecuteCommand() {
  if (cmd == "restart") {
    ESP.restart();
  } else if (cmd == "cm") {
    int x = P1.toInt();
    int y = P2.toInt();
    Serial.printf("Centroid  X=%d  Y=%d\n", x, y);   // <-- the coordinates you want
  } else if (cmd == "quality") {
    esp_camera_sensor_get()->set_quality(esp_camera_sensor_get(), P1.toInt());
  } else if (cmd == "contrast") {
    esp_camera_sensor_get()->set_contrast(esp_camera_sensor_get(), P1.toInt());
  } else if (cmd == "brightness") {
    esp_camera_sensor_get()->set_brightness(esp_camera_sensor_get(), P1.toInt());
  } else {
    Feedback = "Command is not defined.";
  }
  if (Feedback == "") Feedback = Command;
}

// Splits "?cmd=P1;P2;stop" into cmd, P1, P2
void getCommand(char c) {
  if (c == '?') ReceiveState = 1;
  if (c == ' ' || c == '\r' || c == '\n') ReceiveState = 0;

  if (ReceiveState == 1) {
    Command += c;
    if (c == '=') cmdState = 0;
    if (c == ';') strState++;
    if (cmdState == 1 && (c != '?' || questionstate == 1)) cmd += c;
    if (cmdState == 0 && strState == 1 && (c != '=' || equalstate == 1)) P1 += c;
    if (cmdState == 0 && strState == 2 && c != ';') P2 += c;
    if (c == '?') questionstate = 1;
    if (c == '=') equalstate = 1;
  }
}
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>

const char* ssid = "your_wifi_name";
const char* password = "you_wifi_password";

String scriptURL = "YOUR_GOOGLE_APPS_SCRIPT_WEB_APP_URL";

WebServer server(80);

String UID = "unknown";
String LAT = "0";
String LNG = "0";

bool cameraOK = false;

String urlEncode(String str);
void connectWiFi();
void startServer();
bool initCamera();
void takeAndUpload();

String urlEncode(String str) {
  String encoded = "";
  char c;
  char buf[4];

  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);

    if (isalnum(c)) {
      encoded += c;
    } else {
      sprintf(buf, "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }

  return encoded;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  WiFi.begin(ssid, password);

  Serial.print("Connecting WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("Camera IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Gateway: ");
  Serial.println(WiFi.gatewayIP());
  Serial.print("DNS: ");
  Serial.println(WiFi.dnsIP());
}

void startServer() {
  server.on("/", []() {
    server.send(200, "text/plain", "ESP32-CAM alive");
  });

  server.on("/snap", []() {
    UID = server.arg("uid");
    LAT = server.arg("lat");
    LNG = server.arg("lng");

    if (UID == "") UID = "unknown";
    if (LAT == "") LAT = "0";
    if (LNG == "") LNG = "0";

    Serial.println();
    Serial.println("Snap request received");
    Serial.println("UID: " + UID);
    Serial.println("LAT: " + LAT);
    Serial.println("LNG: " + LNG);

    server.send(200, "text/plain", "OK");

    takeAndUpload();
  });

  server.on("/status", []() {
    String s = "";
    s += "WiFi: ";
    s += WiFi.status() == WL_CONNECTED ? "CONNECTED\n" : "DISCONNECTED\n";
    s += "IP: " + WiFi.localIP().toString() + "\n";
    s += "Camera: ";
    s += cameraOK ? "OK\n" : "FAILED\n";
    server.send(200, "text/plain", s);
  });

  server.begin();
  Serial.println("Server started");
}

bool initCamera() {
  Serial.println("Initializing camera...");

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;

  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;

  config.pin_sscb_sda = 26;
  config.pin_sscb_scl = 27;

  config.pin_pwdn = 32;
  config.pin_reset = -1;

  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size = FRAMESIZE_96X96;
  config.jpeg_quality = 35;
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_DRAM;

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    Serial.print("Camera init failed: 0x");
    Serial.println(err, HEX);
    cameraOK = false;
    return false;
  }

  cameraOK = true;
  Serial.println("Camera init success");
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("ESP32-CAM BOOT");

  connectWiFi();
  startServer();
  initCamera();

  Serial.println("SYSTEM READY");
  Serial.print("Use this in RFID code: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/snap");
}

void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost. Reconnecting...");
    WiFi.disconnect();
    delay(500);
    WiFi.begin(ssid, password);
    delay(3000);
  }
}

void takeAndUpload() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi. Upload skipped.");
    return;
  }

  if (!cameraOK) {
    if (!initCamera()) {
      Serial.println("Camera not ready.");
      return;
    }
  }

  Serial.println("Capturing image...");

  camera_fb_t* fb = esp_camera_fb_get();

  if (!fb) {
    Serial.println("Camera capture failed.");
    cameraOK = false;
    esp_camera_deinit();
    delay(500);
    initCamera();
    return;
  }

  Serial.print("Image bytes: ");
  Serial.println(fb->len);

  String imageBase64 = base64::encode(fb->buf, fb->len);

  esp_camera_fb_return(fb);

  Serial.print("Base64 length: ");
  Serial.println(imageBase64.length());

  // IMPORTANT: stop camera before HTTPS upload to free memory
  esp_camera_deinit();
  cameraOK = false;
  delay(500);

  // Make base64 URL-safe
  imageBase64.replace("+", "-");
  imageBase64.replace("/", "_");
  imageBase64.replace("=", ".");

  String url = scriptURL;
  url += "?uid=" + urlEncode(UID);
  url += "&lat=" + urlEncode(LAT);
  url += "&lng=" + urlEncode(LNG);
  url += "&image=" + urlEncode(imageBase64);

  Serial.print("Final URL length: ");
  Serial.println(url.length());

  bool success = false;

  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.print("Google GET attempt ");
    Serial.println(attempt);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(30000);

    HTTPClient http;
    http.setTimeout(30000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, url)) {
      Serial.println("http.begin failed");
      delay(2000);
      continue;
    }

    int code = http.GET();

    Serial.print("Google HTTP Code: ");
    Serial.println(code);

    String reply = http.getString();
    Serial.println("Google Reply:");
    Serial.println(reply);

    if (code == 200 && reply.indexOf("SUCCESS") >= 0) {
      success = true;
      http.end();
      break;
    }

    Serial.print("Error meaning: ");
    Serial.println(http.errorToString(code));

    http.end();
    delay(3000);
  }

  if (success) {
    Serial.println("Final result: Upload successful.");
  } else {
    Serial.println("Final result: Upload failed.");
  }

  // Restart camera after upload
  delay(500);
  initCamera();
}
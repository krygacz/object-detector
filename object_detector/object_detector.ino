#include <esp_camera.h>
#include <esp_int_wdt.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <AccelStepper.h>
#include "Base64.h"
#include "Secrets.h"

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#define LED_PIN           33
#define LED_ON           LOW
#define LED_OFF         HIGH
#define LAMP_PIN           4

camera_config_t cameraConfig;

int interactionCounter = 0;
unsigned long timestamp = 0;
unsigned long lastDetection = 0;
bool motorRunning = false;
bool authOk = false;

WebSocketsClient webSocket;
AccelStepper motor(4, 12, 15, 13, 14);
SemaphoreHandle_t xMutex;

String labels[] = {"orange", "banana", "Granny_Smith"};
int labels_size = 3;

void sendJSON(DynamicJsonDocument doc) {
  doc["id"] = ++interactionCounter;
  String output;
  serializeJson(doc, output);
  webSocket.sendTXT(output);
}

void sendImage() {
  camera_fb_t * fb = NULL;
  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Error: failed to acquire frame from camera");
  }
  size_t fb_len = 0;
  if (fb->format == PIXFORMAT_JPEG) {
    fb_len = fb->len;
    char *input = (char *)fb->buf;
    char output[base64_enc_len(3)];
    String imageFile = "";
    for (int i = 0; i < fb->len; i++) {
      base64_encode(output, (input++), 3);
      if (i % 3 == 0) imageFile += String(output);
    }
    if (imageFile.length() < 10) {
      esp_camera_fb_return(fb);
      fb = NULL;
      delay(50);
      Serial.println("Error: invalid frame");
      sendImage();
      return;
    }
    DynamicJsonDocument doc(32768);
    JsonObject data = doc.createNestedObject("event_data");

    doc["type"] = "fire_event";
    doc["event_type"] = "esp32_ai_event";
    data["image"] = imageFile.c_str();

    while (data["image"].as<String>().equals("null")) {
      delay(10);
      Serial.print(".");
      data["image"] = imageFile.c_str();
    }
    sendJSON(doc);
  } else {
    Serial.println("Error: camera returned non-jpeg image");
  }
  esp_camera_fb_return(fb);
  fb = NULL;
}

void sendAuth() {
  webSocket.sendTXT("{\"type\": \"auth\", \"access_token\": \"" + HA_TOKEN + "\"}");
}

void sendSubscription() {
  DynamicJsonDocument doc(1024);
  doc["type"] = "subscribe_events";
  doc["event_type"] = "esp32_ai_response";
  sendJSON(doc);
}

void authOkCallback() {
  sendSubscription();
}

void checkLabel(JsonArray &predictions) {
  for (JsonVariant v : predictions)
    for (int i = 0; i < labels_size; i++)
      if (labels[i].equals(v["label"].as<String>())) {
        moveMotor(((float)(i + 1) / (float)(labels_size + 1)) * 360);
        lastDetection = millis();
        return;
      }
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("Websocket disconnected");
      break;
    case WStype_CONNECTED:
      Serial.printf("Websocket connected to url: %s\n",  payload);
      break;
    case WStype_TEXT:
      {
        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, (char*)payload);

        String responseType = doc["type"];
        if (responseType.equals("auth_ok")) {
          Serial.println("Authorized");
          authOk = true;
          authOkCallback();
        } else if (responseType.equals("auth_required")) {
          Serial.println("Sending auth...");
          sendAuth();
        } else if (responseType.equals("event")) {
          if (doc["event"]["data"]["data"]["predictions"].isNull() == false) {
            JsonArray predictions = doc["event"]["data"]["data"]["predictions"].as<JsonArray>();

            Serial.println("\npredictions:");
            for (JsonVariant v : predictions) {
              Serial.print("> ");
              Serial.print(v["label"].as<String>());
              Serial.print(": ");
              Serial.println(v["prediction"].as<double>());
            }
            Serial.println("\n");

            checkLabel(predictions);
          } else if (doc["event"]["data"]["data"]["error"].isNull() == true) {
            Serial.println("Error: Thats a big damn error");
            while (true);
          }
        } else if (responseType.equals("result")) {
        } else {
          Serial.println("Error: Wrong response");
          while (true);
        }
      }
      break;
  }
}


void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  Serial.println();

  Serial.println("Starting camera...");
  initCamera();

  Serial.println("Connecting to WiFi...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    counter++;
    if (counter >= 60) {
      Serial.println("\n\nError: WiFi connection failed\n");
      delay(5000);
      ESP.restart();
    }
  }
  
  Serial.println("\nConnecting to websocket...");
  webSocket.beginSSL(HA_URL, 443, "/api/websocket");
  webSocket.onEvent(webSocketEvent);

  Serial.println("\nInitializing motor...");
  motor.setMaxSpeed(1000.0);
  motor.setAcceleration(50.0);
  motor.setSpeed(1000);
  xMutex = xSemaphoreCreateBinary();
  xSemaphoreGive( xMutex );
  moveMotor(0);
  lastDetection = millis();

  Serial.println("Setup done");
}

void moveMotor(float deg) {
  if (xSemaphoreTake( xMutex, ( TickType_t ) 0 ) != pdTRUE) return;
  int stepsToMove = (int)((float)2038 * deg / (float)360);
  xTaskCreatePinnedToCore(
    runMotorTask,
    "Run Motor",
    1000,
    (void*)stepsToMove,
    1,
    NULL,
    0
  );
}

void runMotorTask(void * parameter) {
  disableCore0WDT();
  motor.enableOutputs();
  motor.runToNewPosition((int)parameter);
  motor.disableOutputs();
  xSemaphoreGive( xMutex );
  enableCore0WDT();
  vTaskDelete(NULL);
}

void loop() {
  if (millis() - timestamp >= 2000UL && authOk) {
    timestamp = millis();
    sendImage();
    if (millis() - lastDetection >= 30000UL) {
      lastDetection = millis();
      moveMotor(0);
    }
  }
  webSocket.loop();
}

void initCamera() {
  cameraConfig.ledc_channel = LEDC_CHANNEL_0;
  cameraConfig.ledc_timer = LEDC_TIMER_0;
  cameraConfig.pin_d0 = Y2_GPIO_NUM;
  cameraConfig.pin_d1 = Y3_GPIO_NUM;
  cameraConfig.pin_d2 = Y4_GPIO_NUM;
  cameraConfig.pin_d3 = Y5_GPIO_NUM;
  cameraConfig.pin_d4 = Y6_GPIO_NUM;
  cameraConfig.pin_d5 = Y7_GPIO_NUM;
  cameraConfig.pin_d6 = Y8_GPIO_NUM;
  cameraConfig.pin_d7 = Y9_GPIO_NUM;
  cameraConfig.pin_xclk = XCLK_GPIO_NUM;
  cameraConfig.pin_pclk = PCLK_GPIO_NUM;
  cameraConfig.pin_vsync = VSYNC_GPIO_NUM;
  cameraConfig.pin_href = HREF_GPIO_NUM;
  cameraConfig.pin_sscb_sda = SIOD_GPIO_NUM;
  cameraConfig.pin_sscb_scl = SIOC_GPIO_NUM;
  cameraConfig.pin_pwdn = PWDN_GPIO_NUM;
  cameraConfig.pin_reset = RESET_GPIO_NUM;
  cameraConfig.xclk_freq_hz = 8 * 1000000;
  cameraConfig.pixel_format = PIXFORMAT_JPEG;
  cameraConfig.grab_mode = CAMERA_GRAB_LATEST;

  cameraConfig.frame_size = FRAMESIZE_UXGA;
  cameraConfig.jpeg_quality = 10;
  cameraConfig.fb_count = 2;

  esp_err_t err = esp_camera_init(&cameraConfig);
  if (err != ESP_OK) {
    delay(100);
    Serial.printf("Error: camera failure, reboot in 60s...");
    esp_task_wdt_init(60, true);
    esp_task_wdt_add(NULL);
  } else {
    Serial.println("Camera init done");
    sensor_t * s = esp_camera_sensor_get();
    s->set_framesize(s, FRAMESIZE_SVGA);
  }
}

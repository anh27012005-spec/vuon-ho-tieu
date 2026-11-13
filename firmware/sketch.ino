// Lab 3 - Đề 2: Giám sát môi trường vườn cà phê, hồ tiêu
// ESP32 + DHT22 + FreeRTOS + MQTT

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHTesp.h>

// Cấu hình WiFi và MQTT
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;

// Topic MQTT
#define TOPIC_DATA  "vuoncafe/nanh/data"
#define TOPIC_STATE "vuoncafe/nanh/state"
#define TOPIC_CMD   "vuoncafe/nanh/cmd"

// Chân kết nối
#define DHT_PIN   15
#define MIST_PIN  25   // Máy phun sương (PWM)
#define PUMP_PIN  26   // Máy bơm
#define FAN_PIN   27   // Quạt

// PWM 8 bit, duty từ 0 đến 255
#define PWM_FREQ  5000
#define PWM_RES   8

typedef struct {
  float temperature;
  float humidity;
} SensorData_t;

// Hàng đợi
QueueHandle_t dataQueue;    // Dữ liệu đầy đủ
QueueHandle_t alertQueue;   // Độ ẩm cảnh báo

// Trạng thái thiết bị
volatile bool pumpState    = false;
volatile bool fanState     = false;
volatile int  mistPower    = 0;
volatile bool stateChanged = true;

DHTesp dht;
WiFiClient espClient;
PubSubClient mqtt(espClient);

// Đặt công suất máy phun sương theo phần trăm
void setMist(int percent) {
  int duty = percent * 255 / 100;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(MIST_PIN, duty);
#else
  ledcWrite(0, duty);
#endif
  mistPower = percent;
  stateChanged = true;
}

// Gửi trạng thái thiết bị lên web
void publishState() {
  JsonDocument doc;
  doc["pump"] = pumpState;
  doc["fan"]  = fanState;
  doc["mist"] = mistPower;
  char buf[100];
  serializeJson(doc, buf);
  mqtt.publish(TOPIC_STATE, buf, true);
}

// Nhận lệnh từ web, ví dụ {"device":"pump","state":1}
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;

  const char* device = doc["device"] | "";
  int state = doc["state"] | 0;

  if (strcmp(device, "pump") == 0) {
    pumpState = state;
    digitalWrite(PUMP_PIN, pumpState);
  } else if (strcmp(device, "fan") == 0) {
    fanState = state;
    digitalWrite(FAN_PIN, fanState);
  } else return;

  Serial.printf("[CMD] %s -> %s\n", device, state ? "BAT" : "TAT");
  stateChanged = true;
}

// Kết nối lại MQTT
void reconnectMQTT() {
  String clientId = "ESP32-Cafe-" + String(random(0xffff), HEX);
  Serial.print("[MQTT] Dang ket noi broker... ");
  if (mqtt.connect(clientId.c_str())) {
    Serial.println("OK");
    mqtt.subscribe(TOPIC_CMD);
    stateChanged = true;
  } else {
    Serial.printf("That bai, rc=%d\n", mqtt.state());
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// Task truyền data (Core 0): kết nối WiFi, MQTT và gửi dữ liệu
void TaskTruyenData(void* pv) {
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  Serial.print("[WIFI] Dang ket noi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  Serial.printf("\n[WIFI] Da ket noi, IP: %s\n", WiFi.localIP().toString().c_str());

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  SensorData_t data;
  for (;;) {
    // Mất WiFi thì kết nối lại
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!mqtt.connected()) reconnectMQTT();
    mqtt.loop();

    // Lấy dữ liệu từ dataQueue và gửi lên broker
    if (xQueueReceive(dataQueue, &data, pdMS_TO_TICKS(100)) == pdTRUE) {
      JsonDocument doc;
      doc["temperature"] = data.temperature;
      doc["humidity"]    = data.humidity;
      char buf[100];
      serializeJson(doc, buf);
      mqtt.publish(TOPIC_DATA, buf);
      Serial.printf("[MQTT] Gui: %s\n", buf);
    }

    // Có thay đổi trạng thái thì gửi lên web
    if (stateChanged) {
      stateChanged = false;
      publishState();
    }
  }
}

// Task đọc data (Core 1): đọc DHT22 mỗi 2 giây
void TaskDocData(void* pv) {
  SensorData_t data;
  for (;;) {
    TempAndHumidity th = dht.getTempAndHumidity();

    if (dht.getStatus() == DHTesp::ERROR_NONE) {
      data.temperature = th.temperature;
      data.humidity    = th.humidity;

      Serial.printf("[DHT22] Nhiet do: %.1f C | Do am: %.1f %%\n",
                    data.temperature, data.humidity);

      // Gửi dữ liệu đầy đủ vào dataQueue
      xQueueSend(dataQueue, &data, 0);

      // Độ ẩm dưới 40% thì gửi vào alertQueue
      if (data.humidity < 40) {
        xQueueOverwrite(alertQueue, &data.humidity);
      } else if (data.humidity > 60) {
        Serial.println("[CANH BAO] Do am > 60%");
      }
    } else {
      Serial.printf("[DHT22] Loi doc: %s\n", dht.getStatusString());
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// Task cảnh báo (Core 1): điều khiển máy phun sương theo độ ẩm
void TaskCanhBao(void* pv) {
  float humidity;
  for (;;) {
    if (xQueueReceive(alertQueue, &humidity, portMAX_DELAY) == pdTRUE) {
      int power = 0;
      int timeSec = 0;

      // Dưới 30%: 100% trong 30s; 30-35%: 50% trong 20s; 35-40%: 25% trong 10s
      if (humidity < 30)      { power = 100; timeSec = 30; }
      else if (humidity < 35) { power = 50;  timeSec = 20; }
      else if (humidity < 40) { power = 25;  timeSec = 10; }

      if (power > 0) {
        Serial.printf("[PHUN SUONG] Do am %.1f%% -> BAT %d%% trong %ds\n",
                      humidity, power, timeSec);
        setMist(power);
        vTaskDelay(pdMS_TO_TICKS(timeSec * 1000));
        setMist(0);
        Serial.println("[PHUN SUONG] TAT");

        // Nghỉ 5s rồi mới xét lại
        vTaskDelay(pdMS_TO_TICKS(5000));
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("=== VUON CA PHE - HO TIEU ===");

  pinMode(PUMP_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  digitalWrite(FAN_PIN, LOW);

  // Cấu hình PWM cho máy phun sương
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(MIST_PIN, PWM_FREQ, PWM_RES);
#else
  ledcSetup(0, PWM_FREQ, PWM_RES);
  ledcAttachPin(MIST_PIN, 0);
#endif
  setMist(0);

  dht.setup(DHT_PIN, DHTesp::DHT22);

  // Tạo hàng đợi
  dataQueue  = xQueueCreate(10, sizeof(SensorData_t));
  alertQueue = xQueueCreate(1, sizeof(float));

  // Tạo task: hàm, tên, stack, tham số, ưu tiên, handle, core
  xTaskCreatePinnedToCore(TaskTruyenData, "TruyenData", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskDocData,    "DocData",    4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskCanhBao,    "CanhBao",    4096, NULL, 3, NULL, 1);
}

void loop() {
  vTaskDelete(NULL);
}

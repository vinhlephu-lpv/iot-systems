/* ==============================================================================
 * HE THONG IOT GIAM SAT KHO LANH (MO PHONG BANG THUNG XOP)
 * Vi dieu khien: ESP32 Dev Module (WROOM-32)
 * Cam bien:
 *   - DHT22 (Nhiet do & Do am) - GPIO 4
 *   - MC-38 (Cam bien tu dong/mo cua) - GPIO 18 (INPUT_PULLUP)
 * Co che canh bao thoi gian thuc (FreeRTOS High-Priority Task):
 *   - LED Xanh la: Binh thuong (Cua dong VA Nhiet do an toan)
 *   - LED Vang:    Dang mo cua (> 0s, chua vuot nguong, nhiet do an toan)
 *   - LED Do + Coi: Can xu ly (Cua mo qua lau >= door_delay_sec HOAC Nhiet do vuot nguong)
 * Ket noi: Firebase Realtime Database qua HTTPS REST API
 * ============================================================================== */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>

// ==================== 1. CAU HINH WIFI ====================
const char* WIFI_SSID     = "Thanh Trung";
const char* WIFI_PASSWORD = "10131719";

// ==================== 2. CAU HINH FIREBASE ====================
const char* FIREBASE_HOST = "https://iot-sytems-default-rtdb.firebaseio.com";
const char* FIREBASE_AUTH = "";

#define FB_PATH_SENSOR     "/sensor_data.json"
#define FB_PATH_HISTORY    "/history.json"
#define FB_PATH_THRESHOLDS "/thresholds.json"
#define FB_PATH_ALERTS     "/alerts.json"

const char* SENSOR_ID = "ESP32_KHO_LANH";

// ==================== 3. GPIO ESP32 ====================
#define PIN_DHT22         4    // DATA cam bien DHT22
#define PIN_DOOR_MC38     18   // Cam bien tu MC-38 (INPUT_PULLUP)
#define PIN_LED_GREEN     25   // LED Xanh la - Binh thuong
#define PIN_LED_YELLOW    33   // LED Vang    - Dang mo cua
#define PIN_LED_RED       32   // LED Do      - Can xu ly
#define PIN_LED_ONBOARD   2    // LED onboard ESP32 (Bao WiFi)
#define PIN_BUZZER        16   // Coi buzzer  - Can xu ly
#define ENABLE_BUZZER     false // Dat false de tat coi (che do ban dem/im lang), den do van sang binh thuong

// ==================== 4. CAU HINH CAM BIEN ====================
#define DHTTYPE DHT22
DHT dht(PIN_DHT22, DHTTYPE);

// MC-38 loai NC (Normally Closed khi nam cham ap sat):
//   Cua DONG -> tiep diem dong -> noi GND -> digitalRead = LOW
//   Cua MO   -> tiep diem ho   -> keo 3.3V -> digitalRead = HIGH
const bool IS_MC38_NC = true;

// ==================== 5. NGUONG CANH BAO ====================
// Duoc cap nhat dong tu Firebase /thresholds
volatile float TEMP_MIN = 2.0;
volatile float TEMP_MAX = 8.0;
volatile unsigned long DOOR_DELAY_SEC = 10; // Mac dinh 10s theo yeu cau
// ==================== 6. BIEN TRANG THAI REAL-TIME ====================
volatile float currentTemp = 0.0;
volatile float currentHum  = 0.0;
volatile bool  dhtReady    = false;

volatile bool isDoorOpen = false;
volatile unsigned long doorOpenStartTime = 0;
volatile unsigned long doorOpenDurationSec = 0;

// Trang thai LED & Coi
volatile bool ledGreenState  = false;
volatile bool ledYellowState = false;
volatile bool ledRedState    = false;
volatile bool buzzerActive   = false;

// Muc canh bao: 0 = Binh thuong (Xanh), 1 = Dang mo (Vang), 2 = Can xu ly (Do + Coi)
volatile int currentAlertLevel = 0;
String alarmString = "0";

// Co bao can day du lieu len Firebase ngay (khi doi trang thai)
volatile bool flagNeedPushNow = false;

// Co bao su kien canh bao can day len /alerts tren Firebase
volatile bool flagDoorJustOpened    = false;
volatile bool flagDoorBecameTooLong = false;
volatile bool flagDoorJustClosed    = false;
volatile bool flagTempBecameHigh    = false;
volatile bool flagTempBecameLow     = false;
volatile bool flagTempRecovered     = false;

// Mutex de bao ve du lieu chia se giua 2 Task
portMUX_TYPE stateMutex = portMUX_INITIALIZER_UNLOCKED;

// ==================== 7. SSL & HTTP CLIENTS ====================
// Kenh 1: Chuyen gui sensor_data real-time (Keep-Alive ~40-80ms/lan)
WiFiClientSecure sslSensor;
HTTPClient httpSensor;

// Kenh 2: Chuyen ghi lich su history dinh ky 5s/lan (Keep-Alive ~40-80ms/lan)
WiFiClientSecure sslHistory;
HTTPClient httpHistory;

// Kenh 3: Chuyen doc nguong thresholds moi 1.5s (Keep-Alive ~30-50ms/lan)
WiFiClientSecure sslThresh;
HTTPClient httpThresh;

// ==================== HAM DOC CAM BIEN CUA MC-38 ====================
inline bool readDoorState() {
  int pinVal = digitalRead(PIN_DOOR_MC38);
  return IS_MC38_NC ? (pinVal == HIGH) : (pinVal == LOW);
}

// ==================== HAM DIEU KHIEN LED + COI ====================
void updateOutputs(int level) {
  bool g = (level == 0);
  bool y = (level == 1);
  bool r = (level == 2);
  bool b = (level == 2);

  if (ledGreenState != g) {
    ledGreenState = g;
    digitalWrite(PIN_LED_GREEN, g ? HIGH : LOW);
  }
  if (ledYellowState != y) {
    ledYellowState = y;
    digitalWrite(PIN_LED_YELLOW, y ? HIGH : LOW);
  }
  if (ledRedState != r) {
    ledRedState = r;
    digitalWrite(PIN_LED_RED, r ? HIGH : LOW);
  }
  if (PIN_BUZZER >= 0 && buzzerActive != b) {
    buzzerActive = b;
    #if ENABLE_BUZZER
    digitalWrite(PIN_BUZZER, b ? HIGH : LOW);
    #else
    digitalWrite(PIN_BUZZER, LOW); // Luon giu muc LOW de tat coi
    #endif
  }
}

// ==================== HAM TAO CHUOI ALARM ====================
String buildAlarmString(int level) {
  if (level == 2) {
    if (dhtReady && currentTemp > TEMP_MAX) return "TEMP_HIGH";
    if (dhtReady && currentTemp < TEMP_MIN) return "TEMP_LOW";
    if (isDoorOpen && doorOpenDurationSec >= DOOR_DELAY_SEC) return "DOOR_OPEN_LONG";
    return "2";
  }
  if (level == 1) return "1";
  return "0";
}

// ==============================================================================
// TASK PHAN CUNG REAL-TIME (FREERTOS TASK - PRIORITY 2)
// Chay doc lap tren Core 1, chu ky 20ms, tuyet doi khong bi block boi mang!
// Khi cua mo du 10s (DOOR_DELAY_SEC), coi va den do se bat dung mili-giay!
// ==============================================================================
void hardwareAlarmTask(void *pvParameters) {
  bool lastDoor = false;

  while (true) {
    bool doorNow = readDoorState();
    unsigned long now = millis();

    portENTER_CRITICAL(&stateMutex);

    static bool doorDelayExceeded = false;
    static bool lastTempHigh = false;
    static bool lastTempLow  = false;

    // 1. Kiem tra thay doi cua
    if (doorNow) {
      if (!isDoorOpen) {
        // Vua mo cua
        isDoorOpen = true;
        doorOpenStartTime = now;
        doorOpenDurationSec = 0;
        doorDelayExceeded = false;
        flagDoorJustOpened = true;
        flagNeedPushNow = true;
      } else {
        // Cua van dang mo
        doorOpenDurationSec = (now - doorOpenStartTime) / 1000;
        if (doorOpenDurationSec >= DOOR_DELAY_SEC && !doorDelayExceeded) {
          doorDelayExceeded = true;
          flagDoorBecameTooLong = true;
          flagNeedPushNow = true;
        }
      }
    } else {
      if (isDoorOpen) {
        // Vua dong cua
        isDoorOpen = false;
        doorOpenDurationSec = 0;
        doorDelayExceeded = false;
        flagDoorJustClosed = true;
        flagNeedPushNow = true;
      }
    }

    // 2. Danh gia trang thai canh bao
    bool isTempHigh = dhtReady && (currentTemp > TEMP_MAX);
    bool isTempLow  = dhtReady && (currentTemp < TEMP_MIN);

    if (dhtReady) {
      if (isTempHigh && !lastTempHigh) {
        lastTempHigh = true;
        lastTempLow = false;
        flagTempBecameHigh = true;
        flagNeedPushNow = true;
      } else if (isTempLow && !lastTempLow) {
        lastTempLow = true;
        lastTempHigh = false;
        flagTempBecameLow = true;
        flagNeedPushNow = true;
      } else if (!isTempHigh && !isTempLow && (lastTempHigh || lastTempLow)) {
        lastTempHigh = false;
        lastTempLow = false;
        flagTempRecovered = true;
        flagNeedPushNow = true;
      }
    }

    bool isTempAlert = isTempHigh || isTempLow;
    bool isDoorTooLong = isDoorOpen && (doorOpenDurationSec >= DOOR_DELAY_SEC);

    int targetLevel = 0;
    if (isTempAlert || isDoorTooLong) {
      targetLevel = 2; // Do + Coi
    } else if (isDoorOpen) {
      targetLevel = 1; // Vang
    } else {
      targetLevel = 0; // Xanh
    }

    if (targetLevel != currentAlertLevel) {
      currentAlertLevel = targetLevel;
      flagNeedPushNow = true;
    }

    // 3. Kich hoat phan cung tuc thi
    updateOutputs(currentAlertLevel);

    portEXIT_CRITICAL(&stateMutex);

    // In log khi co su thay doi cua de theo doi qua Serial
    if (doorNow != lastDoor) {
      lastDoor = doorNow;
      Serial.printf("[CUA] -> %s | Nguong cho phep: %lus | Level: %d\n",
                    doorNow ? "DANG MO" : "DA DONG", DOOR_DELAY_SEC, currentAlertLevel);
    }

    vTaskDelay(pdMS_TO_TICKS(20)); // Delay 20ms
  }
}

// ==================== HAM TAO URL FIREBASE ====================
String buildFirebaseUrl(const char* path) {
  String url = String(FIREBASE_HOST) + String(path);
  if (strlen(FIREBASE_AUTH) > 0) {
    url += "?auth=" + String(FIREBASE_AUTH);
  }
  return url;
}

// ==================== PARSE JSON DON GIAN ====================
bool parseJsonFloat(const String& json, const String& key, float& result) {
  String searchKey = "\"" + key + "\":";
  int idx = json.indexOf(searchKey);
  if (idx < 0) return false;

  int start = idx + searchKey.length();
  while (start < (int)json.length() && json[start] == ' ') start++;

  int end = start;
  while (end < (int)json.length() && (json[end] == '-' || json[end] == '.' || (json[end] >= '0' && json[end] <= '9'))) {
    end++;
  }

  if (end > start) {
    result = json.substring(start, end).toFloat();
    return true;
  }
  return false;
}

// ==================== GUI DU LIEU LEN FIREBASE (SENSOR_DATA) ====================
void sendSensorData() {
  if (WiFi.status() != WL_CONNECTED) return;

  float t, h;
  bool door;
  unsigned long doorSec;
  int level;

  portENTER_CRITICAL(&stateMutex);
  t = currentTemp;
  h = currentHum;
  door = isDoorOpen;
  doorSec = door ? doorOpenDurationSec : 0;
  level = currentAlertLevel;
  alarmString = buildAlarmString(level);
  portEXIT_CRITICAL(&stateMutex);

  String json = "{";
  json += "\"temperature_c\":" + String(t, 1) + ",";
  json += "\"humidity\":" + String(h, 1) + ",";
  json += "\"door_status\":\"" + String(door ? "OPEN" : "CLOSED") + "\",";
  json += "\"door_open_sec\":" + String(doorSec) + ",";
  json += "\"alarm\":\"" + alarmString + "\",";
  json += "\"sensor_id\":\"" + String(SENSOR_ID) + "\",";
  json += "\"timestamp\":{\".sv\":\"timestamp\"}";
  json += "}";

  String url = buildFirebaseUrl(FB_PATH_SENSOR);

  if (!httpSensor.connected()) {
    httpSensor.begin(sslSensor, url);
    httpSensor.setReuse(true);
    httpSensor.setConnectTimeout(2500);
    httpSensor.setTimeout(2500);
  }
  httpSensor.addHeader("Content-Type", "application/json");

  int code = httpSensor.PUT(json);
  if (code > 0) {
    httpSensor.getString(); // Doc sach phan hoi de giu ket noi san sang cho lan sau
  } else {
    Serial.printf("[FIREBASE] Loi gui sensor_data: %s (%d)\n", httpSensor.errorToString(code).c_str(), code);
    httpSensor.end();
    sslSensor.stop();
  }
}

// ==================== DOC NGUONG CANH BAO TU FIREBASE ====================
// ==================== PUSH LICH SU LEN FIREBASE (HISTORY) ====================
void pushHistory() {
  if (WiFi.status() != WL_CONNECTED) return;

  float t, h;
  bool door;
  unsigned long doorSec;
  int level;

  portENTER_CRITICAL(&stateMutex);
  t = currentTemp;
  h = currentHum;
  door = isDoorOpen;
  doorSec = door ? doorOpenDurationSec : 0;
  level = currentAlertLevel;
  portEXIT_CRITICAL(&stateMutex);

  String curAlarm = buildAlarmString(level);

  String json = "{";
  json += "\"temperature_c\":" + String(t, 1) + ",";
  json += "\"humidity\":" + String(h, 1) + ",";
  json += "\"door_status\":\"" + String(door ? "OPEN" : "CLOSED") + "\",";
  json += "\"door_open_sec\":" + String(doorSec) + ",";
  json += "\"alarm\":\"" + curAlarm + "\",";
  json += "\"timestamp\":{\".sv\":\"timestamp\"}";
  json += "}";

  String url = buildFirebaseUrl(FB_PATH_HISTORY);

  if (!httpHistory.connected()) {
    httpHistory.begin(sslHistory, url);
    httpHistory.setReuse(true);
    httpHistory.setConnectTimeout(2500);
    httpHistory.setTimeout(2500);
  }
  httpHistory.addHeader("Content-Type", "application/json");

  int code = httpHistory.POST(json);
  if (code > 0) {
    httpHistory.getString();
    Serial.printf("[HISTORY] Ghi lich su -> HTTP %d (Temp: %.1f, Cua: %s, Sec: %lu)\n", code, t, door ? "OPEN" : "CLOSED", doorSec);
  } else {
    Serial.printf("[HISTORY] Loi ghi lich su: %s (%d)\n", httpHistory.errorToString(code).c_str(), code);
    httpHistory.end();
    sslHistory.stop();
  }
}

// ==================== PUSH CANH BAO LEN FIREBASE (/alerts) ====================
void pushAlert(const char* type, const char* severity, const char* message) {
  if (WiFi.status() != WL_CONNECTED) return;

  String json = "{";
  json += "\"type\":\"" + String(type) + "\",";
  json += "\"severity\":\"" + String(severity) + "\",";
  json += "\"message\":\"" + String(message) + "\",";
  json += "\"timestamp\":{\".sv\":\"timestamp\"}";
  json += "}";

  String url = buildFirebaseUrl(FB_PATH_ALERTS);
  httpHistory.end();
  httpHistory.begin(sslHistory, url);
  httpHistory.setConnectTimeout(2500);
  httpHistory.setTimeout(2500);
  httpHistory.addHeader("Content-Type", "application/json");

  int code = httpHistory.POST(json);
  if (code > 0) {
    httpHistory.getString();
    Serial.printf("[ALERT] Firebase POST -> %s: %s (HTTP %d)\n", severity, message, code);
  } else {
    Serial.printf("[ALERT] Loi ghi Firebase: %s (%d)\n", httpHistory.errorToString(code).c_str(), code);
    sslHistory.stop();
  }
  httpHistory.end();
}

// ==================== DOC NGUONG CANH BAO TU FIREBASE ====================
void readThresholds() {
  if (WiFi.status() != WL_CONNECTED) return;

  String url = buildFirebaseUrl(FB_PATH_THRESHOLDS);
  if (!httpThresh.connected()) {
    httpThresh.begin(sslThresh, url);
    httpThresh.setReuse(true);
    httpThresh.setConnectTimeout(2000);
    httpThresh.setTimeout(2000);
  }

  int code = httpThresh.GET();
  if (code == 200) {
    String payload = httpThresh.getString();

    if (payload != "null" && payload.length() > 5) {
      float tmin, tmax, dsec;
      bool changed = false;

      if (parseJsonFloat(payload, "temp_min", tmin)) {
        if (TEMP_MIN != tmin) { TEMP_MIN = tmin; changed = true; }
      }
      if (parseJsonFloat(payload, "temp_max", tmax)) {
        if (TEMP_MAX != tmax) { TEMP_MAX = tmax; changed = true; }
      }
      if (parseJsonFloat(payload, "door_delay_sec", dsec)) {
        unsigned long val = (unsigned long)dsec;
        if (DOOR_DELAY_SEC != val) {
          DOOR_DELAY_SEC = val;
          changed = true;
          Serial.printf("[NGUONG MOI] Cua cho phep: %lu giay\n", DOOR_DELAY_SEC);
        }
      }

      if (changed) {
        Serial.printf("[NGUONG MOI] Temp: [%.1f - %.1f]C | Cua: %lu giay\n", TEMP_MIN, TEMP_MAX, DOOR_DELAY_SEC);
        portENTER_CRITICAL(&stateMutex);
        flagNeedPushNow = true;
        // Cap nhat phan ung den LED ngay tuc khac khi nguong thay doi!
        bool isTempAlert = dhtReady && (currentTemp < TEMP_MIN || currentTemp > TEMP_MAX);
        bool isDoorTooLong = isDoorOpen && (doorOpenDurationSec >= DOOR_DELAY_SEC);
        if (isTempAlert || isDoorTooLong) {
          currentAlertLevel = 2;
        } else if (isDoorOpen) {
          currentAlertLevel = 1;
        } else {
          currentAlertLevel = 0;
        }
        portEXIT_CRITICAL(&stateMutex);
        updateOutputs(currentAlertLevel);
      }
    }
  } else {
    httpThresh.end();
    sslThresh.stop();
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n========================================================");
  Serial.println("  HE THONG IOT GIAM SAT KHO LANH - ESP32 REAL-TIME");
  Serial.println("  Trang thai: Binh thuong(Xanh) | Mo cua(Vang) | Nguy hiem(Do+Coi)");
  Serial.println("========================================================\n");

  // Cau hinh GPIO
  pinMode(PIN_DOOR_MC38, INPUT_PULLUP);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_ONBOARD, OUTPUT);
  if (PIN_BUZZER >= 0) pinMode(PIN_BUZZER, OUTPUT);

  // Khoi tao dau ra
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_YELLOW, LOW);
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_ONBOARD, LOW);
  if (PIN_BUZZER >= 0) digitalWrite(PIN_BUZZER, LOW);

  // Khoi dong DHT22
  dht.begin();
  Serial.println("[DHT22] Da khoi dong cam bien.");

  // SSL Setup cho 3 kenh Keep-Alive
  sslSensor.setInsecure();
  sslSensor.setTimeout(3);
  sslHistory.setInsecure();
  sslHistory.setTimeout(3);
  sslThresh.setInsecure();
  sslThresh.setTimeout(3);

  // KHOI TAO TASK PHAN CUNG REAL-TIME (FreeRTOS)
  // Uu tien cao (Priority 2) de chay ngay ca khi mang dang goi
  xTaskCreatePinnedToCore(
    hardwareAlarmTask,
    "hardwareAlarmTask",
    4096,
    NULL,
    2,    // Priority cao hon loop()
    NULL,
    1     // Core 1 (cung core voi Arduino loop nhung uu tien cao hon)
  );
  Serial.println("[FREERTOS] Task phan cung real-time da duoc tao (20ms).");

  // Ket noi WiFi
  Serial.printf("[WIFI] Dang ket noi toi: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 25) {
    delay(400);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Ket noi thanh cong! IP: " + WiFi.localIP().toString());
    digitalWrite(PIN_LED_ONBOARD, HIGH);
  } else {
    Serial.println("\n[WIFI] Chua the ket noi WiFi, se tu dong thu lai.");
  }

  // Doc nguong khoi dau
  readThresholds();

  // Doc cam bien khoi dau de co du lieu ngay lap tuc
  float initT = dht.readTemperature();
  float initH = dht.readHumidity();
  if (!isnan(initT) && !isnan(initH)) {
    portENTER_CRITICAL(&stateMutex);
    currentTemp = initT;
    currentHum  = initH;
    dhtReady    = true;
    portEXIT_CRITICAL(&stateMutex);
    Serial.printf("[DHT22] Khoi dau: Temp=%.1f C, Hum=%.1f %%\n", initT, initH);
  }
}

// ==================== LOOP (CHAY CAC TAC VU MANG & CAM BIEN) ====================
void loop() {
  unsigned long now = millis();

  static unsigned long lastDhtRead = 0;
  static unsigned long lastSensorPush = 0;
  static unsigned long lastThresholdRead = 0;
  static unsigned long lastHistoryPush = 0;

  // 1. Doc DHT22 dinh ky moi 2 giay (LUON CHAY DAU TIEN, KHONG BI CHAN BOI RETURN)
  if (now - lastDhtRead >= 2000) {
    lastDhtRead = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      portENTER_CRITICAL(&stateMutex);
      currentTemp = t;
      currentHum  = h;
      dhtReady    = true;
      portEXIT_CRITICAL(&stateMutex);
    }
  }

  // 2. KHI CUA VUA DOI TRANG THAI: GUI NGAY LAP TUC CHO CA SENSOR, HISTORY & ALERTS
  if (flagNeedPushNow) {
    flagNeedPushNow = false;
    lastSensorPush = now;
    sendSensorData();
    pushHistory();
    lastHistoryPush = now;

    // Day canh bao tuong ung len Firebase /alerts
    if (flagDoorJustOpened) {
      flagDoorJustOpened = false;
      char msg[80];
      snprintf(msg, sizeof(msg), "đang mở cửa < %lus", DOOR_DELAY_SEC);
      pushAlert("door", "warning", msg);
    }
    if (flagDoorBecameTooLong) {
      flagDoorBecameTooLong = false;
      pushAlert("door", "danger", "cần xử lý - cửa mở quá lâu");
    }
    if (flagDoorJustClosed) {
      flagDoorJustClosed = false;
      pushAlert("door", "ok", "Cửa đã đóng");
    }
    if (flagTempBecameHigh) {
      flagTempBecameHigh = false;
      char msg[80];
      snprintf(msg, sizeof(msg), "Nhiệt độ vượt ngưỡng %.1f > %.1f°C", currentTemp, TEMP_MAX);
      pushAlert("temp", "danger", msg);
    }
    if (flagTempBecameLow) {
      flagTempBecameLow = false;
      char msg[80];
      snprintf(msg, sizeof(msg), "Nhiệt độ dưới ngưỡng %.1f < %.1f°C", currentTemp, TEMP_MIN);
      pushAlert("temp", "danger", msg);
    }
    if (flagTempRecovered) {
      flagTempRecovered = false;
      char msg[80];
      snprintf(msg, sizeof(msg), "Nhiệt độ đã trở lại an toàn (%.1f°C)", currentTemp);
      pushAlert("temp", "ok", msg);
    }

    delay(10);
    return;
  }

  // 3. GUI SENSOR_DATA REAL-TIME:
  // - Khi cua mo: gui deu dan moi 1 giay (1000ms) de Firebase & Web nhan dung so giay thuc
  // - Khi cua dong: gui heartbeat moi 3 giay (3000ms) de cap nhat nhiet do va giu song ket noi
  unsigned long sensorInterval = isDoorOpen ? 1000 : 3000;
  if (now - lastSensorPush >= sensorInterval) {
    lastSensorPush = now;
    sendSensorData();
  }

  // 4. GHI LICH SU (HISTORY): CHUAN DINH KY 5 GIAY / LAN (Ke ca khi mo cua hay dong cua)
  if (now - lastHistoryPush >= 5000) {
    lastHistoryPush = now;
    pushHistory();
  }

  // 5. DOC NGUONG TU FIREBASE: Kiem tra lien tuc moi 1.5s (Keep-Alive toc do ~30-50ms)
  if (now - lastThresholdRead >= 1500) {
    lastThresholdRead = now;
    readThresholds();
  }

  // 7. Kiem tra ket noi WiFi
  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(PIN_LED_ONBOARD, LOW);
    static unsigned long lastWifiRetry = 0;
    if (now - lastWifiRetry >= 8000) {
      lastWifiRetry = now;
      Serial.println("[WIFI] Mat ket noi, dang thu ket noi lai...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  } else {
    digitalWrite(PIN_LED_ONBOARD, HIGH);
  }

  delay(20);
}


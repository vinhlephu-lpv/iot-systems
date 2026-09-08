/* ==============================================================================
 * HE THONG IOT GIAM SAT KHO LANH (MO PHONG BANG THUNG XOP)
 * Vi dieu khien: ESP32 Dev Module (WROOM-32)
 * Cam bien:
 *   - DHT22 (Nhiet do & Do am)
 *   - MC-38 (Cam bien tu dong/mo cua)
 * Canh bao 3 muc:
 *   - LED Xanh la (Muc 1): Canh bao nhe
 *   - LED Vang   (Muc 2): Canh bao trung binh
 *   - LED Do     (Muc 3): Nguy hiem + Coi bao
 * Ket noi: Firebase Realtime Database qua HTTPS REST API
 *
 * Firebase Data Structure:
 *   /sensor_data   <- ESP32 ghi du lieu hien tai (PUT)
 *   /history       <- ESP32 push moi 5 giay (POST)
 *   /thresholds    <- Web ghi, ESP32 doc (GET)
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

// Cac duong dan Firebase khop voi Web Dashboard
// Web doc: /sensor_data
// Web doc: /history
// Web ghi: /thresholds -> ESP32 doc de cap nhat nguong
#define FB_PATH_SENSOR    "/sensor_data.json"
#define FB_PATH_HISTORY   "/history.json"
#define FB_PATH_THRESHOLDS "/thresholds.json"

const char* SENSOR_ID = "ESP32_KHO_LANH";

// ==================== 3. GPIO ESP32 ====================
#define PIN_DHT22         4    // DATA cam bien DHT22
#define PIN_DOOR_MC38     18   // Cam bien tu MC-38 (INPUT_PULLUP)
#define PIN_LED_GREEN     25   // LED Xanh la - Muc 1
#define PIN_LED_YELLOW    33   // LED Vang    - Muc 2
#define PIN_LED_RED       32   // LED Do      - Muc 3
#define PIN_LED_ONBOARD   2    // LED onboard ESP32
#define PIN_BUZZER        26   // Coi buzzer  (dat -1 neu chua lap)

// ==================== 4. CAU HINH CAM BIEN ====================
#define DHTTYPE DHT22
DHT dht(PIN_DHT22, DHTTYPE);

// MC-38 loai NC (Normally Closed khi nam cham ap sat):
//   Cua DONG -> tiep diem dong -> noi GND -> digitalRead = LOW
//   Cua MO   -> tiep diem ho   -> keo 3.3V -> digitalRead = HIGH
const bool IS_MC38_NC = true;

// Che do test: bat tat ca LED de kiem tra mach
const bool TEST_ALL_LEDS_ON = false;

// ==================== 5. NGUONG CANH BAO MAC DINH ====================
// Cac gia tri nay se duoc cap nhat tu Firebase /thresholds
// Khop voi Web Dashboard: 3 muc canh bao

// Nhiet do
float TEMP_LV1_MIN =  2.0;  // Muc 1: duoi 2°C -> LED xanh
float TEMP_LV1_MAX =  8.0;  // Muc 1: tren 8°C -> LED xanh
float TEMP_LV2_MIN =  0.0;  // Muc 2: duoi 0°C -> LED vang
float TEMP_LV2_MAX = 10.0;  // Muc 2: tren 10°C -> LED vang
float TEMP_LV3_MIN = -2.0;  // Muc 3: duoi -2°C -> LED do + coi
float TEMP_LV3_MAX = 15.0;  // Muc 3: tren 15°C -> LED do + coi

// Cua (giay)
unsigned long DOOR_LV1_SEC = 30;   // Muc 1: > 30s -> LED xanh
unsigned long DOOR_LV2_SEC = 60;   // Muc 2: > 60s -> LED vang
unsigned long DOOR_LV3_SEC = 120;  // Muc 3: > 120s -> LED do + coi

// ==================== 6. THOI GIAN ====================
const unsigned long FB_SEND_INTERVAL    = 5000;  // Gui du lieu moi 5 giay
const unsigned long FB_THRESHOLD_INTERVAL = 3000;  // Doc nguong tu Firebase moi 3 giay
const unsigned long DHT_READ_INTERVAL   = 2000;  // Doc DHT22 moi 2 giay

// ==================== 7. BIEN TRANG THAI ====================
float currentTemp = 0.0;
float currentHum  = 0.0;
bool  dhtReady    = false;

bool isDoorOpen = false;
bool lastDoorOpenState = false;
unsigned long doorOpenStartTime = 0;
unsigned long doorOpenDurationSec = 0;

// Trang thai LED
bool ledGreenState  = false;
bool ledYellowState = false;
bool ledRedState    = false;
bool buzzerActive   = false;

// Muc canh bao hien tai: 0 = binh thuong, 1 = muc 1, 2 = muc 2, 3 = muc 3
int currentAlertLevel = 0;
String alarmString = "NONE";

// Timers
unsigned long lastDhtReadTime = 0;
unsigned long lastFirebaseSendTime = 0;
unsigned long lastThresholdReadTime = 0;
unsigned long lastBuzzerToggle = 0;
bool buzzerTone = false;

// SSL Client
WiFiClientSecure sslClient;

// ==================== HAM DOC CAM BIEN CUA MC-38 ====================
bool readDoorState() {
  int pinVal = digitalRead(PIN_DOOR_MC38);
  if (IS_MC38_NC) {
    return (pinVal == HIGH);
  } else {
    return (pinVal == LOW);
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

// ==================== HAM XAC DINH MUC CANH BAO ====================
// Tra ve muc canh bao cao nhat (0-3) tu nhiet do va cua
int calculateAlertLevel() {
  int level = 0;

  // Kiem tra nhiet do: muc 3 > muc 2 > muc 1
  if (dhtReady) {
    if (currentTemp < TEMP_LV3_MIN || currentTemp > TEMP_LV3_MAX) {
      level = 3;
    } else if (currentTemp < TEMP_LV2_MIN || currentTemp > TEMP_LV2_MAX) {
      if (level < 2) level = 2;
    } else if (currentTemp < TEMP_LV1_MIN || currentTemp > TEMP_LV1_MAX) {
      if (level < 1) level = 1;
    }
  }

  // Kiem tra cua: muc 3 > muc 2 > muc 1
  if (isDoorOpen) {
    if (doorOpenDurationSec >= DOOR_LV3_SEC) {
      level = 3;
    } else if (doorOpenDurationSec >= DOOR_LV2_SEC) {
      if (level < 2) level = 2;
    } else if (doorOpenDurationSec >= DOOR_LV1_SEC) {
      if (level < 1) level = 1;
    }
  }

  return level;
}

// ==================== HAM TAO CHUOI ALARM ====================
// Tao chuoi alarm gui len Firebase de Web hieu
String buildAlarmString(int level) {
  if (level == 0) return "NONE";

  // Uu tien nhiet do truoc, cua sau
  if (level == 3) {
    if (currentTemp > TEMP_LV3_MAX) return "TEMP_HIGH";
    if (currentTemp < TEMP_LV3_MIN) return "TEMP_LOW";
    if (isDoorOpen && doorOpenDurationSec >= DOOR_LV3_SEC) return "DOOR_OPEN_LONG";
  }
  if (level == 2) {
    if (currentTemp > TEMP_LV2_MAX) return "TEMP_HIGH";
    if (currentTemp < TEMP_LV2_MIN) return "TEMP_LOW";
    if (isDoorOpen && doorOpenDurationSec >= DOOR_LV2_SEC) return "DOOR_WARN";
  }
  if (level == 1) {
    if (currentTemp > TEMP_LV1_MAX) return "TEMP_HIGH";
    if (currentTemp < TEMP_LV1_MIN) return "TEMP_LOW";
    if (isDoorOpen && doorOpenDurationSec >= DOOR_LV1_SEC) return "DOOR_NOTICE";
  }

  return "NONE";
}

// ==================== HAM DIEU KHIEN LED + COI ====================
void updateOutputs(int level) {
  if (TEST_ALL_LEDS_ON) {
    digitalWrite(PIN_LED_GREEN, HIGH);
    digitalWrite(PIN_LED_YELLOW, HIGH);
    digitalWrite(PIN_LED_RED, HIGH);
    ledGreenState = ledYellowState = ledRedState = true;
    return;
  }

  // Tat het truoc
  ledGreenState  = false;
  ledYellowState = false;
  ledRedState    = false;
  buzzerActive   = false;

  switch (level) {
    case 3:
      ledRedState  = true;
      buzzerActive = true;
      break;
    case 2:
      ledYellowState = true;
      break;
    case 1:
      ledGreenState = true;
      break;
    default:
      // Muc 0: tat het -> binh thuong
      break;
  }

  digitalWrite(PIN_LED_GREEN,  ledGreenState  ? HIGH : LOW);
  digitalWrite(PIN_LED_YELLOW, ledYellowState ? HIGH : LOW);
  digitalWrite(PIN_LED_RED,    ledRedState    ? HIGH : LOW);

  // Coi: keu ngat quang khi muc 3
  if (PIN_BUZZER >= 0) {
    if (buzzerActive) {
      // Keu ngat quang moi 500ms
      if (millis() - lastBuzzerToggle >= 500) {
        lastBuzzerToggle = millis();
        buzzerTone = !buzzerTone;
        digitalWrite(PIN_BUZZER, buzzerTone ? HIGH : LOW);
      }
    } else {
      digitalWrite(PIN_BUZZER, LOW);
      buzzerTone = false;
    }
  }
}

// ==================== GUI DU LIEU LEN FIREBASE ====================
void sendSensorData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[FIREBASE] WiFi chua ket noi, bo qua.");
    return;
  }

  // JSON khop voi Web Dashboard (app.js onValue listener)
  // Fields: temperature_c, humidity, door_status, door_open_sec, alarm, sensor_id, timestamp
  String json = "{";
  json += "\"temperature_c\":" + String(currentTemp, 1) + ",";
  json += "\"humidity\":" + String(currentHum, 1) + ",";
  json += "\"door_status\":\"" + String(isDoorOpen ? "OPEN" : "CLOSED") + "\",";
  json += "\"door_open_sec\":" + String(doorOpenDurationSec) + ",";
  json += "\"alarm\":\"" + alarmString + "\",";
  json += "\"sensor_id\":\"" + String(SENSOR_ID) + "\",";
  json += "\"timestamp\":{\".\u0073v\":\"timestamp\"}";
  json += "}";

  HTTPClient http;
  String url = buildFirebaseUrl(FB_PATH_SENSOR);

  http.begin(sslClient, url);
  http.addHeader("Content-Type", "application/json");

  // PUT de cap nhat /sensor_data
  int code = http.PUT(json);

  if (code > 0) {
    Serial.printf("[FIREBASE] sensor_data -> HTTP %d\n", code);
  } else {
    Serial.printf("[FIREBASE] Loi gui sensor_data: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

// ==================== PUSH LICH SU LEN FIREBASE ====================
void pushHistory() {
  if (WiFi.status() != WL_CONNECTED) return;

  String json = "{";
  json += "\"temperature_c\":" + String(currentTemp, 1) + ",";
  json += "\"humidity\":" + String(currentHum, 1) + ",";
  json += "\"door_status\":\"" + String(isDoorOpen ? "OPEN" : "CLOSED") + "\",";
  json += "\"door_open_sec\":" + String(doorOpenDurationSec) + ",";
  json += "\"alarm\":\"" + alarmString + "\",";
  json += "\"timestamp\":{\".\u0073v\":\"timestamp\"}";
  json += "}";

  HTTPClient http;
  String url = buildFirebaseUrl(FB_PATH_HISTORY);

  http.begin(sslClient, url);
  http.addHeader("Content-Type", "application/json");

  // POST de tao ban ghi moi (auto-generated key)
  int code = http.POST(json);

  if (code > 0) {
    Serial.printf("[FIREBASE] history -> HTTP %d\n", code);
  } else {
    Serial.printf("[FIREBASE] Loi push history: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

// ==================== DOC NGUONG CANH BAO TU FIREBASE ====================
void readThresholds() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = buildFirebaseUrl(FB_PATH_THRESHOLDS);

  http.begin(sslClient, url);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    Serial.println("[FIREBASE] Doc thresholds: " + payload);

    // Parse JSON don gian (khong dung thu vien de tiet kiem RAM)
    // Cac field: temp_lv1_min, temp_lv1_max, temp_lv2_min, temp_lv2_max,
    //            temp_lv3_min, temp_lv3_max, door_lv1_sec, door_lv2_sec, door_lv3_sec
    if (payload != "null" && payload.length() > 5) {
      float val;
      if (parseJsonFloat(payload, "temp_lv1_min", val)) TEMP_LV1_MIN = val;
      if (parseJsonFloat(payload, "temp_lv1_max", val)) TEMP_LV1_MAX = val;
      if (parseJsonFloat(payload, "temp_lv2_min", val)) TEMP_LV2_MIN = val;
      if (parseJsonFloat(payload, "temp_lv2_max", val)) TEMP_LV2_MAX = val;
      if (parseJsonFloat(payload, "temp_lv3_min", val)) TEMP_LV3_MIN = val;
      if (parseJsonFloat(payload, "temp_lv3_max", val)) TEMP_LV3_MAX = val;

      float doorVal;
      if (parseJsonFloat(payload, "door_lv1_sec", doorVal)) DOOR_LV1_SEC = (unsigned long)doorVal;
      if (parseJsonFloat(payload, "door_lv2_sec", doorVal)) DOOR_LV2_SEC = (unsigned long)doorVal;
      if (parseJsonFloat(payload, "door_lv3_sec", doorVal)) DOOR_LV3_SEC = (unsigned long)doorVal;

      Serial.printf("[NGUONG] Nhiet do: Lv1[%.1f,%.1f] Lv2[%.1f,%.1f] Lv3[%.1f,%.1f]\n",
                    TEMP_LV1_MIN, TEMP_LV1_MAX, TEMP_LV2_MIN, TEMP_LV2_MAX, TEMP_LV3_MIN, TEMP_LV3_MAX);
      Serial.printf("[NGUONG] Cua: Lv1=%lus Lv2=%lus Lv3=%lus\n",
                    DOOR_LV1_SEC, DOOR_LV2_SEC, DOOR_LV3_SEC);
    }
  } else if (code > 0) {
    Serial.printf("[FIREBASE] thresholds HTTP %d\n", code);
  } else {
    Serial.printf("[FIREBASE] Loi doc thresholds: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

// ==================== PARSE JSON DON GIAN ====================
bool parseJsonFloat(const String& json, const String& key, float& result) {
  String searchKey = "\"" + key + "\":";
  int idx = json.indexOf(searchKey);
  if (idx < 0) return false;

  int start = idx + searchKey.length();
  // Bo qua khoang trang
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

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n========================================================");
  Serial.println("  HE THONG IOT GIAM SAT KHO LANH (THUNG XOP) - ESP32");
  Serial.println("  Firebase: sensor_data / history / thresholds");
  Serial.println("  3 muc canh bao: Xanh(Lv1) Vang(Lv2) Do+Coi(Lv3)");
  Serial.println("========================================================\n");

  // GPIO
  pinMode(PIN_DOOR_MC38, INPUT_PULLUP);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_ONBOARD, OUTPUT);
  if (PIN_BUZZER >= 0) pinMode(PIN_BUZZER, OUTPUT);

  // Tat het LED ban dau
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_YELLOW, LOW);
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_ONBOARD, LOW);
  if (PIN_BUZZER >= 0) digitalWrite(PIN_BUZZER, LOW);

  // Test LED neu bat
  if (TEST_ALL_LEDS_ON) {
    Serial.println("[TEST] BAT TAT CA LED de kiem tra mach");
    digitalWrite(PIN_LED_GREEN, HIGH);
    digitalWrite(PIN_LED_YELLOW, HIGH);
    digitalWrite(PIN_LED_RED, HIGH);
    digitalWrite(PIN_LED_ONBOARD, HIGH);
  }

  // DHT22
  dht.begin();
  Serial.println("[DHT22] Da khoi dong cam bien.");

  // SSL (bo qua xac thuc cert)
  sslClient.setInsecure();

  // WiFi
  Serial.printf("[WIFI] Dang ket noi: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_PASSWORD) > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  } else {
    WiFi.begin(WIFI_SSID);
  }

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Ket noi thanh cong!");
    Serial.print("[WIFI] IP: ");
    Serial.println(WiFi.localIP());
    digitalWrite(PIN_LED_ONBOARD, HIGH);
  } else {
    Serial.println("\n[WIFI] Khong the ket noi! Kiem tra SSID/Password.");
  }

  // Doc cam bien lan dau
  delay(2000); // DHT22 can thoi gian khoi dong
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h)) {
    currentTemp = t;
    currentHum = h;
    dhtReady = true;
  }
  isDoorOpen = readDoorState();
  lastDoorOpenState = isDoorOpen;

  // Doc nguong tu Firebase
  readThresholds();

  // Gui du lieu dau tien
  currentAlertLevel = calculateAlertLevel();
  alarmString = buildAlarmString(currentAlertLevel);
  updateOutputs(currentAlertLevel);
  sendSensorData();
  pushHistory();
  lastFirebaseSendTime = millis();
}

// ==================== LOOP ====================
void loop() {
  unsigned long now = millis();

  // ---- BUOC 1: Doc DHT22 moi 2 giay ----
  if (now - lastDhtReadTime >= DHT_READ_INTERVAL) {
    lastDhtReadTime = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      currentTemp = t;
      currentHum  = h;
      dhtReady = true;
    } else {
      Serial.println("[DHT22] Loi doc cam bien!");
    }
  }

  // ---- BUOC 2: Quan ly cua MC-38 ----
  isDoorOpen = readDoorState();

  if (isDoorOpen) {
    if (!lastDoorOpenState) {
      // Vua phat hien cua mo
      doorOpenStartTime = now;
      doorOpenDurationSec = 0;
      Serial.println("[CUA] -> CUA BAT DAU MO!");
      // Gui ngay lap tuc de Web cap nhat realtime
      currentAlertLevel = calculateAlertLevel();
      alarmString = buildAlarmString(currentAlertLevel);
      sendSensorData();
      lastFirebaseSendTime = now;
    } else {
      doorOpenDurationSec = (now - doorOpenStartTime) / 1000;
    }
  } else {
    if (lastDoorOpenState) {
      // Vua phat hien cua dong
      Serial.printf("[CUA] -> DA DONG! (mo %lu giay)\n", doorOpenDurationSec);
      doorOpenDurationSec = 0;
      // Gui ngay lap tuc
      currentAlertLevel = calculateAlertLevel();
      alarmString = buildAlarmString(currentAlertLevel);
      sendSensorData();
      lastFirebaseSendTime = now;
    }
    doorOpenDurationSec = 0;
  }
  lastDoorOpenState = isDoorOpen;

  // ---- BUOC 3: Tinh muc canh bao va dieu khien LED + Coi ----
  currentAlertLevel = calculateAlertLevel();
  alarmString = buildAlarmString(currentAlertLevel);
  updateOutputs(currentAlertLevel);

  // ---- BUOC 4: Gui du lieu dinh ky moi 5 giay ----
  if (now - lastFirebaseSendTime >= FB_SEND_INTERVAL) {
    lastFirebaseSendTime = now;

    Serial.println("--------------------------------------------------");
    Serial.printf("[DATA] Nhiet do: %.1f C | Do am: %.1f%%\n", currentTemp, currentHum);
    Serial.printf("[DATA] Cua: %s | TG mo: %lus\n", isDoorOpen ? "MO" : "DONG", doorOpenDurationSec);
    Serial.printf("[DATA] LED: Xanh=%d Vang=%d Do=%d | Coi=%d\n",
                  ledGreenState, ledYellowState, ledRedState, buzzerActive);
    Serial.printf("[DATA] Muc canh bao: %d | Alarm: %s\n", currentAlertLevel, alarmString.c_str());

    sendSensorData();
    pushHistory();
  }

  // ---- BUOC 5: Doc nguong canh bao tu Firebase moi 30 giay ----
  if (now - lastThresholdReadTime >= FB_THRESHOLD_INTERVAL) {
    lastThresholdReadTime = now;
    readThresholds();
  }

  // ---- Reconnect WiFi neu mat ket noi ----
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;
    if (now - lastReconnect >= 10000) {
      lastReconnect = now;
      Serial.println("[WIFI] Mat ket noi, dang thu lai...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, strlen(WIFI_PASSWORD) > 0 ? WIFI_PASSWORD : NULL);
      digitalWrite(PIN_LED_ONBOARD, LOW);
    }
  } else {
    digitalWrite(PIN_LED_ONBOARD, HIGH);
  }

  delay(50);
}

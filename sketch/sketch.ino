#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>

Preferences preferences;

// =====================================================
// 24/7 FAST WIFI CONFIG
// =====================================================
const char* WIFI_SSID     = "MDF";
const char* WIFI_PASSWORD = "@irp0r7df2021";

// =====================================================
// DEVICE & CONFIG VARIABLES
// =====================================================
char deviceID[32] = "PN012";         // Default Device ID
const char* apPassword = "z1234567"; // AP Host Password
const char* apiKey = "mdf@789789";   // مفتاح الأمان لقواعد Firebase

// =====================================================
// FIREBASE
// =====================================================
const char* firebaseBaseURL =
  "https://esp32-alerts-d64e8-default-rtdb.firebaseio.com";

const char* firebaseURL =
  "https://esp32-alerts-d64e8-default-rtdb.firebaseio.com/alerts.json";

// =====================================================
// GPIO & PWM CONFIG
// =====================================================
#define BUTTON_HELP 2   // الزر الأول: طلب مساعدة زبون
#define BUTTON_PC   4   // الزر الثاني: طلب تفويض VOID

#define GREEN_LED   18
#define RED_LED     19
#define YELLOW_LED  21

#define PWM_FREQ        5000
#define PWM_RESOLUTION 8

// =====================================================
// SYSTEM STATES & TIMERS
// =====================================================
enum SystemState {
  STATE_IDLE,
  STATE_WAITING,   // الأخضر يومض (في انتظار استجابة)
  STATE_RESPONDED  // الأصفر يومض بالتلاشي (تمت الاستجابة)
};

SystemState currentState = STATE_IDLE;

String currentAlertKey  = "";        // مفتاح البلاغ المفتوح في Firebase
String activeAlertType  = "URGENT";  // نوع البلاغ الحالي (URGENT أو VOID)
bool secondMessageSent  = false;     // ضمان تحديث التذكير مرة واحدة فقط

unsigned long alertStartTime = 0;
const unsigned long SECOND_MSG_DELAY   = 45000;  // التذكير بعد 45 ثانية في نفس المكان
const unsigned long TIMEOUT_AUTO_RESET = 300000; // مهلة 5 دقائق (300,000ms) لإلغاء التنبيه تلقائياً

unsigned long lastFirebasePoll = 0;
const unsigned long POLL_INTERVAL = 500;         // فحص Firebase كل 500ms

unsigned long lastGreenBlink = 0;
bool greenLedStatus = false;

int yellowBrightness = 0;
int yellowFadeAmount = 5;
unsigned long lastYellowFade = 0;

WiFiClientSecure firebaseClient;
HTTPClient firebaseHTTP;
bool firebaseReady = false;

bool lastHelpState = HIGH;
bool lastPcState   = HIGH;
unsigned long lastHelpPress = 0;
unsigned long lastPcPress   = 0;
const unsigned long BUTTON_DEBOUNCE = 200;

unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 500;
unsigned long lastReconnectAttempt = 0;
const unsigned long WIFI_RECONNECT_INTERVAL = 2000;
bool reconnectingWiFi = false;

// =====================================================
// LED CONTROL & ANIMATIONS
// =====================================================
void allLedsOff() {
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
  ledcWrite(YELLOW_LED, 0);
  yellowBrightness = 0;
  yellowFadeAmount = 5;
}

void resetSystemToIdle() {
  currentState = STATE_IDLE;
  currentAlertKey = "";
  secondMessageSent = false;
  allLedsOff();
  Serial.println("System reset to IDLE.");
}

void flashAndFadeAllLeds() {
  digitalWrite(RED_LED, HIGH);
  ledcWrite(YELLOW_LED, 255);
  digitalWrite(GREEN_LED, HIGH);
  delay(120);

  for (int i = 255; i >= 0; i -= 15) {
    digitalWrite(RED_LED, HIGH);
    ledcWrite(YELLOW_LED, i);
    digitalWrite(GREEN_LED, HIGH);
    delayMicroseconds(i * 4);

    digitalWrite(RED_LED, LOW);
    ledcWrite(YELLOW_LED, 0);
    digitalWrite(GREEN_LED, LOW);
    delayMicroseconds((255 - i) * 4);
  }
  allLedsOff();
}

void handleStateLEDs() {
  switch (currentState) {
    case STATE_IDLE:
      allLedsOff();
      break;

    case STATE_WAITING:
      digitalWrite(RED_LED, LOW);
      ledcWrite(YELLOW_LED, 0);
      if (millis() - lastGreenBlink >= 400) {
        lastGreenBlink = millis();
        greenLedStatus = !greenLedStatus;
        digitalWrite(GREEN_LED, greenLedStatus);
      }
      break;

    case STATE_RESPONDED:
      digitalWrite(GREEN_LED, LOW);
      digitalWrite(RED_LED, LOW);
      if (millis() - lastYellowFade >= 25) {
        lastYellowFade = millis();
        yellowBrightness += yellowFadeAmount;
        if (yellowBrightness <= 0 || yellowBrightness >= 255) {
          yellowFadeAmount = -yellowFadeAmount;
        }
        yellowBrightness = constrain(yellowBrightness, 0, 255);
        ledcWrite(YELLOW_LED, yellowBrightness);
      }
      break;
  }
}

// =====================================================
// CONFIG PORTAL & FAST WIFI CONNECT
// =====================================================
void startConfigPortalWithTimeout() {
  WiFiManager wm;
  float tempC = temperatureRead();
  if (isnan(tempC) || tempC == 0) tempC = 41.8;

  String customHead =
    "<style>"
    "body{background-color:#0d1117;color:#c9d1d9;font-family:sans-serif;margin:0;padding:15px;direction:rtl;}"
    "div.outer,.container,#wifi,#s,.msg{display:none!important;}"
    "#custom-dashboard{display:block!important;max-width:380px;margin:10px auto;background:#161b22;border:1px solid #30363d;border-radius:12px;padding:20px;text-align:right;}"
    ".title{text-align:center;font-size:20px;font-weight:700;color:#58a6ff;margin-bottom:4px;}"
    ".subtitle{text-align:center;font-size:12px;color:#8b949e;margin-bottom:20px;}"
    ".temp-box{background:#21262d;border:1px solid #30363d;border-radius:8px;padding:12px 15px;margin-bottom:18px;display:flex;justify-content:space-between;}"
    "input{width:100%;box-sizing:border-box;background:#0d1117;border:1px solid #30363d;color:#f0f6fc;border-radius:6px;padding:12px;font-size:15px;direction:ltr;}"
    ".btn-save{width:100%;background:#238636;border:none;color:#fff;border-radius:6px;padding:12px;font-size:15px;font-weight:bold;cursor:pointer;margin-top:10px;}"
    "</style>"
    "<div id='custom-dashboard'>"
      "<div class='title'>إعدادات الجهاز</div>"
      "<div class='subtitle'>MDF AlertNet System</div>"
      "<div class='temp-box'><span>حرارة المعالج:</span><span>" + String(tempC, 1) + " &deg;C</span></div>"
      "<form action='/wifi' method='get'>"
        "<label>رقم الجهاز (Device ID):</label>"
        "<input type='text' name='device_id' value='" + String(deviceID) + "' required>"
        "<button type='submit' class='btn-save'>حفظ وخروج</button>"
      "</form>"
    "</div>";

  wm.setCustomHeadElement(customHead.c_str());
  const char* menu[] = {"wifi"};
  wm.setMenu(menu, 1);
  wm.setConfigPortalBlocking(false);
  wm.startConfigPortal(deviceID, apPassword);

  unsigned long startTime = millis();
  unsigned long lastStepTime = 0;
  int ledStep = 0;
  bool clientConnected = false;

  while (true) {
    wm.process();
    if (wm.server->hasArg("device_id")) {
      String newDeviceID = wm.server->arg("device_id");
      newDeviceID.trim();
      if (newDeviceID.length() > 0) {
        preferences.begin("mdf-config", false);
        preferences.putString("deviceID", newDeviceID);
        preferences.end();
        wm.server->send(200, "text/html", "<h2 style='color:#238636;text-align:center;'>تم الحفظ! جاري إعادة التشغيل...</h2>");
        delay(800);
        ESP.restart();
      }
    }

    if (!clientConnected && WiFi.softAPgetStationNum() > 0) {
      clientConnected = true;
      allLedsOff();
      digitalWrite(RED_LED, HIGH);
    }

    if (!clientConnected) {
      if (millis() - startTime >= 10000) break;
      if (millis() - lastStepTime >= 100) {
        lastStepTime = millis();
        allLedsOff();
        if (ledStep == 0) digitalWrite(RED_LED, HIGH);
        else if (ledStep == 1) ledcWrite(YELLOW_LED, 255);
        else if (ledStep == 2) digitalWrite(GREEN_LED, HIGH);
        ledStep = (ledStep + 1) % 3;
      }
    }
    delay(5);
  }

  wm.stopConfigPortal();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  flashAndFadeAllLeds();
}

// =====================================================
// FIREBASE & NETWORK LOGIC
// =====================================================
bool initFirebase() {
  firebaseClient.setInsecure();
  firebaseClient.setTimeout(1200);
  if (!firebaseHTTP.begin(firebaseClient, firebaseURL)) {
    firebaseReady = false;
    return false;
  }
  firebaseHTTP.setReuse(true);
  firebaseHTTP.setConnectTimeout(1200);
  firebaseHTTP.setTimeout(2500);
  firebaseHTTP.addHeader("Content-Type", "application/json");
  firebaseReady = true;
  return true;
}

void closeFirebase() {
  if (firebaseReady) {
    firebaseHTTP.end();
    firebaseReady = false;
  }
}

void monitorWiFi() {
  if (millis() - lastWiFiCheck < WIFI_CHECK_INTERVAL) return;
  lastWiFiCheck = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (reconnectingWiFi) {
      reconnectingWiFi = false;
      digitalWrite(RED_LED, LOW);
      initFirebase();
    }
    return;
  }

  if (!reconnectingWiFi) {
    reconnectingWiFi = true;
    closeFirebase();
    digitalWrite(GREEN_LED, LOW);
    ledcWrite(YELLOW_LED, 0);
    digitalWrite(RED_LED, HIGH);
  }

  if (millis() - lastReconnectAttempt >= WIFI_RECONNECT_INTERVAL) {
    lastReconnectAttempt = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

String jsonEscape(const char* text) {
  String output;
  output.reserve(strlen(text) + 20);
  while (*text) {
    char c = *text++;
    switch (c) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      default: output += c; break;
    }
  }
  return output;
}

bool sendToFirebase(const char* type, const char* message) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!firebaseReady && !initFirebase()) return false;

  String json = "{\"apiKey\":\"" + String(apiKey) +
                "\",\"deviceID\":\"" + String(deviceID) +
                "\",\"location\":\"" + String(deviceID) +
                "\",\"type\":\"" + String(type) +
                "\",\"message\":\"" + jsonEscape(message) +
                "\",\"isReminder\":false,\"status\":\"pending\",\"responder\":\"\",\"timestamp\":{\".sv\":\"timestamp\"}}";

  int httpCode = firebaseHTTP.POST(json);
  if (httpCode <= 0) {
    closeFirebase();
    if (initFirebase()) httpCode = firebaseHTTP.POST(json);
  }

  if (httpCode == 200 || httpCode == 201) {
    String response = firebaseHTTP.getString();
    int keyStart = response.indexOf("\"name\":\"") + 8;
    int keyEnd = response.indexOf("\"", keyStart);
    if (keyStart > 7 && keyEnd > keyStart) {
      currentAlertKey = response.substring(keyStart, keyEnd);
    }
    return true;
  }
  return false;
}

// تحديث نفس البلاغ لتغيير لونه وتفعيله كتذكير عاجل في نفس الخانة
bool updateReminderInFirebase(const String& key, const char* message) {
  if (key == "" || WiFi.status() != WL_CONNECTED) return false;

  String patchURL = String(firebaseBaseURL) + "/alerts/" + key + ".json";
  WiFiClientSecure patchClient;
  patchClient.setInsecure();
  patchClient.setTimeout(1500);
  HTTPClient patchHTTP;

  bool ok = false;
  if (patchHTTP.begin(patchClient, patchURL)) {
    patchHTTP.addHeader("Content-Type", "application/json");
    String body = "{\"isReminder\":true,\"message\":\"" + jsonEscape(message) + "\",\"apiKey\":\"" + String(apiKey) + "\"}";
    int code = patchHTTP.PATCH(body);
    ok = (code == 200 || code == 204);
    patchHTTP.end();
  }
  return ok;
}

void resolveAlertInFirebase(const String& key) {
  if (key == "" || WiFi.status() != WL_CONNECTED) return;

  String patchURL = String(firebaseBaseURL) + "/alerts/" + key + ".json";
  WiFiClientSecure patchClient;
  patchClient.setInsecure();
  patchClient.setTimeout(1500);
  HTTPClient patchHTTP;

  if (patchHTTP.begin(patchClient, patchURL)) {
    patchHTTP.addHeader("Content-Type", "application/json");
    String body = "{\"status\":\"resolved\",\"apiKey\":\"" + String(apiKey) + "\"}";
    patchHTTP.PATCH(body);
    patchHTTP.end();
  }
}

void checkAlertStatus() {
  if (currentAlertKey == "" || WiFi.status() != WL_CONNECTED) return;

  String checkURL = String(firebaseBaseURL) + "/alerts/" + currentAlertKey + "/status.json";
  WiFiClientSecure checkClient;
  checkClient.setInsecure();
  checkClient.setTimeout(1500);
  HTTPClient checkHTTP;

  if (checkHTTP.begin(checkClient, checkURL)) {
    int code = checkHTTP.GET();
    if (code == 200) {
      String status = checkHTTP.getString();
      status.replace("\"", "");
      status.trim();
      status.toLowerCase();

      if (status == "accepted" || status == "in_progress" || status == "seen") {
        currentState = STATE_RESPONDED;
      } 
      else if (status == "resolved" || status == "closed" || status == "completed" || status == "done" || status == "null") {
        resetSystemToIdle();
      }
    }
    checkHTTP.end();
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_HELP, INPUT_PULLUP);
  pinMode(BUTTON_PC, INPUT_PULLUP);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);

  ledcAttach(YELLOW_LED, PWM_FREQ, PWM_RESOLUTION);
  allLedsOff();

  preferences.begin("mdf-config", false);
  String savedID = preferences.getString("deviceID", "PN012");
  savedID.toCharArray(deviceID, 32);
  preferences.end();

  startConfigPortalWithTimeout();

  if (WiFi.status() == WL_CONNECTED) {
    initFirebase();
  }
}

void loop() {
  monitorWiFi();
  handleStateLEDs();

  bool helpState = digitalRead(BUTTON_HELP);
  bool pcState   = digitalRead(BUTTON_PC);

  // 1. الزر الأول (Pin 2): طلب مساعدة زبون
  if (lastHelpState == HIGH && helpState == LOW) {
    unsigned long now = millis();
    if (now - lastHelpPress > BUTTON_DEBOUNCE) {
      lastHelpPress = now;
      digitalWrite(GREEN_LED, HIGH);
      if (sendToFirebase("URGENT", "Customer at counter requires support.")) {
        activeAlertType = "URGENT";
        currentState = STATE_WAITING;
        alertStartTime = millis();
        secondMessageSent = false;
      } else {
        digitalWrite(GREEN_LED, LOW);
      }
    }
  }

  // 2. الزر الثاني (Pin 4): طلب تفويض VOID
  if (lastPcState == HIGH && pcState == LOW) {
    unsigned long now = millis();
    if (now - lastPcPress > BUTTON_DEBOUNCE) {
      lastPcPress = now;
      digitalWrite(GREEN_LED, HIGH);
      if (sendToFirebase("VOID", "Supervisor authorization needed for VOID.")) {
        activeAlertType = "VOID";
        currentState = STATE_WAITING;
        alertStartTime = millis();
        secondMessageSent = false;
      } else {
        digitalWrite(GREEN_LED, LOW);
      }
    }
  }

  lastHelpState = helpState;
  lastPcState   = pcState;

  // 3. بعد 45 ثانية: تحديث نفس البلاغ لتغيير لونه في نفس المكان بدون إضافة خانة جديدة
  if (currentState == STATE_WAITING && !secondMessageSent) {
    if (millis() - alertStartTime >= SECOND_MSG_DELAY) {
      if (activeAlertType == "VOID") {
        updateReminderInFirebase(currentAlertKey, "Urgent! Second alert sent for pending VOID authorization.");
      } else {
        updateReminderInFirebase(currentAlertKey, "Urgent! Second alert sent for pending customer.");
      }
      secondMessageSent = true;
    }
  }

  // 4. مهلة 5 دقائق (300,000 مللي ثانية) لإلغاء التنبيه تلقائياً في حال عدم الاستجابة
  if (currentState != STATE_IDLE) {
    if (millis() - alertStartTime >= TIMEOUT_AUTO_RESET) {
      resolveAlertInFirebase(currentAlertKey);
      resetSystemToIdle();
    }
  }

  // 5. فحص حالة البلاغ كل 500ms
  if (currentState != STATE_IDLE && (millis() - lastFirebasePoll >= POLL_INTERVAL)) {
    lastFirebasePoll = millis();
    checkAlertStatus();
  }

  delay(2);
}

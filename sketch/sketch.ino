#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <Ticker.h>

Preferences preferences;
Ticker ledTicker;

// =====================================================
// DEFAULT WIFI NETWORK
// =====================================================
const char* WIFI_SSID     = "MDF";
const char* WIFI_PASSWORD = "@irp0r7df2021";

// =====================================================
// DEVICE & CONFIG VARIABLES
// =====================================================

char deviceID[32] = "PN012";            // Default Device ID
const char* apPassword = "z1234567";    // AP Host Password
const char* apiKey     = "mdf@789789";  // مفتاح الأمان المتوافق مع التطبيق وفايربيس

// =====================================================
// FIREBASE CONFIG
// =====================================================

const char* firebaseBaseURL =
  "https://mdf-2abd6-default-rtdb.europe-west1.firebasedatabase.app";
const char* firebaseURL =
  "https://mdf-2abd6-default-rtdb.europe-west1.firebasedatabase.app/alerts.json";

// =====================================================
// GPIO & PWM CONFIG
// =====================================================

#define BUTTON_HELP 2
#define BUTTON_PC   4

#define GREEN_LED   18
#define RED_LED     19
#define YELLOW_LED  21

#define PWM_FREQ       5000
#define PWM_RESOLUTION 8

// =====================================================
// SYSTEM STATES & TIMERS
// =====================================================

enum SystemState {
  STATE_IDLE,
  STATE_WAITING,   // الأخضر يومض (في انتظار استجابة من التطبيق)
  STATE_RESPONDED  // الأصفر يومض بالتلاشي Breathing (تمت الاستجابة من التطبيق)
};

volatile SystemState currentState = STATE_IDLE;
volatile bool portalModeActive = false;

String currentAlertKey  = "";    // مفتاح البلاغ الأساسي في Firebase
String reminderAlertKey = "";    // مفتاح البلاغ التذكيري الثاني (بعد 45 ثانية)
bool secondMessageSent  = false; // ضمان إرسال الرسالة الثانية مرة واحدة فقط

unsigned long alertStartTime = 0;
const unsigned long SECOND_MSG_DELAY   = 45000; // الإرسال الثاني بعد 45 ثانية
const unsigned long TIMEOUT_AUTO_RESET = 60000; // مهلة دقيقة واحدة لإلغاء التنبيه تلقائياً

unsigned long lastFirebasePoll = 0;
const unsigned long POLL_INTERVAL = 1500;       // استعلام Firebase كل 1.5 ثانية

// توقيتات أنماط الإضاءة
unsigned long lastGreenBlink = 0;
bool greenLedStatus = false;

int yellowBrightness = 0;
int yellowFadeAmount = 5;
unsigned long lastYellowFade = 0;

// =====================================================
// PERSISTENT HTTPS CONNECTION
// =====================================================

WiFiClientSecure firebaseClient;
HTTPClient firebaseHTTP;
bool firebaseReady = false;

// =====================================================
// BUTTON STATES & NETWORK TIMERS
// =====================================================

bool lastHelpState = HIGH;
bool lastPcState   = HIGH;
unsigned long lastHelpPress = 0;
unsigned long lastPcPress   = 0;
const unsigned long BUTTON_DEBOUNCE = 200;

unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 1000;
bool reconnectingWiFi = false;

// =====================================================
// LED CONTROL & ANIMATIONS
// =====================================================

void allLedsOff() {
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
  ledcWrite(YELLOW_LED, 0); // إطفاء الأصفر تماماً عبر PWM
  yellowBrightness = 0;     // تصفير قيمة السطوع للنمط التالي
  yellowFadeAmount = 5;
}

void handleStateLEDs() {
  if (portalModeActive || reconnectingWiFi) return;

  switch (currentState) {
    case STATE_IDLE:
      allLedsOff();
      break;

    case STATE_WAITING:
      // وميض الأخضر وإطفاء الباقي
      digitalWrite(RED_LED, LOW);
      ledcWrite(YELLOW_LED, 0);
      if (millis() - lastGreenBlink >= 400) {
        lastGreenBlink = millis();
        greenLedStatus = !greenLedStatus;
        digitalWrite(GREEN_LED, greenLedStatus);
      }
      break;

    case STATE_RESPONDED:
      // وميض الأصفر بالتلاشي البطيء (Breathing)
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

void flashAndFadeAllLeds() {
  portalModeActive = true;
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
  portalModeActive = false;
}

// =====================================================
// FIREBASE & NETWORK LOGIC
// =====================================================

bool initFirebase() {
  firebaseClient.setInsecure();
  firebaseClient.setTimeout(2000);

  firebaseHTTP.end();
  if (!firebaseHTTP.begin(firebaseClient, firebaseURL)) {
    firebaseReady = false;
    return false;
  }

  firebaseHTTP.setReuse(true);
  firebaseHTTP.setConnectTimeout(1500);
  firebaseHTTP.setTimeout(3000);
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

String jsonEscape(const char* text) {
  String output;
  output.reserve(strlen(text) + 20);
  while (*text) {
    char c = *text++;
    switch (c) {
      case '"':  output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      default:   output += c; break;
    }
  }
  return output;
}

String extractFirebaseKey(const String& response) {
  int keyIdx = response.indexOf("\"name\"");
  if (keyIdx < 0) return "";
  int colonIdx = response.indexOf(':', keyIdx);
  if (colonIdx < 0) return "";
  int firstQuote = response.indexOf('"', colonIdx + 1);
  if (firstQuote < 0) return "";
  int secondQuote = response.indexOf('"', firstQuote + 1);
  if (secondQuote < 0) return "";
  return response.substring(firstQuote + 1, secondQuote);
}

bool sendToFirebase(const char* type, const char* message, bool isReminder = false) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!firebaseReady && !initFirebase()) return false;

  String title = isReminder
    ? ("⚠️ تذكير عاجل ثانٍ - تأخر الاستجابة (" + String(deviceID) + ")")
    : ("🛎️ طلب القدوم لحل مشكلة زبون (" + String(deviceID) + ")");

  String json = "{\"apiKey\":\"" + String(apiKey) +
                "\",\"deviceID\":\"" + String(deviceID) +
                "\",\"deviceId\":\"" + String(deviceID) +
                "\",\"location\":\"" + String(deviceID) +
                "\",\"type\":\"" + String(type) +
                "\",\"title\":\"" + jsonEscape(title.c_str()) +
                "\",\"message\":\"" + jsonEscape(message) +
                "\",\"status\":\"pending\",\"responder\":\"\",\"timestamp\":{\".sv\":\"timestamp\"}}";

  firebaseHTTP.setURL(firebaseURL);
  firebaseHTTP.addHeader("Content-Type", "application/json");
  int httpCode = firebaseHTTP.POST(json);

  if (httpCode <= 0 && initFirebase()) {
    httpCode = firebaseHTTP.POST(json);
  }

  if (httpCode == 200 || httpCode == 201) {
    String response = firebaseHTTP.getString();
    String newKey = extractFirebaseKey(response);
    if (newKey.length() > 0) {
      if (isReminder) {
        reminderAlertKey = newKey; // حفظ مفتاح التذكير دون مسح المفتاح الأساسي
      } else {
        currentAlertKey = newKey;
        reminderAlertKey = "";
      }
    }
    return true;
  }

  if (httpCode > 0) firebaseHTTP.getString();
  return false;
}

void resolveAlertInFirebase(const String& key) {
  if (key.length() == 0 || WiFi.status() != WL_CONNECTED) return;
  if (!firebaseReady && !initFirebase()) return;

  String patchURL = String(firebaseBaseURL) + "/alerts/" + key + ".json";
  String body = "{\"status\":\"resolved\",\"apiKey\":\"" + String(apiKey) + "\"}";

  firebaseHTTP.setURL(patchURL);
  firebaseHTTP.addHeader("Content-Type", "application/json");
  int code = firebaseHTTP.PATCH(body);
  if (code > 0) firebaseHTTP.getString();
  firebaseHTTP.setURL(firebaseURL);
}

void resetSystemToIdle(bool notifyServer = false) {
  if (notifyServer) {
    if (currentAlertKey.length() > 0) resolveAlertInFirebase(currentAlertKey);
    if (reminderAlertKey.length() > 0) resolveAlertInFirebase(reminderAlertKey);
  }
  currentState = STATE_IDLE;
  currentAlertKey = "";
  reminderAlertKey = "";
  secondMessageSent = false;
  allLedsOff();
  Serial.println("System reset to IDLE.");
}

String fetchSingleKeyStatus(const String& key) {
  if (key.length() == 0 || WiFi.status() != WL_CONNECTED) return "";
  if (!firebaseReady && !initFirebase()) return "";

  String checkURL = String(firebaseBaseURL) + "/alerts/" + key + "/status.json";
  firebaseHTTP.setURL(checkURL);
  int code = firebaseHTTP.GET();
  String status = "";
  if (code == 200) {
    status = firebaseHTTP.getString();
    status.replace("\"", "");
    status.trim();
    status.toLowerCase();
  } else if (code > 0) {
    firebaseHTTP.getString();
  } else {
    initFirebase();
  }
  firebaseHTTP.setURL(firebaseURL);
  return status;
}

void checkAlertStatus() {
  if (currentAlertKey.length() == 0 && reminderAlertKey.length() == 0) return;

  String status = fetchSingleKeyStatus(currentAlertKey);

  // فحص مفتاح الرسالة التذكيرية أيضاً إن وُجدت
  if (reminderAlertKey.length() > 0) {
    String remStatus = fetchSingleKeyStatus(reminderAlertKey);
    if (remStatus == "resolved" || remStatus == "completed" || remStatus == "closed" || remStatus == "done") {
      status = remStatus;
    } else if ((remStatus == "accepted" || remStatus == "in_progress" || remStatus == "seen") && status == "pending") {
      status = remStatus;
    }
  }

  if (status == "accepted" || status == "in_progress" || status == "seen") {
    if (currentState != STATE_RESPONDED) {
      currentState = STATE_RESPONDED;
      Serial.println(">>> Alert ACCEPTED by staff! Yellow breathing LED active <<<");
    }
  } 
  else if (status == "resolved" || status == "closed" || status == "completed" || status == "done" || status == "null") {
    Serial.println(">>> Alert RESOLVED in app! Resetting to IDLE <<<");
    resetSystemToIdle(false);
  }
}

// =====================================================
// CONFIG PORTAL SETUP (DEVICE ID + WIFI)
// =====================================================

void startConfigPortalWithTimeout() {
  portalModeActive = true;
  WiFiManager wm;

  float tempC = temperatureRead();
  if (isnan(tempC) || tempC == 0) tempC = 41.8;

  String customHead = 
    "<style>"
    "body{background-color:#0d1117;color:#c9d1d9;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;padding:15px;direction:rtl;}"
    "div.outer,.container,#wifi,#s,.msg{display:none!important;}"
    "#custom-dashboard{display:block!important;max-width:380px;margin:10px auto;background:#161b22;border:1px solid #30363d;border-radius:12px;padding:20px;box-shadow:0 10px 30px rgba(0,0,0,0.6);text-align:right;}"
    ".title{text-align:center;font-size:20px;font-weight:700;color:#58a6ff;margin-bottom:4px;}"
    ".subtitle{text-align:center;font-size:12px;color:#8b949e;margin-bottom:20px;}"
    ".temp-box{background:#21262d;border:1px solid #30363d;border-radius:8px;padding:12px 15px;margin-bottom:18px;display:flex;justify-content:space-between;align-items:center;}"
    ".temp-label{font-size:13px;color:#8b949e;font-weight:600;}"
    ".temp-val{font-size:18px;font-weight:bold;color:#f0883e;direction:ltr;}"
    ".form-group{margin-bottom:18px;}"
    "label{display:block;font-size:12px;font-weight:600;color:#c9d1d9;margin-bottom:6px;}"
    "input{width:100%;box-sizing:border-box;background:#0d1117;border:1px solid #30363d;color:#f0f6fc;border-radius:6px;padding:12px;font-size:15px;outline:none;transition:0.2s;direction:ltr;text-align:left;}"
    "input:focus{border-color:#58a6ff;box-shadow:0 0 0 3px rgba(88,166,255,0.15);}"
    ".btn-save{width:100%;background:#238636;border:1px solid rgba(240,246,252,0.1);color:#ffffff;border-radius:6px;padding:12px;font-size:15px;font-weight:bold;cursor:pointer;margin-top:10px;transition:0.2s;}"
    ".btn-save:hover{background:#2ea043;}"
    "</style>"

    "<div id='custom-dashboard'>"
      "<div class='title'>إعدادات الجهاز</div>"
      "<div class='subtitle'>MDF AlertNet System</div>"

      "<div class='temp-box'>"
        "<span class='temp-label'>حرارة المعالج الداخلي:</span>"
        "<span class='temp-val'>" + String(tempC, 1) + " &deg;C</span>"
      "</div>"

      "<form action='/wifi' method='get'>"
        "<div class='form-group'>"
          "<label>رقم الجهاز (Device ID):</label>"
          "<input type='text' name='device_id' value='" + String(deviceID) + "' placeholder='PN012' required>"
        "</div>"

        "<button type='submit' class='btn-save'>حفظ وخروج (Save & Exit)</button>"
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

    if (wm.server && wm.server->hasArg("device_id")) {
      String newDeviceID = wm.server->arg("device_id");
      newDeviceID.trim();

      if (newDeviceID.length() > 0) {
        preferences.begin("mdf-config", false);
        preferences.putString("deviceID", newDeviceID);
        preferences.end();

        wm.server->send(200, "text/html", "<h2 style='color:#238636;text-align:center;font-family:sans-serif;'>تم حفظ الرقم بنجاح! جاري إعادة التشغيل...</h2>");
        delay(1000);
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

  // التبديل إلى وضع Station والاتصال بالشبكة المحفوظة
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long wifiWait = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiWait < 6000) {
    delay(100);
  }

  flashAndFadeAllLeds();
  portalModeActive = false;
}

void monitorWiFi() {
  if (millis() - lastWiFiCheck < WIFI_CHECK_INTERVAL) return;
  lastWiFiCheck = millis();

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(RED_LED, LOW);
    return;
  }
  if (reconnectingWiFi) return;

  reconnectingWiFi = true;
  closeFirebase();
  digitalWrite(GREEN_LED, LOW);
  ledcWrite(YELLOW_LED, 0);
  digitalWrite(RED_LED, HIGH);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 4000) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(RED_LED, LOW);
    initFirebase();
  } else {
    digitalWrite(RED_LED, HIGH);
  }

  reconnectingWiFi = false;
}

// =====================================================
// SETUP & LOOP
// =====================================================

void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_HELP, INPUT_PULLUP);
  pinMode(BUTTON_PC, INPUT_PULLUP);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);

  // ربط الـ PWM لمنفذ الأصفر بتوافق مع ESP32 Core v3
  ledcAttach(YELLOW_LED, PWM_FREQ, PWM_RESOLUTION);

  allLedsOff();

  preferences.begin("mdf-config", false);
  String savedID = preferences.getString("deviceID", "PN012");
  savedID.toCharArray(deviceID, 32);
  preferences.end();

  startConfigPortalWithTimeout();

  // تشغيل أنماط الإضاءة عبر مؤقت خلفي لضمان عدم التقطيع أثناء طلبات HTTP
  ledTicker.attach_ms(25, handleStateLEDs);

  if (WiFi.status() == WL_CONNECTED) {
    initFirebase();
  }
}

void loop() {
  monitorWiFi();

  bool helpState = digitalRead(BUTTON_HELP);
  bool pcState   = digitalRead(BUTTON_PC);

  // 1. عند ضغط زر طلب المساعدة
  if (lastHelpState == HIGH && helpState == LOW) {
    unsigned long now = millis();
    if (now - lastHelpPress > BUTTON_DEBOUNCE) {
      lastHelpPress = now;

      if (sendToFirebase("URGENT", "Customer at counter requires support.", false)) {
        currentState = STATE_WAITING;
        alertStartTime = millis();
        secondMessageSent = false;
      }
    }
  }

  // 2. عند ضغط زر الحل / الإلغاء (إغلاق البلاغ في التطبيق وإطفاء الأضواء فوراً)
  if (lastPcState == HIGH && pcState == LOW) {
    unsigned long now = millis();
    if (now - lastPcPress > BUTTON_DEBOUNCE) {
      lastPcPress = now;
      resetSystemToIdle(true);
    }
  }

  lastHelpState = helpState;
  lastPcState   = pcState;

  // 3. الرسالة التذكيرية الثانية تلقائياً عند التأخر (بعد 45 ثانية)
  if (currentState == STATE_WAITING && !secondMessageSent) {
    if (millis() - alertStartTime >= SECOND_MSG_DELAY) {
      sendToFirebase("REMINDER", "Urgent! Second alert sent for pending customer.", true);
      secondMessageSent = true;
    }
  }

  // 4. مؤقت الإلغاء التلقائي (العودة للوضع الطبيعي بعد 60 ثانية من بدء التنبيه)
  if (currentState != STATE_IDLE) {
    if (millis() - alertStartTime >= TIMEOUT_AUTO_RESET) {
      resetSystemToIdle(false);
    }
  }

  // 5. الاستعلام الدوري لمعرفة تغيير حالة البلاغ من تطبيق الهاتف
  if (currentState != STATE_IDLE && (millis() - lastFirebasePoll >= POLL_INTERVAL)) {
    lastFirebasePoll = millis();
    checkAlertStatus();
  }

  delay(2);
}

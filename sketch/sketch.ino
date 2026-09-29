
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Ticker.h>

// =====================================================
// FAST MODE - MDF ALERT ESP32 (Smart Two-Way Sync)
// =====================================================

// ---------------- WiFi ----------------
const char* ssid     = "MDF";
const char* password = "@irp0r7df2021";

// ---------------- Device & Key ----------------
const char* deviceID = "POS-012";
const char* apiKey   = "mdf@789789";

// ---------------- Firebase ----------------
const char* firebaseURL =
  "https://mdf-2abd6-default-rtdb.europe-west1.firebasedatabase.app/alerts.json";

// =====================================================
// Pins
// =====================================================
#define BTN_CUSTOMER_CARE 2
#define BUTTON_HELP       BTN_CUSTOMER_CARE

#define BTN_VOID_AUTH     4
#define BUTTON_PC         BTN_VOID_AUTH

#define GREEN_LED  18
#define RED_LED    19
#define YELLOW_LED 21

// =====================================================
// Persistent HTTP connection & Non-Blocking LED Timer
// =====================================================
WiFiClientSecure firebaseClient;
HTTPClient firebaseHTTP;
Ticker ledTicker;

// =====================================================
// Button & Alert Lock States
// =====================================================
bool lastHelpState = HIGH;
bool lastPcState   = HIGH;

bool waitingForWebsiteAccept = false;
bool alertAcceptedByWebsite  = false;
String activeAlertId         = "";
unsigned long lastStatusPollMs = 0;
const unsigned long STATUS_POLL_INTERVAL_MS = 700;

// =====================================================
// Statistics
// =====================================================
unsigned long totalRequests      = 0;
unsigned long successfulRequests = 0;
unsigned long failedRequests     = 0;

// =====================================================
// LED State Machine
// =====================================================
enum SystemLedMode {
  LED_MODE_OFF = 0,
  LED_MODE_WIFI_SEARCH,
  LED_MODE_WIFI_FAILED,
  LED_MODE_WIFI_GREEN_ONCE,
  LED_MODE_ALERT_PENDING,
  LED_MODE_ALERT_ACCEPTED
};

volatile SystemLedMode currentLedMode = LED_MODE_OFF;
volatile bool blinkToggleState = false;
unsigned long greenOnceUntilMs = 0;

void allLedsOff() {
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(YELLOW_LED, LOW);
}

void onLedTicker() {
  blinkToggleState = !blinkToggleState;

  switch (currentLedMode) {
    case LED_MODE_OFF:
      allLedsOff();
      break;

    case LED_MODE_WIFI_SEARCH:
      digitalWrite(GREEN_LED, LOW);
      digitalWrite(RED_LED, LOW);
      digitalWrite(YELLOW_LED, blinkToggleState ? HIGH : LOW);
      break;

    case LED_MODE_WIFI_FAILED:
      digitalWrite(GREEN_LED, LOW);
      digitalWrite(YELLOW_LED, LOW);
      digitalWrite(RED_LED, blinkToggleState ? HIGH : LOW);
      break;

    case LED_MODE_WIFI_GREEN_ONCE:
      digitalWrite(YELLOW_LED, LOW);
      digitalWrite(RED_LED, LOW);
      if (greenOnceUntilMs != 0 && (int32_t)(millis() - greenOnceUntilMs) >= 0) {
        digitalWrite(GREEN_LED, LOW);
        greenOnceUntilMs = 0;
        currentLedMode = LED_MODE_OFF;
      } else {
        digitalWrite(GREEN_LED, HIGH);
      }
      break;

    case LED_MODE_ALERT_PENDING:
      digitalWrite(GREEN_LED, LOW);
      digitalWrite(RED_LED, LOW);
      digitalWrite(YELLOW_LED, blinkToggleState ? HIGH : LOW);
      break;

    case LED_MODE_ALERT_ACCEPTED:
      digitalWrite(YELLOW_LED, LOW);
      digitalWrite(RED_LED, LOW);
      digitalWrite(GREEN_LED, HIGH);
      break;
  }
}

void setLedMode(SystemLedMode mode) {
  currentLedMode = mode;
  blinkToggleState = true;

  if (mode == LED_MODE_OFF) {
    allLedsOff();
  } else if (mode == LED_MODE_WIFI_SEARCH || mode == LED_MODE_ALERT_PENDING) {
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(RED_LED, LOW);
    digitalWrite(YELLOW_LED, HIGH);
  } else if (mode == LED_MODE_WIFI_FAILED) {
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(YELLOW_LED, LOW);
    digitalWrite(RED_LED, HIGH);
  } else if (mode == LED_MODE_WIFI_GREEN_ONCE) {
    digitalWrite(YELLOW_LED, LOW);
    digitalWrite(RED_LED, LOW);
    digitalWrite(GREEN_LED, HIGH);
    greenOnceUntilMs = millis() + 650;
  } else if (mode == LED_MODE_ALERT_ACCEPTED) {
    digitalWrite(YELLOW_LED, LOW);
    digitalWrite(RED_LED, LOW);
    digitalWrite(GREEN_LED, HIGH);
  }
}

void updateLEDs() {
  if (currentLedMode == LED_MODE_WIFI_GREEN_ONCE &&
      greenOnceUntilMs != 0 &&
      (int32_t)(millis() - greenOnceUntilMs) >= 0) {
    greenOnceUntilMs = 0;
    setLedMode(LED_MODE_OFF);
  }
}

// =====================================================
// Firebase HTTP initialization
// =====================================================
bool initFirebaseConnection() {
  firebaseClient.setInsecure();
  firebaseClient.setTimeout(2000);

  firebaseHTTP.end();
  if (!firebaseHTTP.begin(firebaseClient, firebaseURL)) {
    Serial.println("Firebase HTTP begin FAILED!");
    return false;
  }

  firebaseHTTP.setReuse(true);
  firebaseHTTP.setConnectTimeout(1500);
  firebaseHTTP.setTimeout(2500);
  firebaseHTTP.addHeader("Content-Type", "application/json");
  return true;
}

// =====================================================
// Helpers
// =====================================================
String jsonEscape(const char* text) {
  String output;
  output.reserve(strlen(text) + 10);
  while (*text) {
    char c = *text++;
    if (c == '"') output += "\\\"";
    else if (c == '\\') output += "\\\\";
    else if (c == '\n') output += "\\n";
    else if (c == '\r') output += "\\r";
    else output += c;
  }
  return output;
}

String extractAlertNameFromJson(const String& resp) {
  int keyIdx = resp.indexOf("\"name\"");
  if (keyIdx < 0) return "";
  int colonIdx = resp.indexOf(':', keyIdx);
  if (colonIdx < 0) return "";
  int firstQuote = resp.indexOf('"', colonIdx + 1);
  if (firstQuote < 0) return "";
  int secondQuote = resp.indexOf('"', firstQuote + 1);
  if (secondQuote < 0) return "";
  return resp.substring(firstQuote + 1, secondQuote);
}

String buildStatusUrl(const String& alertId) {
  String base = String(firebaseURL);
  int idx = base.indexOf("/alerts.json");
  if (idx >= 0) {
    return base.substring(0, idx) + "/alerts/" + alertId + "/status.json";
  }
  return base;
}

// =====================================================
// WIFI CONNECT & RECONNECT
// =====================================================
void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println();
  Serial.println("[WIFI] Searching & Connecting to WiFi (Yellow Blinking)...");
  setLedMode(LED_MODE_WIFI_SEARCH);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  unsigned long searchStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - searchStart) < 8000) {
    delay(50);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Connection failed! Red LED blinking & retrying until connected...");
    setLedMode(LED_MODE_WIFI_FAILED);

    unsigned long lastRetry = millis();
    while (WiFi.status() != WL_CONNECTED) {
      if (millis() - lastRetry >= 5000) {
        lastRetry = millis();
        Serial.println("[WIFI] Retrying WiFi connection...");
        WiFi.disconnect();
        WiFi.begin(ssid, password);
      }
      delay(50);
    }
  }

  Serial.println();
  Serial.println("[WIFI] CONNECTED SUCCESSFULLY!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.print("RSSI: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");

  initFirebaseConnection();

  setLedMode(LED_MODE_WIFI_GREEN_ONCE);
  delay(650);
  setLedMode(LED_MODE_OFF);

  Serial.println("[READY] All LEDs OFF. Standby Mode ready for instant button press.");
}

// =====================================================
// SEND ALERT
// =====================================================
bool sendToFirebase(const char* type, const char* title, const char* message) {
  unsigned long startTime = millis();
  totalRequests++;

  if (WiFi.status() != WL_CONNECTED) {
    ensureWiFiConnected();
  }

  waitingForWebsiteAccept = true;
  alertAcceptedByWebsite  = false;
  setLedMode(LED_MODE_ALERT_PENDING);

  String json;
  json.reserve(360);
  json =
    "{\"apiKey\":\"" + String(apiKey) +
    "\",\"deviceID\":\"" + String(deviceID) +
    "\",\"device\":\"" + String(deviceID) +
    "\",\"type\":\"" + String(type) +
    "\",\"title\":\"" + jsonEscape(title) +
    "\",\"message\":\"" + jsonEscape(message) +
    "\",\"location\":\"" + String(deviceID) +
    "\",\"status\":\"pending\",\"responder\":\"\",\"timestamp\":{\".sv\":\"timestamp\"}}";

  firebaseHTTP.setURL(firebaseURL);
  firebaseHTTP.addHeader("Content-Type", "application/json");

  unsigned long postStart = millis();
  int httpCode = firebaseHTTP.POST(json);
  if (httpCode <= 0) {
    if (initFirebaseConnection()) {
      httpCode = firebaseHTTP.POST(json);
    }
  }

  unsigned long postTime = millis() - postStart;
  Serial.print("HTTP Code: "); Serial.print(httpCode);
  Serial.print(" | POST Time: "); Serial.print(postTime); Serial.println(" ms");

  if (httpCode == 200 || httpCode == 201) {
    successfulRequests++;
    String resp = firebaseHTTP.getString();
    String createdId = extractAlertNameFromJson(resp);
    if (createdId.length() > 0) {
      activeAlertId = createdId;
    }
    Serial.print("[SENT] Alert ID: "); Serial.println(activeAlertId);
    Serial.print("[TOTAL TIME] "); Serial.print(millis() - startTime); Serial.println(" ms");
    Serial.println("[WAITING] Yellow LED blinking until website presses ACCEPT...");
    lastStatusPollMs = millis();
    return true;
  }

  failedRequests++;
  if (httpCode > 0) {
    firebaseHTTP.getString();
  }
  waitingForWebsiteAccept = false;
  activeAlertId = "";
  if (WiFi.status() != WL_CONNECTED) {
    ensureWiFiConnected();
  } else {
    setLedMode(LED_MODE_OFF);
  }
  return false;
}

// =====================================================
// POLL ALERT STATUS
// =====================================================
void checkAlertStatusFromWebsite() {
  if (activeAlertId.length() == 0) return;
  if (!waitingForWebsiteAccept && !alertAcceptedByWebsite) return;
  if (millis() - lastStatusPollMs < STATUS_POLL_INTERVAL_MS) return;
  lastStatusPollMs = millis();

  if (WiFi.status() != WL_CONNECTED) return;

  String statusUrl = buildStatusUrl(activeAlertId);
  firebaseHTTP.setURL(statusUrl);
  int httpCode = firebaseHTTP.GET();

  if (httpCode == 200) {
    String statusPayload = firebaseHTTP.getString();
    statusPayload.trim();
    statusPayload.toLowerCase();

    if (waitingForWebsiteAccept &&
        (statusPayload.indexOf("in_progress") >= 0 ||
         statusPayload.indexOf("seen") >= 0 ||
         statusPayload.indexOf("accepted") >= 0)) {
      waitingForWebsiteAccept = false;
      alertAcceptedByWebsite  = true;
      setLedMode(LED_MODE_ALERT_ACCEPTED);
      Serial.println(">>> [ACCEPTED] تم قبول الطلب من الموقع! إطفاء الأصفر وإضاءة الأخضر + السماح بالأزرار <<<");
    }
    else if (statusPayload.indexOf("completed") >= 0 ||
             statusPayload.indexOf("resolved") >= 0 ||
             statusPayload.indexOf("done") >= 0 ||
             statusPayload == "null") {
      waitingForWebsiteAccept = false;
      alertAcceptedByWebsite  = false;
      activeAlertId           = "";
      setLedMode(LED_MODE_OFF);
      firebaseHTTP.setURL(firebaseURL);
      Serial.println(">>> [RESOLVED] تم حل المشكلة! إطفاء جميع الأضواء والعودة لوضع الاستعداد <<<");
    }
  } else if (httpCode > 0) {
    firebaseHTTP.getString();
  } else {
    initFirebaseConnection();
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(150);

  Serial.println();
  Serial.println("======================================");
  Serial.println(" MDF-AlertNet ESP32 - SMART SYNC");
  Serial.println("======================================");

  pinMode(BUTTON_HELP, INPUT_PULLUP);
  pinMode(BUTTON_PC, INPUT_PULLUP);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  allLedsOff();

  ledTicker.attach_ms(180, onLedTicker);

  ensureWiFiConnected();
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  updateLEDs();

  if (WiFi.status() != WL_CONNECTED) {
    ensureWiFiConnected();
  }

  checkAlertStatusFromWebsite();

  bool helpState = digitalRead(BUTTON_HELP);
  bool pcState   = digitalRead(BUTTON_PC);

  if (!waitingForWebsiteAccept) {
    if (lastHelpState == HIGH && helpState == LOW) {
      Serial.println();
      Serial.println(">>> [BUTTON_HELP] CUSTOMER ISSUE RESOLUTION PRESSED <<<");
      sendToFirebase(
        "customer_assistance",
        "🛎️ طلب القدوم لحل مشكلة زبون (POS-012)",
        "يرجى التوجه فوراً إلى كاونتر (POS-012) للقدوم ومعالجة مشكلة الزبون المتواجد حالياً."
      );
      delay(60);
    }
    else if (lastPcState == HIGH && pcState == LOW) {
      Serial.println();
      Serial.println(">>> [BUTTON_PC] TRANSACTION VOID AUTHORIZATION PRESSED <<<");
      sendToFirebase(
        "VOID",
        "🔐 طلب تفويض إلغاء عملية (VOID — POS-012)",
        "مطلوب حضور المشرف فوراً إلى كاشير (POS-012) لاعتماد وإتمام عملية الإلغاء (VOID Authorization)."
      );
      delay(60);
    }
  } else {
    if ((lastHelpState == HIGH && helpState == LOW) || (lastPcState == HIGH && pcState == LOW)) {
      Serial.println("[LOCKED] لا يمكن الضغط على الأزرار متتالياً إلا بعد قبول الطلب الأول من الموقع!");
    }
  }

  lastHelpState = helpState;
  lastPcState   = pcState;
  delay(2);
}

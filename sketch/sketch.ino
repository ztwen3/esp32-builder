#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// =====================================================
// FAST MODE - MDF ALERT ESP32
// =====================================================

// ---------------- WiFi ----------------

const char* ssid = "MDF";
const char* password = "@irp0r7df2021";

// ---------------- Device ----------------

const char* deviceID = "POS-012";

// ---------------- Firebase ----------------

const char* firebaseURL =
  "https://mdf-2abd6-default-rtdb.europe-west1.firebasedatabase.app/alerts.json";

// =====================================================
// Pins
// =====================================================

#define BUTTON_HELP 2
#define BUTTON_PC   4

#define GREEN_LED  18
#define RED_LED    19
#define YELLOW_LED 21

// =====================================================
// Persistent HTTP connection
// =====================================================

WiFiClientSecure firebaseClient;
HTTPClient firebaseHTTP;

// =====================================================
// Button states
// =====================================================

bool lastHelpState = HIGH;
bool lastPcState   = HIGH;

// =====================================================
// LED state
// =====================================================

unsigned long greenLedUntil = 0;

// =====================================================
// Statistics
// =====================================================

unsigned long totalRequests = 0;
unsigned long successfulRequests = 0;
unsigned long failedRequests = 0;

// =====================================================
// LED CONTROL
// =====================================================

void allLedsOff() {

  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(YELLOW_LED, LOW);
}

// -----------------------------------------------------

void processingOn() {

  digitalWrite(YELLOW_LED, HIGH);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
}

// -----------------------------------------------------

void processingOff() {

  digitalWrite(YELLOW_LED, LOW);
}

// -----------------------------------------------------

void successLed() {

  digitalWrite(YELLOW_LED, LOW);
  digitalWrite(RED_LED, LOW);

  digitalWrite(GREEN_LED, HIGH);

  // لا نستخدم delay
  greenLedUntil = millis() + 300;
}

// -----------------------------------------------------

void failureLed() {

  digitalWrite(YELLOW_LED, LOW);

  digitalWrite(GREEN_LED, LOW);

  // إشارة خطأ قصيرة
  digitalWrite(RED_LED, HIGH);

  delay(80);

  digitalWrite(RED_LED, LOW);
}

// =====================================================
// Non-blocking LED update
// =====================================================

void updateLEDs() {

  if (greenLedUntil != 0 && millis() >= greenLedUntil) {

    digitalWrite(GREEN_LED, LOW);

    greenLedUntil = 0;
  }
}

// =====================================================
// Firebase HTTP initialization
// =====================================================

bool initFirebaseConnection() {

  Serial.println();
  Serial.println("Initializing Firebase connection...");

  firebaseClient.setInsecure();

  firebaseClient.setTimeout(1500);

  if (!firebaseHTTP.begin(firebaseClient, firebaseURL)) {

    Serial.println("Firebase HTTP begin FAILED!");

    return false;
  }

  firebaseHTTP.setReuse(true);

  firebaseHTTP.setConnectTimeout(1500);

  firebaseHTTP.setTimeout(3000);

  firebaseHTTP.addHeader(
    "Content-Type",
    "application/json"
  );

  Serial.println("Firebase connection READY.");

  return true;
}

// =====================================================
// JSON escaping
// =====================================================

String jsonEscape(const char* text) {

  String output;

  output.reserve(strlen(text) + 10);

  while (*text) {

    char c = *text++;

    if (c == '"') {
      output += "\\\"";
    }

    else if (c == '\\') {
      output += "\\\\";
    }

    else if (c == '\n') {
      output += "\\n";
    }

    else if (c == '\r') {
      output += "\\r";
    }

    else {
      output += c;
    }
  }

  return output;
}

// =====================================================
// SEND ALERT
// =====================================================

bool sendToFirebase(
  const char* type,
  const char* message
) {

  unsigned long startTime = millis();

  totalRequests++;

  Serial.println();
  Serial.println("================================");
  Serial.println("FAST ALERT");
  Serial.println("================================");

  Serial.print("Device: ");
  Serial.println(deviceID);

  Serial.print("Type: ");
  Serial.println(type);

  // ---------------------------------------------------
  // WiFi check
  // ---------------------------------------------------

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("WiFi disconnected.");

    failedRequests++;

    failureLed();

    return false;
  }

  // ---------------------------------------------------
  // Yellow immediately
  // ---------------------------------------------------

  processingOn();

  // ---------------------------------------------------
  // Build JSON
  // ---------------------------------------------------

  String json;

  json.reserve(220);

  json =
    "{\"deviceID\":\"" +
    String(deviceID) +
    "\",\"type\":\"" +
    String(type) +
    "\",\"message\":\"" +
    jsonEscape(message) +
    "\",\"status\":\"pending\",\"responder\":\"\",\"timestamp\":{\".sv\":\"timestamp\"}}";

  // ---------------------------------------------------
  // Send
  // ---------------------------------------------------

  unsigned long postStart = millis();

  int httpCode = firebaseHTTP.POST(json);

  unsigned long postTime = millis() - postStart;

  // ---------------------------------------------------
  // Result
  // ---------------------------------------------------

  Serial.print("HTTP Code: ");
  Serial.println(httpCode);

  Serial.print("POST Time: ");
  Serial.print(postTime);
  Serial.println(" ms");

  if (httpCode == 200 || httpCode == 201) {

    successfulRequests++;

    processingOff();

    successLed();

    Serial.println("SUCCESS");

    Serial.print("TOTAL Time: ");
    Serial.print(millis() - startTime);
    Serial.println(" ms");

    Serial.print("Success: ");
    Serial.println(successfulRequests);

    return true;
  }

  // ---------------------------------------------------
  // Failed
  // ---------------------------------------------------

  Serial.println("Firebase request FAILED.");

  if (httpCode > 0) {

    Serial.print("Response: ");

    Serial.println(
      firebaseHTTP.getString()
    );
  }

  failedRequests++;

  processingOff();

  failureLed();

  Serial.print("TOTAL Time: ");
  Serial.print(millis() - startTime);
  Serial.println(" ms");

  return false;
}

// =====================================================
// WIFI CONNECT
// =====================================================

void connectWiFi() {

  Serial.println();
  Serial.println("Connecting WiFi...");

  WiFi.mode(WIFI_STA);

  WiFi.setSleep(false);

  WiFi.begin(ssid, password);

  unsigned long start = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 10000
  ) {

    digitalWrite(YELLOW_LED, !digitalRead(YELLOW_LED));

    delay(100);
  }

  digitalWrite(YELLOW_LED, LOW);

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println();
    Serial.println("WiFi CONNECTED");

    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  }

  else {

    Serial.println();
    Serial.println("WiFi CONNECTION FAILED");

    failureLed();
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(200);

  Serial.println();
  Serial.println("======================================");
  Serial.println(" MDF-AlertNet ESP32");
  Serial.println(" FAST MODE");
  Serial.println("======================================");

  // ---------------------------------------------------
  // Buttons
  // ---------------------------------------------------

  pinMode(BUTTON_HELP, INPUT_PULLUP);
  pinMode(BUTTON_PC, INPUT_PULLUP);

  // ---------------------------------------------------
  // LEDs
  // ---------------------------------------------------

  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);

  allLedsOff();

  // ---------------------------------------------------
  // WiFi
  // ---------------------------------------------------

  connectWiFi();

  // ---------------------------------------------------
  // Firebase
  // ---------------------------------------------------

  if (WiFi.status() == WL_CONNECTED) {

    if (initFirebaseConnection()) {

      successLed();

      Serial.println();
      Serial.println("======================================");
      Serial.println(" SYSTEM READY");
      Serial.println("======================================");
      Serial.println("HELP  -> GPIO 2");
      Serial.println("VOID  -> GPIO 4");
      Serial.println();
    }
  }
}

// =====================================================
// LOOP
// =====================================================

void loop() {

  updateLEDs();

  // ===================================================
  // HELP
  // ===================================================

  bool helpState = digitalRead(BUTTON_HELP);

  if (
    lastHelpState == HIGH &&
    helpState == LOW
  ) {

    Serial.println();
    Serial.println(">>> HELP PRESSED <<<");

    sendToFirebase(
      "URGENT",
      "Customer at counter requires support."
    );

    // سريع جدًا لمنع double click
    delay(80);
  }

  // ===================================================
  // VOID
  // ===================================================

  bool pcState = digitalRead(BUTTON_PC);

  if (
    lastPcState == HIGH &&
    pcState == LOW
  ) {

    Serial.println();
    Serial.println(">>> VOID PRESSED <<<");

    sendToFirebase(
      "VOID",
      "Supervisor authorization needed."
    );

    delay(80);
  }

  // ===================================================
  // Save states
  // ===================================================

  lastHelpState = helpState;
  lastPcState = pcState;

  // ===================================================
  // Very short loop
  // ===================================================

  delay(2);
}

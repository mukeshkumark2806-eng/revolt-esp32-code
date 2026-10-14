
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include "secrets.h"

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ========== PINS ==========
#define BATTERY_ADC 34
#define TEMP_PIN    4
#define LDR_PIN     33

#define UART_TX     17
#define UART_RX     16

#define BLUE_LED    25
#define GREEN_LED   26
#define RED_LED     27
#define BUTTON_PIN  32
#define BUZZER_PIN  14

// ========== BACKEND ==========
const char* BACKEND_URL =
    "http://192.168.1.6:3000/api/device/screening";

bool backendSubmitted = false;

// ========== VOLTAGE ==========
const int SAMPLES = 128;
const float RATIO = 1.9788;

// ========== TEMPERATURE ==========
OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);

// ========== STATES ==========
enum State {
  HOME = 0,
  SCREENING = 1,
  RESULT = 2,
  SOLAR = 3
};

State state = HOME;

unsigned long stateStarted = 0;
unsigned long lastSend = 0;
unsigned long lastSensorRead = 0;

const unsigned long SCREENING_TIME = 4000;
const unsigned long SEND_INTERVAL = 500;
const unsigned long SENSOR_INTERVAL = 1000;

// ========== SENSOR DATA ==========
float batteryVoltage = 0.0;
float temperature = -127.0;
float current_mA = 0.0;  // SIMULATED

bool suitable = false;
bool solarDetected = true;  // DEMO ONLY
bool charging = false;      // DEMO DISPLAY INDICATOR

// ========== BUTTON DEBOUNCE ==========
int lastButtonReading = HIGH;
int stableButtonState = HIGH;

unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_MS = 40;

// ========== VOLTAGE READING ==========
float readBatteryVoltage() {
  uint32_t total = 0;

  for (int i = 0; i < SAMPLES; i++) {
    total += analogReadMilliVolts(BATTERY_ADC);
    delayMicroseconds(500);
  }

  float adcVoltage =
      total / (SAMPLES * 1000.0);

  return adcVoltage * RATIO;
}

// ========== BUZZER ==========
void beep(int durationMs) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
}

void resultBeep() {
  if (suitable) {
    beep(100);
    delay(100);
    beep(100);
  } else {
    beep(500);
  }
}

// ========== LED CONTROL ==========
void updateLEDs() {
  digitalWrite(BLUE_LED, state == SCREENING);

  digitalWrite(
      GREEN_LED,
      state == RESULT && suitable
  );

  digitalWrite(
      RED_LED,
      state == RESULT && !suitable
  );
}

// ========== SCREENING LOGIC ==========
void evaluateBattery() {
  bool voltageOK =
      batteryVoltage >= 3.50 &&
      batteryVoltage <= 4.20;

  bool temperatureOK =
      temperature != DEVICE_DISCONNECTED_C &&
      temperature > -50.0 &&
      temperature < 45.0;

  // DEMO estimate only; not measured current.
  bool currentOK =
      current_mA > 0 &&
      current_mA <= 800.0;

  suitable =
      voltageOK &&
      temperatureOK &&
      currentOK;
}

// ========== CHARGING DISPLAY LOGIC ==========
// DEMO ONLY: This controls the displayed indicator.
// It does not activate or verify a physical charging circuit.
void updateChargingIndicator() {
  charging = suitable && solarDetected;
}

// ========== STATE CHANGE ==========
void changeState(State newState) {
  state = newState;
  stateStarted = millis();

  updateChargingIndicator();
  updateLEDs();

  if (state == SCREENING) {
    beep(80);
  } else if (state == RESULT) {
    resultBeep();
  } else if (state == SOLAR) {
    beep(80);
  }
}

// ========== BUTTON ==========
bool buttonPressed() {
  int reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
  }

  bool pressedEvent = false;

  if (millis() - lastDebounceTime > DEBOUNCE_MS) {
    if (reading != stableButtonState) {
      stableButtonState = reading;

      if (stableButtonState == LOW) {
        pressedEvent = true;
      }
    }
  }

  lastButtonReading = reading;
  return pressedEvent;
}

// ========== UART PACKET ==========
void sendToUNO() {
  Serial2.print("V:");
  Serial2.print(batteryVoltage, 2);

  Serial2.print(",T:");
  Serial2.print(temperature, 1);

  Serial2.print(",I:");
  Serial2.print(current_mA, 0);

  Serial2.print(",S:");
  Serial2.print(suitable ? 1 : 0);

  Serial2.print(",SOL:");
  Serial2.print(solarDetected ? 1 : 0);

  Serial2.print(",CHG:");
  Serial2.print(charging ? 1 : 0);

  Serial2.print(",SCR:");
  Serial2.println((int)state);
}

// ========== WI-FI ==========
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 10000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected");
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWi-Fi connection failed");
  }
}

// ========== BACKEND SUBMISSION ==========
void submitScreening() {
  if (backendSubmitted) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(
        "Backend submission skipped: Wi-Fi disconnected"
    );
    return;
  }

  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, BACKEND_URL)) {
    Serial.println("HTTP initialization failed");
    return;
  }

  http.setTimeout(2500);

  String payload = "{";
  payload += "\"voltage\":" + String(batteryVoltage, 2) + ",";
  payload += "\"temperature\":" + String(temperature, 1) + ",";
  payload += "\"current_mA\":" + String(current_mA, 0) + ",";
  payload += "\"suitable\":";
  payload += suitable ? "true" : "false";
  payload += ",\"testDurationSeconds\":4";
  payload += ",\"deviceId\":\"REVOLTCELL-ESP32-001\"";
  payload += "}";

  http.addHeader("Content-Type", "application/json");

  Serial.println("Submitting screening to backend...");

  Serial.println("Testing backend TCP connection...");

  WiFiClient testClient;

  bool tcpConnected =
      testClient.connect("192.168.1.6", 3000);

  Serial.print("TCP connection: ");
  Serial.println(
      tcpConnected ? "SUCCESS" : "FAILED"
  );

  testClient.stop();

  int statusCode = http.POST(payload);

  Serial.print("WiFi status: ");
  Serial.println(WiFi.status());

  Serial.print("Backend URL: ");
  Serial.println(BACKEND_URL);

  Serial.print("HTTP result: ");
  Serial.println(statusCode);

  if (statusCode < 0) {
    Serial.print("HTTP error: ");
    Serial.println(http.errorToString(statusCode));
  }

  if (statusCode == 201) {
    Serial.println("Screening saved successfully!");
    Serial.println(http.getString());

    backendSubmitted = true;
  } else {
    Serial.print("Backend submission failed. HTTP status: ");
    Serial.println(statusCode);
  }

  http.end();
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);

  connectWiFi();

  Serial2.begin(
      9600,
      SERIAL_8N1,
      UART_RX,
      UART_TX
  );

  analogSetAttenuation(ADC_11db);

  pinMode(LDR_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(BLUE_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);

  tempSensor.begin();

  // Keep temperature conversions non-blocking.
  tempSensor.setWaitForConversion(false);
  tempSensor.requestTemperatures();

  randomSeed(micros());

  updateChargingIndicator();
  updateLEDs();

  Serial.println("RE-VOLT READY");
  Serial.println("Current estimate: DEMO ONLY");
  Serial.println("Charging indicator: DEMO ONLY");
}

// ========== LOOP ==========
void loop() {
  // ----- Button-controlled flow -----
  if (buttonPressed()) {
    if (state == HOME) {
      backendSubmitted = false;
      changeState(SCREENING);

    } else if (state == RESULT) {
      changeState(SOLAR);

    } else if (state == SOLAR) {
      changeState(HOME);
    }

    // Button presses during screening are ignored.
  }

  // ----- Update real sensors periodically -----
  if (millis() - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = millis();

    batteryVoltage = readBatteryVoltage();

    temperature = tempSensor.getTempCByIndex(0);
    tempSensor.requestTemperatures();

    // DEMO estimate only:
    // 600 mA at 3.70 V; not a real measurement.
    current_mA = 600.0 * (batteryVoltage / 3.70);

    if (state == SCREENING) {
      evaluateBattery();
    }
  }

  // ----- Finish screening automatically -----
  if (state == SCREENING &&
      millis() - stateStarted >= SCREENING_TIME) {

    evaluateBattery();

    // Save screening result to backend.
    submitScreening();

    changeState(RESULT);
  }

  // ----- FIX: Update charging indicator continuously -----
  updateChargingIndicator();

  // ----- Send fresh data to UNO -----
  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();

    sendToUNO();

    Serial.print("V=");
    Serial.print(batteryVoltage, 2);

    Serial.print(" T=");
    Serial.print(temperature, 1);

    Serial.print(" I_DEMO=");
    Serial.print(current_mA, 0);

    Serial.print(" Suitable=");
    Serial.print(suitable ? 1 : 0);

    Serial.print(" ChargingDemo=");
    Serial.print(charging ? 1 : 0);

    Serial.print(" State=");
    Serial.println((int)state);
  }
}

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <MQTT.h>
#include <ArduinoJson.h>
#include <debounce.h>                   // (lib/kimballa-button-debounce)
#include "Balboa_GS_Interface.h"        // https://github.com/MagnusPer/Balboa-GS510SZ
#include "secrets.h"                    // gitignored - copy secrets.h.example and fill in real values

// SPA display controller for Balboa system GS (Z-suffix controllers only - see README/COMMENTS_ARCHIVE.md).
// Publishes water temp/heater/pump/light status to HA, merged into the same "Bubbelkopp" device
// as SDM-Universal-Env. BalboaInterface's own write path is dead code - GPIO button pulses below
// are the only mechanism that actually works.

//////////////////////////////////////////////////////////////////////////////
// CONFIGURATION - everything you're likely to want to tweak lives below.
//////////////////////////////////////////////////////////////////////////////

// WiFi
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* WIFI_HOSTNAME = "BubbelkoppSpa";

// MQTT broker
const char* MQTT_BROKER_HOST = MQTT_HOST;
const char* MQTT_USER_NAME = MQTT_USER;
const char* MQTT_PASSWORD_SECRET = MQTT_PASSWORD;
const int MQTT_BUFFER_SIZE = 2048; // discovery config payloads can approach 1024 - keep headroom

// HA topic naming: base/domain/deviceName/cdName<unique>/suffix (see buildTopic()).
const String base = "hemma";
String domain = "sensor";
const String clientName = "spa";
const String deviceName = "bubbelkopp";

// Shared HA device metadata - must stay identical to SDM-Universal-Env's bubbelkopp fields.
const char* DEVICE_MANUFACTURER = "StarkJohan";
const char* DEVICE_MODEL = "SDM-HA bridge";
const char* DEVICE_SW_VERSION = "0.2b";
const char* DEVICE_HW_VERSION = "0.1a";
const char* DEVICE_CONFIG_URL = "https://snutt.net/";

// Display/clock protocol pins - shared by every controller variant.
const uint8_t PIN_CLOCK = D1; // GPIO 5
const uint8_t PIN_READ  = D2; // GPIO 4

#if defined(CONTROLLER_Z)
  // Z suffix: 4 discrete button GPIO lines, pulsed HIGH to simulate a physical press.
  const uint8_t PIN_LIGHT = D8; // GPIO 15 - also read passively, see checkPhysicalButtons()
  const uint8_t PIN_UP    = D5; // GPIO 14
  const uint8_t PIN_DOWN  = D6; // GPIO 12
  const uint8_t PIN_PUMP  = D7; // GPIO 13
#elif defined(CONTROLLER_SZ)
  #error "SZ/D controller button-write not implemented yet - see README"
#else
  #error "No controller variant selected: build with -e z or -e sz (see platformio.ini)"
#endif

// How long to hold a simulated button-press pulse.
const unsigned long BUTTON_PULSE_MS = 100;

// OTA
const int OTA_PORT = 8266;
const char* OTA_PASSWORD_SECRET = OTA_PASSWORD;

// How often to read + publish Balboa status.
const unsigned long STATE_PUBLISH_INTERVAL_MS = 5000;

// A Temp Up/Down press makes the panel blink to show the SET temp instead of water temp - a
// blank frame within this many ms counts as still blinking (see isBlinking()).
const unsigned long BLINK_TIMEOUT_MS = 2000;

// How long a settled water-temp reading must hold before the boot reveal press fires (see loop()).
const unsigned long SET_TEMP_BOOT_SETTLE_MS = 3000;

// Number entity target range - the SPA's hard limits.
const int SET_TEMP_MIN = 26;
const int SET_TEMP_MAX = 40;

// Spacing between simulated presses while walking to a number entity's target - must exceed one
// full blink confirm cycle (~700-1000ms observed) or the walk overshoots/oscillates around the
// target (see COMMENTS_ARCHIVE.md for the tuning history).
const unsigned long WALK_STEP_INTERVAL_MS = 1200;

// Consecutive no-progress presses before assuming one landed outside the SPA's adjust-mode
// window and forcing a fresh press episode (see loop()).
const uint8_t WALK_STALL_LIMIT = 3;

// Safety cap on how long a walk-to-target may run before giving up.
const unsigned long WALK_MAX_DURATION_MS = 30000;

// A changed boolean status reading must persist this long before being accepted (filters
// single-cycle noise flips, see DebouncedBool).
const unsigned long STATUS_DEBOUNCE_MS = 250;

// NTP - feeds _boottime/_uptime only. Uses configTime()/time() (not NTPClient's manual offset)
// so HA's timestamp_local filter doesn't double-apply the timezone shift.
const char* NTP_SERVER = "10.10.10.1";
const char* NTP_TZ = "CET-1CEST,M3.5.0/02,M10.5.0/03";

// Connectivity retry/outage timing.
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 10000;
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;
const unsigned long MAX_OFFLINE_MS = 5UL * 60UL * 1000UL; // 5 min

// Bounded boot waits - WiFi reboots on timeout, NTP/MQTT are best-effort and keep retrying after.
const uint8_t WIFI_CONNECT_MAX_ATTEMPTS = 60; // * 500ms delay = ~30s
const uint8_t NTP_MAX_ATTEMPTS = 10;          // * 1s delay = ~10s
const uint8_t MQTT_CONNECT_MAX_ATTEMPTS = 10; // * 1s delay = ~10s

//////////////////////////////////////////////////////////////////////////////
// RUNTIME STATE - derived values and objects, not meant to be edited directly.
//////////////////////////////////////////////////////////////////////////////

const String cdName = deviceName + "_" + clientName; // bubbelkopp_spa

String aTopic, dTopic, sTopic, attrTopic, cTopic, writeTopic, updateTempTopic, testTopic;

WiFiClient wclient;
MQTTClient client(MQTT_BUFFER_SIZE);

time_t now;
String tsBoottime;
unsigned long lastPublish = 0;

BalboaInterface Balboa(PIN_CLOCK, PIN_READ, PIN_LIGHT);

unsigned long lastBlankFrameMs = 0; // last blank frame seen; isBlinking() true within BLINK_TIMEOUT_MS of this

// Water vs set temp can only be told apart in hindsight (a frame followed by a blank was the
// set temp) - tracked here rather than via the library's forward-only setTempActive.
String previousFrame = "";
unsigned long previousFrameMs = 0;
int currentWaterTemp = -1;
int currentSetTemp = -1;
int waterTempBeforeLastWrite = -1; // for rolling back a water-temp write that turns out to be set-temp
int waterTempAtLastSettleCheck = -1;
unsigned long waterTempSettledSinceMs = 0;
bool bootSetTempTriggered = false; // fires exactly once per boot

// Number entity walk-to-target state (see loop()/messageReceived()).
bool walkTargetActive = false;
int walkTargetValue = -1;
unsigned long walkLastPressMs = 0;
unsigned long walkStartMs = 0;
int walkLastSeenSetTemp = -999; // sentinel, distinct from the real "unknown" value of -1
uint8_t walkStallCount = 0;

unsigned long blinkStartMs = 0; // when the current blink episode began (see markBlinkStart())
// True until the episode's first blank is seen - the panel hasn't necessarily redrawn yet on
// the very first blank, so that one is skipped rather than trusted (see loop()).
bool blinkFirstBlankPending = false;

// Requires a changed reading to persist for STATUS_DEBOUNCE_MS before accepting it.
struct DebouncedBool {
  bool confirmed = false;
  bool candidate = false;
  unsigned long candidateSinceMs = 0;
  bool hasCandidate = false;

  void update(bool raw, unsigned long nowMs) {
    if (raw == confirmed) {
      hasCandidate = false;
      return;
    }
    if (!hasCandidate || candidate != raw) {
      candidate = raw;
      candidateSinceMs = nowMs;
      hasCandidate = true;
      return;
    }
    if (nowMs - candidateSinceMs >= STATUS_DEBOUNCE_MS) {
      confirmed = raw;
      hasCandidate = false;
    }
  }
};
DebouncedBool debouncedPump1;
DebouncedBool debouncedLight;
DebouncedBool debouncedUnknownFlag;

// Last-published snapshot, for detecting a real change worth publishing immediately (see stateChanged()).
int lastPubWaterTemp = -1;
int lastPubSetTemp = -1;
String lastPubDisplay = "";
bool lastPubHeater = false;
bool lastPubPump1 = false;
bool lastPubLight = false;
bool lastPubUnknownFlag = false;
bool hasPublishedOnce = false;

unsigned long offlineSince = 0;      // 0 while WiFi+MQTT are both up
unsigned long lastWifiAttempt = 0;
unsigned long lastMqttAttempt = 0;

//////////////////////////////////////////////////////////////////////////////

String buildTopic(String devName, String uid, String domain, String unique, String suffix) {
  return base + "/" + domain + "/" + devName + "/" + uid + unique + "/" + suffix;
}

void publishDebug(String debugString) {
  if (client.connected()) {
    client.publish(dTopic.c_str(), debugString, false, 1);
  }
}

// Board identity/diagnostics, published as HA's json_attributes_topic on every entity.
void publishAttributes() {
  StaticJsonDocument<256> attrs;
  attrs["_clientname"] = cdName;
  attrs["_ip-adr"] = WiFi.localIP().toString();
  attrs["_boottime"] = tsBoottime.toInt();

  time(&now);
  attrs["_updated"] = now;
  attrs["_uptime"] = (unsigned long)now - (unsigned long)tsBoottime.toInt();

  String output;
  serializeJson(attrs, output);
  client.publish(attrTopic.c_str(), output, false, 1);
}

// One JSON blob with every read field; each entity's value_template pulls its own key back out.
void publishState() {
  StaticJsonDocument<256> state;
  state["_water_temp"] = currentWaterTemp;
  state["_set_temp"] = currentSetTemp;
  state["_heater"] = Balboa.displayHeater;
  state["_pump1"] = debouncedPump1.confirmed;
  state["_light"] = debouncedLight.confirmed;
  state["_display"] = Balboa.LCD_display;
  state["_unknown_flag"] = debouncedUnknownFlag.confirmed;

  String output;
  serializeJson(state, output);
  client.publish(sTopic.c_str(), output, false, 1);

  lastPubWaterTemp = currentWaterTemp;
  lastPubSetTemp = currentSetTemp;
  lastPubDisplay = Balboa.LCD_display;
  lastPubHeater = Balboa.displayHeater;
  lastPubPump1 = debouncedPump1.confirmed;
  lastPubLight = debouncedLight.confirmed;
  lastPubUnknownFlag = debouncedUnknownFlag.confirmed;
  hasPublishedOnce = true;
}

// True if any field differs from what was last published (see loop()). Display/Heater are
// tracked raw/undebounced on purpose - real-time diagnostics, at the cost of more publishes.
bool stateChanged() {
  if (!hasPublishedOnce) return true;
  return currentWaterTemp != lastPubWaterTemp
      || currentSetTemp != lastPubSetTemp
      || Balboa.LCD_display != lastPubDisplay
      || Balboa.displayHeater != lastPubHeater
      || debouncedPump1.confirmed != lastPubPump1
      || debouncedLight.confirmed != lastPubLight
      || debouncedUnknownFlag.confirmed != lastPubUnknownFlag;
}

// entName also doubles as the state-blob JSON key. customExpr overrides the default
// value_json.<entName> lookup; stateTopicOverride reads from attrTopic instead of sTopic.
void publishEntity(StaticJsonDocument<400>& jObj, String domain, String entName, String friendlyName, String device_class, String state_class, String unit_of_measurement, int precision = -1, String customExpr = "", String entityCategory = "", String stateTopicOverride = "") {
  String output;
  StaticJsonDocument<900> ent;

  ent["device"] = jObj;
  if (device_class.length() > 0) ent["device_class"] = device_class;
  if (state_class.length() > 0) ent["state_class"] = state_class;
  if (unit_of_measurement.length() > 0) ent["unit_of_measurement"] = unit_of_measurement;
  if (precision >= 0) ent["suggested_display_precision"] = precision;
  if (entityCategory.length() > 0) ent["entity_category"] = entityCategory;
  ent["name"] = friendlyName;
  ent["unique_id"] = cdName + entName;

  ent["state_topic"] = stateTopicOverride.length() > 0 ? stateTopicOverride : sTopic;
  ent["availability_topic"] = aTopic;
  ent["json_attributes_topic"] = attrTopic;
  String expr = customExpr.length() > 0 ? customExpr : ("value_json." + entName);
  ent["value_template"] = "{{ " + expr + " }}";

  serializeJson(ent, output);

  cTopic = buildTopic(deviceName, cdName, domain, entName, "config");
  if (!client.publish(cTopic.c_str(), output, true, 1)) {
    publishDebug("Entity config publish FAILED for " + entName + ", length=" + String(output.length()));
  }
}

// Button entity for one of the four hard-wired GPIO functions; all share writeTopic's dispatch.
void publishButtonEntity(StaticJsonDocument<400>& jObj, String entName, String friendlyName, String payloadPress) {
  String output;
  StaticJsonDocument<700> ent;

  ent["device"] = jObj;
  ent["name"] = friendlyName;
  ent["unique_id"] = cdName + entName;
  ent["command_topic"] = writeTopic;
  ent["payload_press"] = payloadPress;
  ent["availability_topic"] = aTopic;
  ent["json_attributes_topic"] = attrTopic;

  serializeJson(ent, output);

  cTopic = buildTopic(deviceName, cdName, "button", entName, "config");
  if (!client.publish(cTopic.c_str(), output, true, 1)) {
    publishDebug("Button config publish FAILED for " + entName + ", length=" + String(output.length()));
  }
}

// HA light entity - payload_on/off must match state_value_template's own ON/OFF output (MQTT
// light has no state_on/off override like switch does). messageReceived() only pulses the
// physical toggle when the commanded direction differs from the debounced real state.
void publishLightEntity(StaticJsonDocument<400>& jObj, String entName, String friendlyName) {
  String output;
  StaticJsonDocument<900> ent;

  ent["device"] = jObj;
  ent["name"] = friendlyName;
  ent["unique_id"] = cdName + entName;
  ent["command_topic"] = writeTopic;
  ent["state_topic"] = sTopic;
  ent["state_value_template"] = "{{ 'ON' if value_json._light else 'OFF' }}";
  ent["payload_on"] = "ON";
  ent["payload_off"] = "OFF";
  ent["icon"] = "mdi:lightbulb";
  ent["availability_topic"] = aTopic;
  ent["json_attributes_topic"] = attrTopic;

  serializeJson(ent, output);

  cTopic = buildTopic(deviceName, cdName, "light", entName, "config");
  if (!client.publish(cTopic.c_str(), output, true, 1)) {
    publishDebug("Light config publish FAILED for " + entName + ", length=" + String(output.length()));
  }
}

// Number entity for the target set temp (26-40C) - command_topic reuses updateTempTopic, and
// loop()'s walk state machine turns a target into real Up/Down presses.
void publishNumberEntity(StaticJsonDocument<400>& jObj, String entName, String friendlyName) {
  String output;
  StaticJsonDocument<900> ent;

  ent["device"] = jObj;
  ent["name"] = friendlyName;
  ent["unique_id"] = cdName + entName;
  ent["command_topic"] = updateTempTopic;
  ent["state_topic"] = sTopic;
  ent["value_template"] = "{{ value_json._set_temp }}";
  ent["min"] = SET_TEMP_MIN;
  ent["max"] = SET_TEMP_MAX;
  ent["step"] = 1;
  ent["unit_of_measurement"] = "\xC2\xB0" "C";
  ent["mode"] = "slider";
  ent["availability_topic"] = aTopic;
  ent["json_attributes_topic"] = attrTopic;

  serializeJson(ent, output);

  cTopic = buildTopic(deviceName, cdName, "number", entName, "config");
  if (!client.publish(cTopic.c_str(), output, true, 1)) {
    publishDebug("Number config publish FAILED for " + entName + ", length=" + String(output.length()));
  }
}

// Removes the superseded switch-domain Lights entity - safe/idempotent, remove once confirmed gone.
void clearOldLightEntity() {
  cTopic = buildTopic(deviceName, cdName, "switch", "_light", "config");
  client.publish(cTopic.c_str(), "", true, 1);
}

// Republishes HA discovery + online status and resubscribes - runs after every fresh MQTT connect.
void announceMqttConnected() {
  StaticJsonDocument<400> dev;
  dev["identifiers"][0] = deviceName;
  dev["name"] = "Bubbelkopp";
  dev["manufacturer"] = DEVICE_MANUFACTURER;
  dev["model"] = DEVICE_MODEL;
  dev["sw_version"] = DEVICE_SW_VERSION;
  dev["hw_version"] = DEVICE_HW_VERSION;
  dev["configuration_url"] = DEVICE_CONFIG_URL;

  publishEntity(dev, "sensor", "_water_temp", "Water temperature", "temperature", "measurement", "\xC2\xB0" "C", 0);
  publishEntity(dev, "sensor", "_set_temp", "Set temperature", "temperature", "measurement", "\xC2\xB0" "C", 0);
  publishNumberEntity(dev, "_set_temp_target", "Set temperature target");
  publishEntity(dev, "binary_sensor", "_heater", "Heater", "heat", "", "", -1, "'ON' if value_json._heater else 'OFF'");
  publishEntity(dev, "binary_sensor", "_pump1", "Pump 1", "running", "", "", -1, "'ON' if value_json._pump1 else 'OFF'");
  publishEntity(dev, "sensor", "_display", "Display", "", "", "", -1, "", "diagnostic");
  publishEntity(dev, "binary_sensor", "_unknown_flag", "Unknown flag (bit23)", "", "", "", -1, "'ON' if value_json._unknown_flag else 'OFF'", "diagnostic");
  // Read-only mirror of Lights' real status, distinct unique_id from the light entity below.
  publishEntity(dev, "binary_sensor", "_light_diag", "Lights", "light", "", "", -1, "'ON' if value_json._light else 'OFF'", "diagnostic");

  publishEntity(dev, "sensor", "_uptime", "Uptime", "duration", "measurement", "s", 0, "", "diagnostic", attrTopic);
  publishEntity(dev, "sensor", "_boottime", "Boottime", "timestamp", "", "", -1, "value_json._boottime | timestamp_local", "diagnostic", attrTopic);

  publishButtonEntity(dev, "_temp_up", "Temp Up", "TempUp");
  publishButtonEntity(dev, "_temp_down", "Temp Down", "TempDown");
  publishButtonEntity(dev, "_pump1_btn", "Pump 1", "Pump1");

  clearOldLightEntity();
  publishLightEntity(dev, "_light", "Lights");

  client.publish(aTopic.c_str(), "online", true, 1);
  publishAttributes();
  publishDebug("MQTT connected");

  client.subscribe(writeTopic.c_str());
  client.subscribe(updateTempTopic.c_str());
}

// The library falls back to '-' for any unrecognized segment pattern - reject the whole frame
// (all bits, same 23-bit cycle) rather than propagate a glitch like "3-"/"J-"/"--".
bool isCorruptedFrame(const String &s) {
  return s.indexOf('-') >= 0;
}

// Every char a space = the blank half of the blink cycle; "" (no decode yet) must NOT count.
bool isBlankFrame(const String &s) {
  if (s.length() == 0) return false;
  for (unsigned int i = 0; i < s.length(); i++) {
    if (s[i] != ' ') return false;
  }
  return true;
}

// True while the panel is (or was very recently) showing the set temp - see BLINK_TIMEOUT_MS.
bool isBlinking() {
  return lastBlankFrameMs != 0 && (millis() - lastBlankFrameMs < BLINK_TIMEOUT_MS);
}

// Arms the blink window proactively the instant we press, rather than waiting to see a blank
// frame prove it. Only advances blinkStartMs on a genuinely new episode (see loop()).
void markBlinkStart() {
  if (!isBlinking()) {
    blinkStartMs = millis();
    blinkFirstBlankPending = true;
  }
  lastBlankFrameMs = millis();
}

// Simulates a physical button press (CONTROLLER_Z only).
void pulseButton(uint8_t pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
  delay(BUTTON_PULSE_MS);
  digitalWrite(pin, LOW);
  pinMode(pin, INPUT);
}

void messageReceived(String &topic, String &payload) {
  if (topic == writeTopic) {
    if (payload == "TempUp") { markBlinkStart(); pulseButton(PIN_UP); }
    else if (payload == "TempDown") { markBlinkStart(); pulseButton(PIN_DOWN); }
    // Only pulse if the debounced real state differs from what was requested (toggle-only button).
    else if (payload == "ON") { if (!debouncedLight.confirmed) pulseButton(PIN_LIGHT); }
    else if (payload == "OFF") { if (debouncedLight.confirmed) pulseButton(PIN_LIGHT); }
    else if (payload == "Lights") { pulseButton(PIN_LIGHT); } // legacy/manual blind toggle
    else if (payload == "Pump1") { pulseButton(PIN_PUMP); }
    else if (payload == "Stop") { Balboa.stop(); }
    else if (payload == "Reset") { ESP.restart(); }
  } else if (topic == updateTempTopic) {
    // Number entity target - loop()'s walk state machine drives this to real Up/Down presses.
    int target = payload.toInt();
    if (target >= SET_TEMP_MIN && target <= SET_TEMP_MAX) {
      walkTargetValue = target;
      walkTargetActive = true;
      walkLastPressMs = 0; // fire the first press as soon as loop() next checks
      walkStartMs = millis();
      walkLastSeenSetTemp = -999;
      walkStallCount = 0;
    }
  }
}

// Passive debounced monitoring of the 4 hard-wired lines (idle LOW/pulse HIGH, opposite of the
// debounce library's convention, hence !digitalRead()).
enum PhysicalButtonId : uint8_t { BTN_ID_UP = 0, BTN_ID_DOWN = 1, BTN_ID_PUMP = 2, BTN_ID_LIGHT = 3 };

void physicalButtonHandler(uint8_t id, uint8_t state) {
  if (state != BTN_PRESSED) return;

  const char* label;
  switch (id) {
    case BTN_ID_UP:    label = "TempUp";   markBlinkStart(); break;
    case BTN_ID_DOWN:  label = "TempDown"; markBlinkStart(); break;
    case BTN_ID_PUMP:  label = "Pump1";    break;
    case BTN_ID_LIGHT: label = "Lights";   break;
    default:           return;
  }
  publishDebug(String("Physical button pressed: ") + label);
  client.publish(testTopic.c_str(), label, false, 0);
}

Button btnUp(BTN_ID_UP, physicalButtonHandler);
Button btnDown(BTN_ID_DOWN, physicalButtonHandler);
Button btnPump(BTN_ID_PUMP, physicalButtonHandler);
Button btnLight(BTN_ID_LIGHT, physicalButtonHandler);

void checkPhysicalButtons() {
  btnUp.update(!digitalRead(PIN_UP));
  btnDown.update(!digitalRead(PIN_DOWN));
  btnPump.update(!digitalRead(PIN_PUMP));
  btnLight.update(!digitalRead(PIN_LIGHT));
}

// Bounded boot connect - WiFi reboots on timeout; MQTT is best-effort, retried later in loop().
void connect() {
  uint8_t wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    if (++wifiAttempts >= WIFI_CONNECT_MAX_ATTEMPTS) {
      ESP.restart();
    }
  }

  uint8_t mqttAttempts = 0;
  while (!client.connect(cdName.c_str(), MQTT_USER_NAME, MQTT_PASSWORD_SECRET)) {
    delay(1000);
    if (++mqttAttempts >= MQTT_CONNECT_MAX_ATTEMPTS) {
      return; // continue booting, keep retrying in the background
    }
  }
  announceMqttConnected();
}

// Non-blocking reconnect - never delays/blocks, so OTA and MQTT keep servicing during an outage.
void maintainConnectivity() {
  unsigned long nowMs = millis();
  bool wifiUp = (WiFi.status() == WL_CONNECTED);

  if (!wifiUp) {
    if (nowMs - lastWifiAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
      lastWifiAttempt = nowMs;
      WiFi.reconnect();
    }
  } else if (!client.connected()) {
    if (nowMs - lastMqttAttempt >= MQTT_RECONNECT_INTERVAL_MS) {
      lastMqttAttempt = nowMs;
      if (client.connect(cdName.c_str(), MQTT_USER_NAME, MQTT_PASSWORD_SECRET)) {
        announceMqttConnected();
      }
    }
  }

  bool allUp = wifiUp && client.connected();
  if (allUp) {
    offlineSince = 0;
  } else {
    if (offlineSince == 0) {
      offlineSince = nowMs;
    } else if (nowMs - offlineSince >= MAX_OFFLINE_MS) {
      ESP.restart();
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Welcome to SPA - Balboa system GS");

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  pinMode(PIN_UP, INPUT);
  pinMode(PIN_DOWN, INPUT);
  pinMode(PIN_PUMP, INPUT);
  pinMode(PIN_LIGHT, INPUT);

  aTopic = buildTopic(deviceName, cdName, domain, "", "availability");
  dTopic = buildTopic(deviceName, cdName, domain, "", "debug");
  sTopic = buildTopic(deviceName, cdName, domain, "", "state");
  attrTopic = buildTopic(deviceName, cdName, domain, "", "attributes");
  writeTopic = buildTopic(deviceName, cdName, domain, "", "write");
  updateTempTopic = buildTopic(deviceName, cdName, domain, "", "updatetemp");
  testTopic = buildTopic(deviceName, cdName, domain, "", "test");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.hostname(WIFI_HOSTNAME);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.begin(ssid, password);

  client.begin(MQTT_BROKER_HOST, wclient);
  client.setWill(aTopic.c_str(), "offline", true, 1);
  client.onMessage(messageReceived);

  connect(); // WiFi + MQTT

  // Runs after connect() so WiFi is confirmed up first; best-effort/bounded, only feeds a
  // cosmetic boottime attribute (self-corrected later in loop() as a backstop).
  configTime(NTP_TZ, NTP_SERVER);
  for (uint8_t ntpAttempts = 0; time(&now) < 1600000000L && ntpAttempts < NTP_MAX_ATTEMPTS; ntpAttempts++) {
    delay(1000);
  }
  tsBoottime = String(now); // best-effort; self-corrects in loop() below if NTP hadn't synced yet here

  Balboa.begin();

  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setPassword(OTA_PASSWORD_SECRET);
  ArduinoOTA.setHostname(WIFI_HOSTNAME);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  Serial.println("Setup complete");
}

void loop() {
  ArduinoOTA.handle();

  Balboa.loop();

  // Retroactively confirm the *previous* iteration's frame now that we know what followed it.
  String frame = Balboa.LCD_display;
  unsigned long nowFrameMs = millis();

  // Corrupted frame - skip this whole cycle; previousFrame/previousFrameMs stay untouched.
  if (!isCorruptedFrame(frame)) {
    if (isBlankFrame(frame)) {
      if (!isBlinking()) {
        // Blink started for an unknown reason (no markBlinkStart()) - don't trust the preceding frame.
        blinkStartMs = nowFrameMs;
        blinkFirstBlankPending = true;
      }
      // Confirms the preceding non-blank frame as the set temp, but only if captured after
      // blinkStartMs, and never on the episode's first blank (panel may not have redrawn yet).
      if (blinkFirstBlankPending) {
        blinkFirstBlankPending = false;
      } else if (previousFrame.length() > 0 && !isBlankFrame(previousFrame) && previousFrameMs >= blinkStartMs) {
        int v = previousFrame.toInt();
        if (v > 0) {
          currentSetTemp = v;
          // Undo an optimistic water-temp write if it turns out that frame was actually the set temp.
          if (currentWaterTemp == v) {
            currentWaterTemp = waterTempBeforeLastWrite;
          }
        }
      }
      lastBlankFrameMs = nowFrameMs;
    } else if (frame.length() > 0) {
      // Only a confirmed water-temp reading outside a blink window; written optimistically
      // otherwise (rollback above corrects a wrong guess within one cycle).
      if (!isBlinking()) {
        int v = frame.toInt();
        if (v > 0) {
          waterTempBeforeLastWrite = currentWaterTemp;
          currentWaterTemp = v;
        }
      }
    }
    previousFrame = frame;
    previousFrameMs = nowFrameMs;

    // Heater is deliberately excluded from debouncing - see stateChanged().
    debouncedPump1.update(Balboa.displayPump1, nowFrameMs);
    debouncedLight.update(Balboa.displayLight, nowFrameMs);
    debouncedUnknownFlag.update(Balboa.displayBit23, nowFrameMs);
  } // !isCorruptedFrame(frame)

  // Boot set-temp reveal: once water temp has settled, simulate a single Up press to learn it.
  if (currentWaterTemp != waterTempAtLastSettleCheck) {
    waterTempAtLastSettleCheck = currentWaterTemp;
    waterTempSettledSinceMs = nowFrameMs;
  }
  if (!bootSetTempTriggered && currentSetTemp == -1 && currentWaterTemp > 0 && !isBlinking()
      && (nowFrameMs - waterTempSettledSinceMs) >= SET_TEMP_BOOT_SETTLE_MS) {
    bootSetTempTriggered = true;
    publishDebug("Boot: water temp settled at " + String(currentWaterTemp) + ", triggering Up press to reveal set temp");
    markBlinkStart();
    pulseButton(PIN_UP);
  }

  checkPhysicalButtons();

  client.loop(); // MQTT

  // Number entity walk-to-target: repeated single presses spaced WALK_STEP_INTERVAL_MS apart.
  // Uses a fresh timestamp, not nowFrameMs (captured before client.loop() ran) - comparing
  // against a stale nowFrameMs could underflow and cancel the walk instantly (observed live).
  unsigned long nowWalkMs = millis();
  if (walkTargetActive) {
    if (currentSetTemp == walkTargetValue) {
      walkTargetActive = false;
    } else if ((nowWalkMs - walkStartMs) >= WALK_MAX_DURATION_MS) {
      walkTargetActive = false;
      publishDebug("Number: walk to target " + String(walkTargetValue) + " timed out at " + String(currentSetTemp));
    } else if ((nowWalkMs - walkLastPressMs) >= WALK_STEP_INTERVAL_MS) {
      walkLastPressMs = nowWalkMs;
      if (currentSetTemp == walkLastSeenSetTemp) {
        walkStallCount++;
      } else {
        walkLastSeenSetTemp = currentSetTemp;
        walkStallCount = 0;
      }
      if (walkStallCount >= WALK_STALL_LIMIT) {
        // No progress for a while - force a clean break so the next press starts a fresh episode.
        walkStallCount = 0;
        lastBlankFrameMs = 0;
        publishDebug("Number: walk stalled at " + String(currentSetTemp) + ", forcing a fresh press episode");
      } else {
        markBlinkStart();
        // Unknown set temp (-1) presses Up - a fresh press only reveals, never adjusts, so it's safe.
        pulseButton((currentSetTemp == -1 || currentSetTemp < walkTargetValue) ? PIN_UP : PIN_DOWN);
      }
    }
  }

  maintainConnectivity(); // non-blocking WiFi/MQTT reconnect

  unsigned long nowMs = millis();
  // LCD_display stays "" until the first real decode - used to avoid publishing default-false fields.
  bool hasValidRead = (Balboa.LCD_display.length() > 0);
  bool heartbeatDue = (nowMs - lastPublish > STATE_PUBLISH_INTERVAL_MS);

  // Publish immediately on a real change (sTopic isn't retained); heartbeat is a periodic backstop.
  if (client.connected() && hasValidRead && (stateChanged() || heartbeatDue)) {
    publishState();
  }

  if (heartbeatDue) {
    lastPublish = nowMs;

    // Self-corrects tsBoottime the first time the clock looks real, in case NTP hadn't synced at boot.
    time(&now);
    if (tsBoottime.toInt() < 1600000000L && now >= 1600000000) {
      tsBoottime = String((unsigned long)now - (nowMs / 1000));
    }

    if (client.connected()) {
      publishAttributes();
    }
  }
}

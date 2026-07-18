#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <MQTT.h>
#include <ArduinoJson.h>
#include <debounce.h>                   // (lib/kimballa-button-debounce)
#include <FastLED.h>                    // RGB status LED
#include "Balboa_GS_Interface.h"        // https://github.com/MagnusPer/Balboa-GS510SZ
#include "secrets.h"                    // gitignored - copy secrets.h.example and fill in real values

// SPA display controller for Balboa system GS (Z-suffix controllers only - see README).
// Publishes water temp/heater/pump/light status to HA, merged into the same "Bubbelkopp" device
// as SDM-Universal-Env. BalboaInterface's own write path is dead code - GPIO button pulses below
// are the only mechanism that actually works.

//////////////////////////////////////////////////////////////////////////////
// CONFIGURATION 
//////////////////////////////////////////////////////////////////////////////

// WiFi
const char* ssid = SECRET_WIFI_SSID;
const char* password = SECRET_WIFI_PASSWORD;
const char* WIFI_HOSTNAME = "BubbelkoppSpa";

// MQTT broker
const char* MQTT_BROKER_HOST = SECRET_MQTT_HOST;
const char* MQTT_USER = SECRET_MQTT_USER;
const char* MQTT_PASSWORD = SECRET_MQTT_PASSWORD;
const int MQTT_BUFFER_SIZE = 2048; // discovery config payloads can approach 1024 - keep headroom

// HA topic naming: base/domain/deviceName/cdName<unique>/suffix (see buildTopic()).
const String base = "hemma";
const String domain = "sensor";
const String clientName = "spa";
const String deviceName = "bubbelkopp";

// Shared HA device metadata - must stay identical to SDM-Universal-Env's bubbelkopp fields.
const char* DEVICE_MANUFACTURER = "StarkJohan";
const char* DEVICE_MODEL = "HA Bridge";
const char* DEVICE_SW_VERSION = "0.3";
const char* DEVICE_HW_VERSION = "0.1a";
const char* DEVICE_CONFIG_URL = "https://snutt.net/";

// Display/clock protocol pins - shared by every controller variant.
// Raw GPIO numbers, not Wemos D1 mini Dx aliases - this is a custom ESP8266 board with its own
// pinout, not a real D1 mini (platformio.ini's board=d1_mini is only there for toolchain/flash
// matching, see the comment there).
const uint8_t PIN_CLOCK = 5; // GPIO5
const uint8_t PIN_READ  = 4; // GPIO4

#if defined(CONTROLLER_Z)
  // Z suffix: 4 discrete button GPIO lines, pulsed HIGH to simulate a physical press.
  const uint8_t PIN_LIGHT = 15; // GPIO15 - also read passively, see checkPhysicalButtons()
  const uint8_t PIN_UP    = 14; // GPIO14
  const uint8_t PIN_DOWN  = 12; // GPIO12
  const uint8_t PIN_PUMP  = 13; // GPIO13
#elif defined(CONTROLLER_SZ)
  #error "SZ/D controller button-write not implemented yet - see README"
#else
  #error "No controller variant selected: build with -e z or -e sz (see platformio.ini)"
#endif

// How long to hold a simulated button-press pulse.
const unsigned long BUTTON_PULSE_MS = 100;

// RGB status LED (WS2812B, addressable) - shows WiFi+MQTT connectivity (see updateStatusLed()).
const uint8_t STATUS_LED_PIN = 2; // GPIO2 - also LED_BUILTIN, now dedicated to this LED instead
const uint8_t NUM_STATUS_LEDS = 1;

// OTA
const int OTA_PORT = 8266;
const char* OTA_PASSWORD = SECRET_OTA_PASSWORD;

// How often to read + publish Balboa status.
const unsigned long STATE_PUBLISH_INTERVAL_MS = 5000;

// A Temp Up/Down press makes the panel blink to show the SET temp instead of water temp - a
// blank frame within this many ms counts as still blinking (see BlinkState::isBlinking()).
const unsigned long BLINK_TIMEOUT_MS = 2000;

// How long a settled water-temp reading must hold before the boot reveal press fires (see loop()).
const unsigned long SET_TEMP_BOOT_SETTLE_MS = 3000;

// Number entity target range - the SPA's hard limits.
const int SET_TEMP_MIN = 26;
const int SET_TEMP_MAX = 40;

// Spacing between simulated presses while walking to a number entity's target - must exceed one
// full blink confirm cycle (~700-1000ms observed) or the walk overshoots/oscillates around the
// target.
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

// MQTT command payloads (writeTopic) - named once, shared by messageReceived()'s dispatch, the
// button entities' payload_press, and physicalButtonHandler()'s labels, so a typo can't silently
// desync a publisher from its handler.
const char* CMD_TEMP_UP = "TempUp";
const char* CMD_TEMP_DOWN = "TempDown";
const char* CMD_LIGHT_ON = "ON";
const char* CMD_LIGHT_OFF = "OFF";
const char* CMD_LIGHTS_TOGGLE = "Lights"; // legacy/manual blind toggle
const char* CMD_PUMP1 = "Pump1";
const char* CMD_STOP = "Stop";
const char* CMD_RESET = "Reset";

//////////////////////////////////////////////////////////////////////////////
// RUNTIME STATE - derived values and objects, not meant to be edited directly.
//////////////////////////////////////////////////////////////////////////////

const String cdName = deviceName + "_" + clientName; // bubbelkopp_spa

String aTopic, dTopic, sTopic, attrTopic, cTopic, writeTopic, updateTempTopic, testTopic, statusLedCmdTopic;

WiFiClient wclient;
MQTTClient client(MQTT_BUFFER_SIZE);

time_t now;
String tsBoottime;
unsigned long lastPublish = 0;

BalboaInterface Balboa(PIN_CLOCK, PIN_READ, PIN_LIGHT);

// Blink episode tracking - owns "is the panel currently showing the set temp", used by temp
// confirmation, the boot reveal, and the number-entity walk. Direct field access is intentionally
// avoided outside this struct's own methods (see reset(), used instead of poking lastBlankFrameMs
// directly) so every reset goes through one obvious place.
struct BlinkState {
  unsigned long lastBlankFrameMs = 0;
  unsigned long blinkStartMs = 0;
  bool firstBlankPending = false;

  bool isBlinking() const {
    return lastBlankFrameMs != 0 && (millis() - lastBlankFrameMs < BLINK_TIMEOUT_MS);
  }

  // Arms the blink window proactively the instant we press, rather than waiting to see a blank
  // frame prove it. Only advances blinkStartMs on a genuinely new episode.
  void markStart() {
    if (!isBlinking()) {
      blinkStartMs = millis();
      firstBlankPending = true;
    }
    lastBlankFrameMs = millis();
  }

  // Fully closes out the current episode, so the next markStart() begins a clean, brand-new one.
  void reset() {
    lastBlankFrameMs = 0;
  }
};
BlinkState blink;

// Water vs set temp can only be told apart in hindsight (a frame followed by a blank was the
// set temp) - tracked here rather than via the library's forward-only setTempActive.
struct TempTracking {
  int currentWaterTemp = -1;
  int currentSetTemp = -1;
  int waterTempBeforeLastWrite = -1; // for rolling back a water-temp write that turns out to be set-temp
  String previousFrame = "";
  unsigned long previousFrameMs = 0;
};
TempTracking temp;

// Boot set-temp reveal: fires exactly once, the moment the water temp looks settled.
struct BootReveal {
  int waterTempAtLastSettleCheck = -1;
  unsigned long settledSinceMs = 0;
  bool triggered = false;
};
BootReveal bootReveal;

// Number entity walk-to-target state (see loop()/messageReceived()).
struct WalkState {
  bool active = false;
  int targetValue = -1;
  unsigned long lastPressMs = 0;
  unsigned long startMs = 0;
  int lastSeenSetTemp = -999; // sentinel, distinct from the real "unknown" value of -1
  uint8_t stallCount = 0;
};
WalkState walk;

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
struct StatusDebounce {
  DebouncedBool pump1;
  DebouncedBool light;
  DebouncedBool unknownFlag;
};
StatusDebounce debounced;

// Last-published state JSON, for detecting a real change worth publishing immediately (see
// publishStateIfNeeded()) - a plain string compare, rather than a hand-maintained field-by-field
// snapshot, so the field list only has to exist in one place (publishStateIfNeeded() itself).
String lastPublishedState = "";
bool hasPublishedStateOnce = false;

struct ConnectivityState {
  unsigned long offlineSince = 0; // 0 while WiFi+MQTT are both up
  unsigned long lastWifiAttempt = 0;
  unsigned long lastMqttAttempt = 0;
};
ConnectivityState connectivity;

// Most recent physical button press label (see physicalButtonHandler()) - surfaced as a
// diagnostic sensor instead of only being visible on the raw testTopic.
String lastPhysicalButton = "";

// Raw 24-bit frame binary (see rawFrameBinary()), recomputed once per loop() and cached here so
// stateChanged()/publishState() don't each rebuild the string separately.
String currentRawFrame = "";

CRGB statusLed[NUM_STATUS_LEDS];
// Tracks the LED's last-shown state, so updateStatusLed() only calls FastLED.show() on an
// actual change - WS2812B bit-banging briefly disables interrupts, which could otherwise
// occasionally collide with the Balboa clock/data capture if done every single loop() iteration.
bool statusLedShowingUp = false;
bool statusLedInitialized = false;

//////////////////////////////////////////////////////////////////////////////

String buildTopic(String devName, String uid, String domain, String unique, String suffix) {
  return base + "/" + domain + "/" + devName + "/" + uid + unique + "/" + suffix;
}

void publishDebug(String debugString) {
  if (client.connected()) {
    client.publish(dTopic.c_str(), debugString, false, 1);
  }
}

// Shared tail for every publish*Entity() function below: serialize, build the config topic, and
// publish (retained), logging a debug message on failure. Keeps the serialize/topic/publish/
// failure-log sequence in exactly one place instead of duplicated per entity type.
void publishConfig(String domain, String entName, StaticJsonDocument<900>& ent) {
  String output;
  serializeJson(ent, output);

  cTopic = buildTopic(deviceName, cdName, domain, entName, "config");
  if (!client.publish(cTopic.c_str(), output, true, 1)) {
    publishDebug("Entity config publish FAILED for " + entName + ", length=" + String(output.length()));
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
// Builds it once and only actually publishes (and updates the change-detection snapshot) if the
// serialized result differs from what was last published, or heartbeatDue forces it regardless -
// Display/Heater/raw frame are tracked raw/undebounced on purpose (real-time diagnostics, at the
// cost of more publishes when they're noisy).
void publishStateIfNeeded(bool heartbeatDue) {
  StaticJsonDocument<384> state;
  state["_water_temp"] = temp.currentWaterTemp;
  state["_set_temp"] = temp.currentSetTemp;
  state["_heater"] = Balboa.displayHeater;
  state["_pump1"] = debounced.pump1.confirmed;
  state["_light"] = debounced.light.confirmed;
  state["_display"] = Balboa.LCD_display;
  state["_unknown_flag"] = debounced.unknownFlag.confirmed;
  state["_raw_frame"] = currentRawFrame;
  state["_last_physical_button"] = lastPhysicalButton;
  state["_led_r"] = statusLed[0].r;
  state["_led_g"] = statusLed[0].g;
  state["_led_b"] = statusLed[0].b;

  String output;
  serializeJson(state, output);

  if (!heartbeatDue && hasPublishedStateOnce && output == lastPublishedState) return;

  client.publish(sTopic.c_str(), output, false, 1);
  lastPublishedState = output;
  hasPublishedStateOnce = true;
}

// entName also doubles as the state-blob JSON key. customExpr overrides the default
// value_json.<entName> lookup; stateTopicOverride reads from attrTopic instead of sTopic.
void publishEntity(StaticJsonDocument<400>& jObj, String domain, String entName, String friendlyName, String device_class, String state_class, String unit_of_measurement, int precision = -1, String customExpr = "", String entityCategory = "", String stateTopicOverride = "") {
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

  publishConfig(domain, entName, ent);
}

// Button entity for one of the four hard-wired GPIO functions; all share writeTopic's dispatch.
void publishButtonEntity(StaticJsonDocument<400>& jObj, String entName, String friendlyName, String payloadPress) {
  StaticJsonDocument<900> ent;

  ent["device"] = jObj;
  ent["name"] = friendlyName;
  ent["unique_id"] = cdName + entName;
  ent["command_topic"] = writeTopic;
  ent["payload_press"] = payloadPress;
  ent["availability_topic"] = aTopic;
  ent["json_attributes_topic"] = attrTopic;

  publishConfig("button", entName, ent);
}

// HA light entity - payload_on/off must match state_value_template's own ON/OFF output (MQTT
// light has no state_on/off override like switch does). messageReceived() only pulses the
// physical toggle when the commanded direction differs from the debounced real state.
void publishLightEntity(StaticJsonDocument<400>& jObj, String entName, String friendlyName) {
  StaticJsonDocument<900> ent;

  ent["device"] = jObj;
  ent["name"] = friendlyName;
  ent["unique_id"] = cdName + entName;
  ent["command_topic"] = writeTopic;
  ent["state_topic"] = sTopic;
  ent["state_value_template"] = "{{ 'ON' if value_json._light else 'OFF' }}";
  ent["payload_on"] = CMD_LIGHT_ON;
  ent["payload_off"] = CMD_LIGHT_OFF;
  ent["icon"] = "mdi:lightbulb";
  ent["availability_topic"] = aTopic;
  ent["json_attributes_topic"] = attrTopic;

  publishConfig("light", entName, ent);
}

// Number entity for the target set temp (26-40C) - command_topic reuses updateTempTopic, and
// loop()'s walk state machine turns a target into real Up/Down presses.
void publishNumberEntity(StaticJsonDocument<400>& jObj, String entName, String friendlyName) {
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

  publishConfig("number", entName, ent);
}

// Read-only status LED as a full-color light entity (see updateStatusLed()) - shows the real
// green/red as an actual color swatch in HA, not just a boolean. command_topic/rgb_command_topic
// both point at a topic we never subscribe to (HA's default light schema requires both a command
// and an rgb_command_topic for the frontend to render an actual color swatch instead of a plain
// toggle, even though this entity is never meant to be controlled) - any command sent there is
// simply dropped, and the real color reasserts itself on the next update.
void publishStatusLedEntity(StaticJsonDocument<400>& jObj, String entName, String friendlyName) {
  StaticJsonDocument<900> ent;

  ent["device"] = jObj;
  ent["name"] = friendlyName;
  ent["unique_id"] = cdName + entName;
  ent["command_topic"] = statusLedCmdTopic;
  ent["state_topic"] = sTopic;
  ent["state_value_template"] = "{{ 'ON' }}"; // LED is always lit some color, never off
  ent["supported_color_modes"][0] = "rgb";
  ent["rgb_state_topic"] = sTopic;
  ent["rgb_value_template"] = "{{ value_json._led_r }},{{ value_json._led_g }},{{ value_json._led_b }}";
  ent["rgb_command_topic"] = statusLedCmdTopic;
  ent["availability_topic"] = aTopic;
  ent["json_attributes_topic"] = attrTopic;
  ent["entity_category"] = "diagnostic";

  publishConfig("light", entName, ent);
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
  // Raw frame (see rawFrameBinary()) and the most recent physical button press label - both
  // real-time/undebounced, same rationale as Display above.
  publishEntity(dev, "sensor", "_raw_frame", "Raw frame", "", "", "", -1, "", "diagnostic");
  publishEntity(dev, "sensor", "_last_physical_button", "Last physical button", "", "", "", -1, "", "diagnostic");
  publishStatusLedEntity(dev, "_status_led", "Status LED");
  // Read-only mirror of Lights' real status, distinct unique_id from the light entity below.
  publishEntity(dev, "binary_sensor", "_light_diag", "Lights", "light", "", "", -1, "'ON' if value_json._light else 'OFF'", "diagnostic");

  publishEntity(dev, "sensor", "_uptime", "Uptime", "duration", "measurement", "s", 0, "", "diagnostic", attrTopic);
  publishEntity(dev, "sensor", "_boottime", "Boottime", "timestamp", "", "", -1, "value_json._boottime | timestamp_local", "diagnostic", attrTopic);

  publishButtonEntity(dev, "_temp_up", "Temp Up", CMD_TEMP_UP);
  publishButtonEntity(dev, "_temp_down", "Temp Down", CMD_TEMP_DOWN);
  publishButtonEntity(dev, "_pump1_btn", "Pump 1", CMD_PUMP1);

  clearOldLightEntity();
  publishLightEntity(dev, "_light", "Lights");

  client.publish(aTopic.c_str(), "online", true, 1);
  publishAttributes();
  publishDebug("MQTT connected");

  client.subscribe(writeTopic.c_str());
  client.subscribe(updateTempTopic.c_str());
}

// The library falls back to '-' for any unrecognized segment pattern - reject the whole frame
// (all bits, same 24-bit cycle) rather than propagate a glitch like "3-"/"J-"/"--".
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

// Raw frame as last captured, formatted to match README.md's chunk layout as closely as
// possible (7 bits - 7 bits - 7 bits - up to 3 bits). Diagnostic only - includes bits our own
// decode logic doesn't otherwise use (e.g. chunk 1's still-unknown bits), for protocol
// investigation.
//
// Only captures totalDataBits (23) of the documented 24-bit frame - the library has a
// pre-existing off-by-one where the true last bit is never read off the display pin at all
// (see decodeDisplayData()'s x==totalDataBits case, which reads one byte past this buffer
// instead). A real fix (tried live) changed displayBit23/_unknown_flag's behavior and broke
// the set-temp reveal logic in a way not yet understood, so it was reverted - this diagnostic
// reports what's actually captured today (chunk 4 only 2 bits) rather than the full protocol.
String rawFrameBinary() {
  String s = "";
  for (uint8_t i = 0; i < totalDataBits; i++) {
    s += Balboa.displayDataBuffer[i] ? '1' : '0';
    if (i == 6 || i == 13 || i == 20) s += '-';
  }
  return s;
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
    if (payload == CMD_TEMP_UP) { blink.markStart(); pulseButton(PIN_UP); }
    else if (payload == CMD_TEMP_DOWN) { blink.markStart(); pulseButton(PIN_DOWN); }
    // Only pulse if the debounced real state differs from what was requested (toggle-only button).
    else if (payload == CMD_LIGHT_ON) { if (!debounced.light.confirmed) pulseButton(PIN_LIGHT); }
    else if (payload == CMD_LIGHT_OFF) { if (debounced.light.confirmed) pulseButton(PIN_LIGHT); }
    else if (payload == CMD_LIGHTS_TOGGLE) { pulseButton(PIN_LIGHT); }
    else if (payload == CMD_PUMP1) { pulseButton(PIN_PUMP); }
    else if (payload == CMD_STOP) { Balboa.stop(); }
    else if (payload == CMD_RESET) { ESP.restart(); }
  } else if (topic == updateTempTopic) {
    // Number entity target - loop()'s walk state machine drives this to real Up/Down presses.
    int target = payload.toInt();
    if (target >= SET_TEMP_MIN && target <= SET_TEMP_MAX) {
      walk.targetValue = target;
      walk.active = true;
      walk.lastPressMs = 0; // fire the first press as soon as loop() next checks
      walk.startMs = millis();
      walk.lastSeenSetTemp = -999;
      walk.stallCount = 0;
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
    case BTN_ID_UP:    label = CMD_TEMP_UP;        blink.markStart(); break;
    case BTN_ID_DOWN:  label = CMD_TEMP_DOWN;      blink.markStart(); break;
    case BTN_ID_PUMP:  label = CMD_PUMP1;    break;
    case BTN_ID_LIGHT: label = CMD_LIGHTS_TOGGLE; break;
    default:           return;
  }
  publishDebug(String("Physical button pressed: ") + label);
  client.publish(testTopic.c_str(), label, false, 0);
  lastPhysicalButton = label;
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
  while (!client.connect(cdName.c_str(), MQTT_USER, MQTT_PASSWORD)) {
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
    if (nowMs - connectivity.lastWifiAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
      connectivity.lastWifiAttempt = nowMs;
      WiFi.reconnect();
    }
  } else if (!client.connected()) {
    if (nowMs - connectivity.lastMqttAttempt >= MQTT_RECONNECT_INTERVAL_MS) {
      connectivity.lastMqttAttempt = nowMs;
      if (client.connect(cdName.c_str(), MQTT_USER, MQTT_PASSWORD)) {
        announceMqttConnected();
      }
    }
  }

  bool allUp = wifiUp && client.connected();
  if (allUp) {
    connectivity.offlineSince = 0;
  } else {
    if (connectivity.offlineSince == 0) {
      connectivity.offlineSince = nowMs;
    } else if (nowMs - connectivity.offlineSince >= MAX_OFFLINE_MS) {
      ESP.restart();
    }
  }
}

// Green while WiFi+MQTT are both up, red otherwise - only touches the LED on an actual change.
void updateStatusLed() {
  bool allUp = (WiFi.status() == WL_CONNECTED) && client.connected();
  if (statusLedInitialized && allUp == statusLedShowingUp) return;

  statusLed[0] = allUp ? CRGB::Green : CRGB::Red;
  FastLED.show();
  statusLedShowingUp = allUp;
  statusLedInitialized = true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("Welcome to SPA - Balboa system GS");

  FastLED.addLeds<WS2812B, STATUS_LED_PIN, GRB>(statusLed, NUM_STATUS_LEDS);
  statusLed[0] = CRGB::Red; // offline until connect() below proves otherwise
  FastLED.show();

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
  statusLedCmdTopic = buildTopic(deviceName, cdName, domain, "", "statusled_cmd"); // never subscribed - see publishStatusLedEntity()

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
  ArduinoOTA.setPassword(OTA_PASSWORD);
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
  currentRawFrame = rawFrameBinary();

  // Corrupted frame - skip this whole cycle; previousFrame/previousFrameMs stay untouched.
  if (!isCorruptedFrame(frame)) {
    if (isBlankFrame(frame)) {
      if (!blink.isBlinking()) {
        // Blink started for an unknown reason (no markStart()) - don't trust the preceding frame.
        blink.blinkStartMs = nowFrameMs;
        blink.firstBlankPending = true;
      }
      // Confirms the preceding non-blank frame as the set temp, but only if captured after
      // blinkStartMs, and never on the episode's first blank (panel may not have redrawn yet).
      if (blink.firstBlankPending) {
        blink.firstBlankPending = false;
      } else if (temp.previousFrame.length() > 0 && !isBlankFrame(temp.previousFrame) && temp.previousFrameMs >= blink.blinkStartMs) {
        int v = temp.previousFrame.toInt();
        if (v > 0) {
          temp.currentSetTemp = v;
          // Undo an optimistic water-temp write if it turns out that frame was actually the set temp.
          if (temp.currentWaterTemp == v) {
            temp.currentWaterTemp = temp.waterTempBeforeLastWrite;
          }
        }
      }
      blink.lastBlankFrameMs = nowFrameMs;
    } else if (frame.length() > 0) {
      // Only a confirmed water-temp reading outside a blink window; written optimistically
      // otherwise (rollback above corrects a wrong guess within one cycle).
      if (!blink.isBlinking()) {
        int v = frame.toInt();
        if (v > 0) {
          temp.waterTempBeforeLastWrite = temp.currentWaterTemp;
          temp.currentWaterTemp = v;
        }
      }
    }
    temp.previousFrame = frame;
    temp.previousFrameMs = nowFrameMs;

    // Heater is deliberately excluded from debouncing - see stateChanged().
    debounced.pump1.update(Balboa.displayPump1, nowFrameMs);
    debounced.light.update(Balboa.displayLight, nowFrameMs);
    debounced.unknownFlag.update(Balboa.displayBit23, nowFrameMs);
  } // !isCorruptedFrame(frame)

  // Boot set-temp reveal: once water temp has settled, simulate a single Up press to learn it.
  if (temp.currentWaterTemp != bootReveal.waterTempAtLastSettleCheck) {
    bootReveal.waterTempAtLastSettleCheck = temp.currentWaterTemp;
    bootReveal.settledSinceMs = nowFrameMs;
  }
  if (!bootReveal.triggered && temp.currentSetTemp == -1 && temp.currentWaterTemp > 0 && !blink.isBlinking()
      && (nowFrameMs - bootReveal.settledSinceMs) >= SET_TEMP_BOOT_SETTLE_MS) {
    bootReveal.triggered = true;
    publishDebug("Boot: water temp settled at " + String(temp.currentWaterTemp) + ", triggering Up press to reveal set temp");
    blink.markStart();
    pulseButton(PIN_UP);
  }

  checkPhysicalButtons();

  client.loop(); // MQTT

  // Number entity walk-to-target: repeated single presses spaced WALK_STEP_INTERVAL_MS apart.
  // Uses a fresh timestamp, not nowFrameMs (captured before client.loop() ran) - comparing
  // against a stale nowFrameMs could underflow and cancel the walk instantly (observed live).
  unsigned long nowWalkMs = millis();
  if (walk.active) {
    if (temp.currentSetTemp == walk.targetValue) {
      walk.active = false;
    } else if ((nowWalkMs - walk.startMs) >= WALK_MAX_DURATION_MS) {
      walk.active = false;
      publishDebug("Number: walk to target " + String(walk.targetValue) + " timed out at " + String(temp.currentSetTemp));
    } else if ((nowWalkMs - walk.lastPressMs) >= WALK_STEP_INTERVAL_MS) {
      walk.lastPressMs = nowWalkMs;
      if (temp.currentSetTemp == walk.lastSeenSetTemp) {
        walk.stallCount++;
      } else {
        walk.lastSeenSetTemp = temp.currentSetTemp;
        walk.stallCount = 0;
      }
      if (walk.stallCount >= WALK_STALL_LIMIT) {
        // No progress for a while - force a clean break so the next press starts a fresh episode.
        walk.stallCount = 0;
        blink.reset();
        publishDebug("Number: walk stalled at " + String(temp.currentSetTemp) + ", forcing a fresh press episode");
      } else {
        blink.markStart();
        // Unknown set temp (-1) presses Up - a fresh press only reveals, never adjusts, so it's safe.
        pulseButton((temp.currentSetTemp == -1 || temp.currentSetTemp < walk.targetValue) ? PIN_UP : PIN_DOWN);
      }
    }
  }

  maintainConnectivity(); // non-blocking WiFi/MQTT reconnect
  updateStatusLed();

  unsigned long nowMs = millis();
  // LCD_display stays "" until the first real decode - used to avoid publishing default-false fields.
  bool hasValidRead = (frame.length() > 0);
  bool heartbeatDue = (nowMs - lastPublish > STATE_PUBLISH_INTERVAL_MS);

  // Publish immediately on a real change (sTopic isn't retained); heartbeat is a periodic backstop.
  if (client.connected() && hasValidRead) {
    publishStateIfNeeded(heartbeatDue);
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

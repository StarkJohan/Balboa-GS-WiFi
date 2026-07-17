#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <MQTT.h>
#include <ArduinoJson.h>
#include <debounce.h>                   // (lib/kimballa-button-debounce)
#include "Balboa_GS_Interface.h"        // https://github.com/MagnusPer/Balboa-GS510SZ
#include "secrets.h"                    // gitignored - copy secrets.h.example and fill in real values

/*
 *  SPA display controller for Balboa system GS. Reads the clock/data protocol shared by every
 *  Z/SZ/D-suffix panel (see BalboaInterface) and publishes water temp / heater / pump / light
 *  status to Home Assistant, merged into the same "Bubbelkopp" device as SDM-Universal-Env's
 *  bubbelkopp power-meter entities (both publish identifiers[0] = "bubbelkopp").
 *
 *  Only the Z-suffix controller's write side (4 discrete button GPIO lines, simulating a finger
 *  press by pulsing each pin HIGH) is implemented - see CONTROLLER_Z below and platformio.ini.
 *  SZ/D controllers multiplex buttons onto one shared data line instead, which is a different
 *  mechanism not implemented here; the [env:sz] build intentionally fails until that exists.
 *
 *  Known gap, not addressed here: BalboaInterface's own in-protocol write path and
 *  updateTemperature()/setTempActive are dead code (see git history / prior discussion) - they
 *  never actually pulse a pin, so the SPA/UpdateTemp auto-walk-to-target command and any
 *  "SetTemp" reading are non-functional. The direct GPIO button pulses below are the only
 *  write mechanism that actually works, and are what HA commands should go through.
*/

//////////////////////////////////////////////////////////////////////////////
// CONFIGURATION - everything you're likely to want to tweak lives below.
//////////////////////////////////////////////////////////////////////////////

// WiFi - was still "JAM" / an old long password (a stale, no-longer-valid network) before this
// pass; this board would never have connected to the current network as originally written.
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* WIFI_HOSTNAME = "BubbelkoppSpa";

// MQTT broker
const char* MQTT_BROKER_HOST = MQTT_HOST;
const char* MQTT_USER_NAME = MQTT_USER;
const char* MQTT_PASSWORD_SECRET = MQTT_PASSWORD;
const int MQTT_BUFFER_SIZE = 2048; // Max publish length in bytes, defaults to 97 - discovery
                                    // config payloads (device block + several fields/topics)
                                    // can approach 1024, so this keeps real headroom

// HA topic naming: base/domain/deviceName/cdName<unique>/suffix, same scheme as the rest of the
// fleet's buildTopic(). domain is the default/board-level domain; individual entities pass
// their own domain ("sensor"/"binary_sensor") to publishEntity() directly.
const String base = "hemma";
String domain = "sensor";
const String clientName = "spa";
const String deviceName = "bubbelkopp";

// Device metadata published in the HA discovery configs. This board shares one HA device with
// SDM-Universal-Env's "bubbelkopp" meter board (both publish identifiers[0] = "bubbelkopp"), so
// these fields must be kept identical to that project's DEVICE_MANUFACTURER/MODEL/SW_VERSION/
// HW_VERSION/CONFIG_URL - see the ivt490/IVT490-Data pairing for why (a mismatch doesn't split
// the device in HA, but the two firmwares will keep overwriting each other's device metadata).
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
  const uint8_t PIN_LIGHT = D8; // GPIO 15 - also read passively, see checkLightButton()
  const uint8_t PIN_UP    = D5; // GPIO 14
  const uint8_t PIN_DOWN  = D6; // GPIO 12
  const uint8_t PIN_PUMP  = D7; // GPIO 13
#elif defined(CONTROLLER_SZ)
  #error "SZ/D controller button-write not implemented yet - see the header comment and README"
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

// After pressing Up/Down once, the panel blinks the display slowly to show it's now displaying
// (and further Up/Down presses will adjust) the SET temperature rather than the water
// temperature. Detected as: a blank display frame was seen within this many ms - see
// isBlinking()/loop(). Long enough to bridge one blink's blank phase, short enough to revert to
// normal water-temp readings promptly once the panel stops blinking.
const unsigned long BLINK_TIMEOUT_MS = 2000;

// On boot, currentSetTemp starts at -1 (unknown) - the panel only ever reveals it via a blink,
// which we have to trigger ourselves with a single Up press (harmless: a fresh press when not
// already blinking only reveals the set temp, it doesn't adjust it - see messageReceived()/
// physicalButtonHandler()). Only do this once the water temp reading has been the same, valid,
// non-blinking value for this long, so we don't fire it off a transient boot-time garbage read.
const unsigned long SET_TEMP_BOOT_SETTLE_MS = 3000;

// Number entity target range - the SPA's hard limits (see header comment/README).
const int SET_TEMP_MIN = 26;
const int SET_TEMP_MAX = 40;

// How far apart to space each simulated Up/Down press while walking toward a number entity's
// target - the hardware only accepts one-step nudges (see messageReceived()/loop()), so reaching
// a target is a sequence of individual presses, not a single command. MUST be longer than one
// full blink confirm cycle (blank->digit->blank, observed live at ~700-1000ms) - firing faster
// than that outruns our own retroactive confirm logic, so by the time a press is evaluated
// against the target, an extra press has already gone out, causing a perpetual overshoot/
// oscillation that skips right over the target in both directions (observed live: 500ms was too
// fast and caused exactly this). Also needs to stay comfortably inside the SPA's own
// (undocumented) adjust-mode timeout - if a press lands after that real timeout has lapsed, the
// SPA treats it as a fresh reveal press instead of an adjust step, stalling the walk.
// Mostly driven by automations rather than manual slider drags, so correctness matters more
// than speed here - kept comfortably above the observed ~700-1000ms confirm-cycle ceiling rather
// than trimmed close to it.
const unsigned long WALK_STEP_INTERVAL_MS = 1200;

// If currentSetTemp hasn't moved for this many consecutive press attempts, assume a press
// landed outside the SPA's real adjust-mode window and got treated as a no-op reveal instead of
// an adjust (see WALK_STEP_INTERVAL_MS) - force a clean break so the next press starts a
// brand-new episode (goes through the full reveal semantics again) rather than continuing to
// blindly press into a dead window forever.
const uint8_t WALK_STALL_LIMIT = 3;

// Safety cap on how long a walk-to-target may run before giving up - bounds it the same way the
// rest of the fleet bounds its wait loops, in case currentSetTemp somehow never reaches the
// target (e.g. persistent decode trouble).
const unsigned long WALK_MAX_DURATION_MS = 30000;

// Line noise occasionally corrupts a single decode cycle - visibly, as a garbled digit
// ("3-"/"J-"/"--", see isCorruptedFrame()), and less visibly as a status bit flipping for one
// frame (e.g. Light). A changed boolean status reading must persist for this long before it's
// accepted, rather than trusting any single decode in isolation - see DebouncedBool/loop().
const unsigned long STATUS_DEBOUNCE_MS = 250;

// NTP - only feeds the _boottime/_uptime diagnostic attributes. Uses the ESP8266 core's own
// configTime()/time() (POSIX TZ string, handles DST) rather than NTPClient with a manual fixed
// offset - NTPClient's offset bakes local time into getEpochTime() itself, which then gets
// local-shifted a second time by HA's timestamp_local filter on _boottime, showing "in 2 hours"
// instead of the real time (same fix as IVT490-Data).
const char* NTP_SERVER = "10.10.10.1";
const char* NTP_TZ = "CET-1CEST,M3.5.0/02,M10.5.0/03";

// Connectivity: how often to retry each layer while it's down, and how long a combined
// WiFi+MQTT outage may last before giving up and rebooting the whole device.
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 10000;
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;
const unsigned long MAX_OFFLINE_MS = 5UL * 60UL * 1000UL; // 5 min

// Boot-time waits are bounded so a single unresponsive dependency can never hang startup
// forever. WiFi is required for anything else to work, so it reboots on timeout. NTP and MQTT
// are best-effort at boot - they keep being retried/refreshed non-blockingly afterward.
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

// Set the moment a blank display frame is observed (see loop()); isBlinking() stays true for
// BLINK_TIMEOUT_MS after that.
unsigned long lastBlankFrameMs = 0;

// A numeric display frame can only be confirmed as the SET temperature (rather than the water
// temperature) in hindsight, once it turns out to be immediately followed by a blank frame -
// predicting it forward from a timeout alone (as Balboa.setTempActive would, if we drove it)
// misses the first value frame of a new blink sequence (no blank has happened yet when it
// decodes) and over-extends past the last one (still "blinking" for the rest of the timeout
// window). So this is tracked here in the sketch instead of relying on the library's own
// setTempActive/setTemperature fields, which have exactly that forward-only limitation.
String previousFrame = "";
unsigned long previousFrameMs = 0; // when previousFrame was captured - see blinkStartMs
int currentWaterTemp = -1;
int currentSetTemp = -1;
// currentWaterTemp's value from just before its most recent write, so a write that turns out
// (via the next blank) to have actually been a set-temp frame can be rolled back rather than
// left standing for the whole rest of the blink sequence.
int waterTempBeforeLastWrite = -1;
// Tracks how long currentWaterTemp has held its current value, so the boot set-temp reveal
// (see SET_TEMP_BOOT_SETTLE_MS/loop()) waits for a settled reading rather than firing off
// whatever partial/garbage value happens to be showing right after power-on.
int waterTempAtLastSettleCheck = -1;
unsigned long waterTempSettledSinceMs = 0;
// Fires exactly once per boot, the moment the water temp looks settled.
bool bootSetTempTriggered = false;

// Number entity walk-to-target state (see loop()/messageReceived()).
bool walkTargetActive = false;
int walkTargetValue = -1;
unsigned long walkLastPressMs = 0;
unsigned long walkStartMs = 0;
int walkLastSeenSetTemp = -999; // sentinel, distinct from the real "unknown" value of -1
uint8_t walkStallCount = 0;
// When the current blink episode began (see markBlinkStart()). A blank frame only confirms the
// immediately-preceding frame as the set temperature if that frame was captured at/after this
// point - otherwise it could be a stale pre-press frame (still the old water temp reading) that
// merely happened to be sitting there when the first blank of a new episode arrived.
unsigned long blinkStartMs = 0;
// True from the start of a new blink episode until its first blank frame has been seen. The
// panel takes a moment to actually redraw after a press - our own isBlinking() flips true the
// instant we press (see markBlinkStart()), but the display may still show the stale pre-press
// value for that first cycle. Confirming off the first blank risks confirming that stale value
// as the set temp (observed live: a "37"/water-temp blip briefly reported as set_temp=37 before
// self-correcting to the real 36 on the next cycle) - so the first blank only arms detection,
// and confirmation waits for the second blank onward, once the panel has genuinely caught up.
bool blinkFirstBlankPending = false;

// Requires a changed boolean reading to persist for STATUS_DEBOUNCE_MS before accepting it,
// filtering out a single noise-flipped decode cycle rather than propagating it. `confirmed` is
// the debounced, trustworthy value to actually use.
struct DebouncedBool {
  bool confirmed = false;
  bool candidate = false;
  unsigned long candidateSinceMs = 0;
  bool hasCandidate = false;

  void update(bool raw, unsigned long nowMs) {
    if (raw == confirmed) {
      hasCandidate = false; // matches the accepted value, nothing to debounce
      return;
    }
    if (!hasCandidate || candidate != raw) {
      candidate = raw; // a new/different divergent reading - restart the confirmation timer
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

// Snapshot of the last-published state, used to detect a real change worth publishing
// immediately rather than waiting for the next heartbeat (see stateChanged()/publishState()).
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

// base is a fixed project-wide constant; devName/uid identify this board's HA device.
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

// Publishes one JSON blob with every read field; each entity's value_template pulls its own
// key back out, same pattern as SDM-Universal-Env/IVT490-Data. Also records the just-published
// values so stateChanged() can detect the next real change.
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

// True if any field differs from what was last actually published, so loop() can publish
// immediately on a real change instead of waiting for the next heartbeat. Always true before
// the first publish, so the very first valid read goes out immediately too.
//
// LCD_display and _heater are tracked here deliberately raw/undebounced, including their
// flicker - kept as "real time" diagnostics per request, at the cost of publishing much more
// often while blinking/flashing. Both are entity_category=diagnostic sensors precisely because
// of this. Heater specifically flashes rapidly as a natural SPA state sometimes (cause not yet
// understood - possibly related to ignition/startup) - debouncing it like the other status bits
// would hide that signal instead of letting it be observed and characterized over time.
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

// entName is also used as the JSON key in the state blob published by publishState(). Pass
// customExpr for anything that needs more than a plain value_json.<entName> lookup (e.g. the
// ON/OFF conversion for binary_sensors). device_class/state_class/unit_of_measurement/
// precision are omitted from the discovery config entirely when not applicable, rather than
// published as an empty string - HA validates several of these fields against fixed enums and
// silently rejects the entity's config if given "" instead of an omitted key. stateTopicOverride
// lets an entity read from attrTopic instead of the main state blob (see Uptime/Boottime below,
// which live in publishAttributes()'s payload, not publishState()'s).
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
  if (!client.publish(cTopic.c_str(), output, true, 1)) { // Retain config string
    publishDebug("Entity config publish FAILED for " + entName + ", length=" + String(output.length()));
  }
}

// HA button entity for one of the four hard-wired GPIO functions. All four share the same
// command_topic (writeTopic) - messageReceived() already dispatches on payload, matching how
// SPA/Write has always worked, so this only adds discovery, no new write-side logic.
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
  if (!client.publish(cTopic.c_str(), output, true, 1)) { // Retain config string
    publishDebug("Button config publish FAILED for " + entName + ", length=" + String(output.length()));
  }
}

// HA light entity. MQTT light's default schema compares state_value_template's output
// directly against payload_on/payload_off (confirmed via isolated discovery tests), so unlike
// the earlier switch attempt, payload_on/payload_off must be real "ON"/"OFF" - not both
// "Lights" - for HA to ever resolve a known state (and get the amber bulb-glow styling).
// The physical button is still just a toggle (one pulse = flip), so messageReceived() is the
// half of this that makes it correct: it only pulses PIN_LIGHT when the commanded direction
// actually differs from the real (debounced) current state (see debouncedLight), since pulsing
// while already in the requested state would incorrectly flip it the wrong way.
void publishLightEntity(StaticJsonDocument<400>& jObj, String entName, String friendlyName) {
  String output;
  StaticJsonDocument<900> ent;

  ent["device"] = jObj;
  ent["name"] = friendlyName;
  ent["unique_id"] = cdName + entName;
  ent["command_topic"] = writeTopic;
  ent["state_topic"] = sTopic;
  // MQTT light (default schema) parses state via state_value_template, not value_template
  // (unlike sensor/binary_sensor/switch).
  ent["state_value_template"] = "{{ 'ON' if value_json._light else 'OFF' }}";
  ent["payload_on"] = "ON";
  ent["payload_off"] = "OFF";
  ent["icon"] = "mdi:lightbulb";
  ent["availability_topic"] = aTopic;
  ent["json_attributes_topic"] = attrTopic;

  serializeJson(ent, output);

  cTopic = buildTopic(deviceName, cdName, "light", entName, "config");
  if (!client.publish(cTopic.c_str(), output, true, 1)) { // Retain config string
    publishDebug("Light config publish FAILED for " + entName + ", length=" + String(output.length()));
  }
}

// HA number entity for the target ("set") temperature - min/max are the SPA's hard 26-40C
// limits. command_topic reuses updateTempTopic (previously dead code, see header comment) -
// loop()'s walk state machine turns a received target into a sequence of real Up/Down presses,
// the only way the hardware actually accepts an adjustment. State reads back off the same
// _set_temp key the read-only sensor uses, so there's no separate state to keep in sync.
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
  if (!client.publish(cTopic.c_str(), output, true, 1)) { // Retain config string
    publishDebug("Number config publish FAILED for " + entName + ", length=" + String(output.length()));
  }
}

// One-time migration cleanup: removes the "switch"-domain entity (superseded by reverting back
// to the light entity above, now with the correct state_value_template key) by overwriting its
// retained config with an empty payload. Safe/idempotent to call on every connect - remove this
// once confirmed the old entity is gone from HA.
void clearOldLightEntity() {
  cTopic = buildTopic(deviceName, cdName, "switch", "_light", "config");
  client.publish(cTopic.c_str(), "", true, 1);
}

// Republishes HA discovery configs + online status and resubscribes to the command topics. A
// fresh MQTT connection has no subscriptions, so this must run after every connect.
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
  // Only meaningful while the panel is blinking (see isBlinking()) - holds the last value seen
  // during that state the rest of the time, same as a physical display would.
  publishEntity(dev, "sensor", "_set_temp", "Set temperature", "temperature", "measurement", "\xC2\xB0" "C", 0);
  publishNumberEntity(dev, "_set_temp_target", "Set temperature target");
  // Real-time raw, deliberately not debounced (see stateChanged()) - it flashes rapidly as a
  // natural SPA state sometimes, not yet understood, so it's left unfiltered.
  publishEntity(dev, "binary_sensor", "_heater", "Heater", "heat", "", "", -1, "'ON' if value_json._heater else 'OFF'");
  publishEntity(dev, "binary_sensor", "_pump1", "Pump 1", "running", "", "", -1, "'ON' if value_json._pump1 else 'OFF'");
  // Real-time raw diagnostic - intentionally not throttled/de-blinked (see stateChanged()).
  publishEntity(dev, "sensor", "_display", "Display", "", "", "", -1, "", "diagnostic");
  // bit23's actual meaning was never determined (README speculates Pump 2 / Blower) - exposed
  // as a raw diagnostic flag rather than left unpublished.
  publishEntity(dev, "binary_sensor", "_unknown_flag", "Unknown flag (bit23)", "", "", "", -1, "'ON' if value_json._unknown_flag else 'OFF'", "diagnostic");
  // Read-only diagnostic mirror of the switch's real status (see publishLightEntity()) - kept
  // separate/entity_category=diagnostic rather than the main control, distinct unique_id
  // (_light_diag) from the switch's (_light) to avoid a collision.
  publishEntity(dev, "binary_sensor", "_light_diag", "Lights", "light", "", "", -1, "'ON' if value_json._light else 'OFF'", "diagnostic");

  // Uptime/Boottime, matching the dedicated diagnostic sensors added to IVT490-Data - sourced
  // from attrTopic (publishAttributes()' payload), since that's where these two keys actually
  // live, not the main state blob.
  publishEntity(dev, "sensor", "_uptime", "Uptime", "duration", "measurement", "s", 0, "", "diagnostic", attrTopic);
  publishEntity(dev, "sensor", "_boottime", "Boottime", "timestamp", "", "", -1, "value_json._boottime | timestamp_local", "diagnostic", attrTopic);

  // Buttons for the three hard-wired GPIO functions that aren't a toggleable Light (see
  // pulseButton()/messageReceived()). Lights merged into one "light" entity below instead of a
  // separate binary_sensor + button pair, so it both shows real status and can be toggled.
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

// lookup_LCD_character() (in the library) falls back to '-' for any 7-segment pattern it
// doesn't recognize - i.e. this frame's digit(s) got corrupted by line noise mid-decode. Used
// to reject the whole frame (display text AND status bits, since they're all part of the same
// 23-bit cycle) rather than propagate a glitch like "3-"/"J-"/"--".
bool isCorruptedFrame(const String &s) {
  return s.indexOf('-') >= 0;
}

// A blank display frame is every character being a space (lookup_LCD_character() returns " "
// for an all-off segment pattern) - this is what the panel shows during the blank half of its
// blink cycle, distinct from "" (no decode has happened yet at all, see hasValidRead in loop()).
// "" itself must NOT count as blank here, or every iteration before the first real decode would
// look like a blink frame.
bool isBlankFrame(const String &s) {
  if (s.length() == 0) return false;
  for (unsigned int i = 0; i < s.length(); i++) {
    if (s[i] != ' ') return false;
  }
  return true;
}

// True while the panel is (or very recently was) blinking, i.e. showing the set temperature
// rather than the water temperature - see BLINK_TIMEOUT_MS/loop().
bool isBlinking() {
  return lastBlankFrameMs != 0 && (millis() - lastBlankFrameMs < BLINK_TIMEOUT_MS);
}

// The panel only ever starts blinking as a direct result of a Temp Up/Down press, and we're
// always the one causing or directly detecting that press (see the TempUp/TempDown handling in
// messageReceived()/physicalButtonHandler()) - so we can arm the blink window proactively right
// then, before the panel has even had a chance to show the first blinking frame, rather than
// waiting to reactively see a blank frame prove it after the fact. loop()'s rollback logic
// remains as a fallback in case the panel ever blinks for some other, unknown reason.
//
// Only advances blinkStartMs on the actual start of a new episode (not on every repeated
// press while already blinking) - it marks the cutoff before which a captured frame is
// considered stale/pre-press and must not be confirmed as the set temperature (see loop()).
void markBlinkStart() {
  if (!isBlinking()) {
    blinkStartMs = millis();
    blinkFirstBlankPending = true;
  }
  lastBlankFrameMs = millis();
}

// Simulates a physical button press by pulsing the pin HIGH, matching how a real finger press
// looks to the controller. Only meaningful for CONTROLLER_Z (see header comment).
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
    // The light entity's command_topic sends real ON/OFF (see publishLightEntity()), but the
    // physical button only pulses/toggles - only actually pulse it if the real current state
    // (from the display read) differs from what was requested, otherwise a redundant press
    // would flip it the wrong way.
    else if (payload == "ON") { if (!debouncedLight.confirmed) pulseButton(PIN_LIGHT); }
    else if (payload == "OFF") { if (debouncedLight.confirmed) pulseButton(PIN_LIGHT); }
    else if (payload == "Lights") { pulseButton(PIN_LIGHT); } // legacy/manual blind toggle
    else if (payload == "Pump1") { pulseButton(PIN_PUMP); }
    else if (payload == "Stop") { Balboa.stop(); }
    else if (payload == "Reset") { ESP.restart(); }
  } else if (topic == updateTempTopic) {
    // Number entity target - loop()'s walk state machine turns this into a sequence of real
    // Up/Down presses, the only mechanism that actually moves the setpoint (BalboaInterface's
    // own in-protocol write path is dead code, see header comment).
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

// Passive: debounced monitoring of all 4 hard-wired button lines (does not drive them),
// reporting a real physical press the same way an MQTT-triggered one is identified. Per the
// README, these lines idle LOW and pulse HIGH on press - opposite polarity from the debounce
// library's BTN_PRESSED=0/BTN_OPEN=1 convention, hence the `!digitalRead()` inversion below.
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

// One-time best-effort connect used at boot. WiFi is required for anything else to work, so
// it's worth waiting (bounded) and rebooting if it never comes up. MQTT is attempted but not
// required to proceed - if it doesn't connect in time, setup() continues (OTA still comes up)
// and maintainConnectivity() in loop() keeps retrying it non-blockingly afterward.
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

// Non-blocking reconnect used from loop(): never delays or blocks, so ArduinoOTA.handle() and
// client.loop() keep running during an outage. WiFi and MQTT are retried independently on
// their own cooldowns; if the combined outage runs too long, reboot as a last resort.
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

  // Build topics
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

  // MQTT
  client.begin(MQTT_BROKER_HOST, wclient);
  client.setWill(aTopic.c_str(), "offline", true, 1);
  client.onMessage(messageReceived);

  connect(); // WiFi + MQTT

  // NTP - runs after connect() so WiFi is actually confirmed up first, rather than racing
  // WiFi's own (still in-progress) connection attempt and wasting every bounded attempt on a
  // network that isn't up yet. Best-effort and bounded either way - this only feeds a cosmetic
  // boottime attribute (also self-corrected later in loop() as a backstop), not worth blocking
  // startup over if the NTP server doesn't answer.
  configTime(NTP_TZ, NTP_SERVER);
  for (uint8_t ntpAttempts = 0; time(&now) < 1600000000L && ntpAttempts < NTP_MAX_ATTEMPTS; ntpAttempts++) {
    delay(1000);
  }
  tsBoottime = String(now); // best-effort; self-corrects in loop() below if NTP hadn't synced yet here

  Balboa.begin();

  // Setup OTA
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

  // Retroactively confirm/attribute the frame from the *previous* iteration, now that we know
  // what followed it - see the currentWaterTemp/currentSetTemp comment above for why this can
  // only be done in hindsight, not predicted forward.
  String frame = Balboa.LCD_display;
  unsigned long nowFrameMs = millis();

  // Line noise garbled this decode cycle's digits (e.g. "3-"/"J-"/"--") - the rest of this same
  // 23-bit frame's bits (heater/pump1/light/unknown_flag) are suspect too, so the whole block
  // below skips this cycle entirely rather than propagate any of it. previousFrame/
  // previousFrameMs are deliberately left untouched, so the next good frame still gets compared
  // against the last known-good one, not this corrupted one.
  if (!isCorruptedFrame(frame)) {
    if (isBlankFrame(frame)) {
      if (!isBlinking()) {
        // First blank of an episode we didn't proactively mark via markBlinkStart() (unknown
        // cause) - don't trust whatever frame preceded it, it may predate whatever triggered
        // this. Confirming starts from the next blink cycle onward instead.
        blinkStartMs = nowFrameMs;
        blinkFirstBlankPending = true;
      }
      // This blank confirms the previous (non-blank) frame was a SET temperature reading, not
      // water temperature - but only if that frame was actually captured after this blink
      // episode began (blinkStartMs), not a stale frame left over from before the button was
      // pressed that merely happened to still be sitting there. The very first blank of the
      // episode is skipped regardless (see blinkFirstBlankPending) - the panel hasn't
      // necessarily redrawn yet by then, so previousFrame may still be the stale pre-press
      // value even though it was captured after blinkStartMs.
      if (blinkFirstBlankPending) {
        blinkFirstBlankPending = false;
      } else if (previousFrame.length() > 0 && !isBlankFrame(previousFrame) && previousFrameMs >= blinkStartMs) {
        int v = previousFrame.toInt();
        if (v > 0) {
          currentSetTemp = v;
          // The very first frame of a new blink sequence arrives before any blank has been
          // seen yet, so isBlinking() was still false when it decoded and it got optimistically
          // written to currentWaterTemp below (in the branch that handled it as the *previous*
          // frame, one loop() iteration ago) - now that this blank proves it was actually the
          // set temp, undo that if it's still standing.
          if (currentWaterTemp == v) {
            currentWaterTemp = waterTempBeforeLastWrite;
          }
        }
      }
      lastBlankFrameMs = nowFrameMs;
    } else if (frame.length() > 0) {
      // Only count this as a confirmed water-temp reading if we're not still within a blink
      // window. Still written optimistically otherwise (rather than withheld) so a genuine
      // water temp change is reflected promptly - the rollback above corrects it within one
      // decode cycle on the rare occasion this guess turns out wrong (the first frame of a new
      // blink).
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

    // Filters a single noise-flipped status bit (e.g. Light flickering) the same way the digit
    // corruption above is filtered - see DebouncedBool/STATUS_DEBOUNCE_MS. Heater is
    // deliberately excluded - see stateChanged().
    debouncedPump1.update(Balboa.displayPump1, nowFrameMs);
    debouncedLight.update(Balboa.displayLight, nowFrameMs);
    debouncedUnknownFlag.update(Balboa.displayBit23, nowFrameMs);
  } // !isCorruptedFrame(frame)

  // Boot set-temp reveal: once the water temp has held one valid value for SET_TEMP_BOOT_SETTLE_MS
  // (and we're not already blinking, e.g. from a real user press racing this), simulate a single
  // Up press. That's the only way to learn the set temp - the panel never shows it unprompted.
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

  // Number entity walk-to-target: drive currentSetTemp toward walkTargetValue via repeated
  // single Up/Down presses, spaced WALK_STEP_INTERVAL_MS apart so the panel and our own
  // retroactive confirm logic above have time to register each step before the next one fires.
  //
  // Uses a freshly-captured timestamp rather than nowFrameMs (captured at the very top of
  // loop(), before client.loop() ran) - messageReceived() sets walkStartMs to millis() *during*
  // client.loop() above, which can be later than nowFrameMs within this same tick. Comparing
  // against the stale nowFrameMs would then underflow (unsigned long, walkStartMs > nowFrameMs)
  // to a huge value, look like an instant 30s+ timeout, and cancel the walk before its first
  // press - observed live as the walk self-cancelling within milliseconds of a fresh command.
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
        // No progress for several presses in a row - a press likely landed outside the SPA's
        // real adjust-mode window and got treated as a no-op reveal instead of an adjust (see
        // WALK_STEP_INTERVAL_MS). Force our own blink tracking to fully lapse so the *next*
        // press starts a clean, brand-new episode (full reveal semantics) instead of continuing
        // to press into what might still be a dead window - skip this cycle's press to let that
        // take effect first.
        walkStallCount = 0;
        lastBlankFrameMs = 0;
        publishDebug("Number: walk stalled at " + String(currentSetTemp) + ", forcing a fresh press episode");
      } else {
        markBlinkStart();
        // currentSetTemp == -1 (not yet revealed) presses Up, same as the boot reveal - a single
        // press when not already blinking only reveals the set temp rather than changing it, so
        // this is always safe regardless of which direction turns out to actually be needed.
        pulseButton((currentSetTemp == -1 || currentSetTemp < walkTargetValue) ? PIN_UP : PIN_DOWN);
      }
    }
  }

  maintainConnectivity(); // non-blocking WiFi/MQTT reconnect

  unsigned long nowMs = millis();
  // LCD_display stays "" until decodeDisplayData() runs for the first time (it's only set
  // inside that function, and every decoded value - even blank/space segments - produces a
  // non-empty string). Used as a proxy for "Balboa has completed at least one real read cycle"
  // so we never publish the class's default-initialized (all-false) fields as if they were a
  // real "everything is off" reading, same principle as the nan-streak caching elsewhere in
  // the fleet.
  bool hasValidRead = (Balboa.LCD_display.length() > 0);
  bool heartbeatDue = (nowMs - lastPublish > STATE_PUBLISH_INTERVAL_MS);

  // Publish immediately on a real change (near-instant, since Balboa's own decode is already
  // interrupt-driven) rather than waiting for the next heartbeat - state_topic isn't retained,
  // so a change sitting unpublished for up to STATE_PUBLISH_INTERVAL_MS would otherwise leave
  // HA stale for that whole window. The heartbeat still fires independently so HA (and any late
  // subscriber) gets a periodic freshness confirmation even when nothing has changed.
  if (client.connected() && hasValidRead && (stateChanged() || heartbeatDue)) {
    publishState();
  }

  if (heartbeatDue) {
    lastPublish = nowMs;

    // tsBoottime may still be a small pre-sync placeholder if NTP hadn't answered yet by the
    // end of setup()'s bounded wait (likely, since that wait runs before connect() confirms
    // WiFi is even up) - self-correct the first time the clock looks like a real epoch value
    // (year 2020+), deriving boot time from millis() instead of trusting that early capture.
    time(&now);
    if (tsBoottime.toInt() < 1600000000L && now >= 1600000000) {
      tsBoottime = String((unsigned long)now - (nowMs / 1000));
    }

    if (client.connected()) {
      publishAttributes();
    }
  }
}

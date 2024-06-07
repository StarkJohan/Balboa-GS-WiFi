  /*
 *    
 *    Main board: Custom PCB - esp8266
 *  
 *    SPA display controller for Balboa system GS
 *    
 */
    
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ElegantOTA.h>                  // https://github.com/ayushsharma82/ElegantOTA
#include <ESP8266WiFi.h>  
#include <PubSubClient.h>               // https://github.com/knolleary/pubsubclient
#include "Balboa_GS_Interface.h"       // https://github.com/MagnusPer/Balboa-GS510SZ    

#define setClockPin D1  // GPIO 5
#define setReadPin  D2  // GPIO 4
#define setUpPin    D5  // GPIO 14
#define setDownPin  D6  // GPIO 12
#define setPumpPin  D7  // GPIO 13
#define setLightPin D8  // GPIO 15

//Constants
const char *wifi_ssid                    = "JAM";          // WiFi SSID
const char *wifi_pwd                     = "REDACTED_WIFI_PASSWORD";          // WiFi Password 
const char *wifi_hostname                = "SPA";
const char* mqtt_server                  = "mqtt.h.snutt.net";           // MQTT Boker IP, your home MQTT server eg Mosquitto on RPi, or some public MQTT
const int mqtt_port                      = 1883;        // MQTT Broker PORT, default is 1883 but can be anything.
const char *mqtt_user                    = "sensors";          // MQTT Broker User Name
const char *mqtt_pwd                     = "REDACTED_PASSWORD";          // MQTT Broker Password 
String clientId                          = "SPA : " + String(ESP.getChipId(), HEX);

//Globals 
bool debug                               = true;    // If true activate debug values to write to serial port

const unsigned long ReportTimerMillis    = 5000;   // Timer in milliseconds to report mqtt topics 
unsigned long ReportTimerPrevMillis      = 0;       // Store previous millis


// MQTT Constants
const char* mqtt_Display_topic              = "SPA/Display";
const char* mqtt_SetTemp_topic              = "SPA/SetTemp";
const char* mqtt_WaterTemp_topic            = "SPA/WaterTemp";
const char* mqtt_Heater_topic               = "SPA/Heater";
const char* mqtt_Pump1_topic                = "SPA/Pump1";
const char* mqtt_Lights_topic               = "SPA/Lights";
const char* mqtt_bit23_topic                = "SPA/bit23";
const char* mqtt_Subscribe_write_topic      = "SPA/Write"; 
const char* mqtt_Subscribe_updateTemp_topic = "SPA/UpdateTemp";

const char* mqtt_test_topic = "SPA/test";

//Initialize components
WiFiClient espClient;                                           // Setup WiFi client definition WiFi
PubSubClient client(espClient);                                 // Setup MQTT client
BalboaInterface Balboa(setClockPin, setReadPin, setLightPin);   // Setup Balboa interface 
ESP8266WebServer server(80);


////////////////////////////////////////
  static unsigned long inputTimer = 0;
  unsigned long inputInterval = 20;
  int lightState, lightInput;
  int lastLightState = 0;
  int lightCounter = 0;
  ///////////////////////////////
 
/**************************************************************************/
/* Setup                                                                  */
/**************************************************************************/

void setup() {
  
  if (debug) { Serial.begin(115200); Serial.println("Welcome to SPA - Balboa system GS");}
  setup_wifi();
  client.setCallback(callback);
  Serial.begin(115200);
  Balboa.begin();
////////////////////////////////////////

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  pinMode(setUpPin, INPUT);
  pinMode(setDownPin, INPUT);
  pinMode(setPumpPin, INPUT);

//////////////////////////////////////////
  server.on("/", []() {
    server.send(200, "text/plain", "Hi! I am ESP8266.");
  });
  
  ElegantOTA.begin(&server);    // Start ElegantOTA
  server.begin();
  if (debug) { Serial.println("HTTP server started"); }

}

void checkInputs(byte lightPin){

   lightInput = digitalRead(lightPin);

   if (lightInput != lastLightState) {
      inputTimer = millis();
   }

   if ((millis() - inputTimer) > inputInterval)
   {
      
     if(lightInput != lightState) {
          lightState = lightInput;

          if (lightState == 1) {
               digitalWrite(LED_BUILTIN, LOW);
               String message = "Button pushed: " + String(lightCounter);
               client.publish(mqtt_test_topic, message.c_str());
               lightCounter++;
          } else {
               digitalWrite(LED_BUILTIN, HIGH);
          }
     }
      
   }
   lastLightState = lightInput;
}	


/**************************************************************************/
/* Setup WiFi connection                                                  */
/**************************************************************************/

void setup_wifi() {

    /*  WiFi status return values and meaning 
        WL_IDLE_STATUS      = 0,
        WL_NO_SSID_AVAIL    = 1,
        WL_SCAN_COMPLETED   = 2,
        WL_CONNECTED        = 3,
        WL_CONNECT_FAILED   = 4,
        WL_CONNECTION_LOST  = 5,
        WL_WRONG_PASSWORD   = 6,
        WL_DISCONNECTED     = 7 */
  
    if (debug){ Serial.print("WiFi.status(): "); Serial.println(WiFi.status()); }
    
    int WiFi_retry_counter = 0;
    WiFi.mode(WIFI_STA);
    WiFi.hostname(wifi_hostname);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.begin(wifi_ssid, wifi_pwd);
    
    // Loop until reconnected or max retry then restart
    while (WiFi.status() != WL_CONNECTED){
        WiFi_retry_counter ++;
        if (WiFi_retry_counter == 30) {ESP.restart();}  
        if (debug){ Serial.print("WiFi.status(): "); Serial.print(WiFi.status()); 
                    Serial.print("   WiFi retry: "); Serial.println(WiFi_retry_counter); } 
        delay(1000);
    }
    
    if (debug){ Serial.print("WiFi connected: ");Serial.println(WiFi.localIP());}

}


/**************************************************************************/
/* Setup MQTT connection                                                   */
/**************************************************************************/

void reconnect() {

    int MQTT_retry_counter = 0;
    
    // Loop until reconnected or max retry then leave
    while (!client.connected() && MQTT_retry_counter < 30) {
       client.setServer(mqtt_server, mqtt_port);
       if (debug){ Serial.print("Connecting to MQTT server, retry: "); Serial.println(MQTT_retry_counter); }
       client.setServer(mqtt_server, mqtt_port);
       client.connect(clientId.c_str(), mqtt_user, mqtt_pwd);
       MQTT_retry_counter ++;
       delay (1000);
    }
    
    if (debug && client.connected()){ Serial.println("MQTT connected"); }

    client.subscribe(mqtt_Subscribe_write_topic);
    client.subscribe(mqtt_Subscribe_updateTemp_topic);
    client.publish(mqtt_test_topic, "MQTT Connected!");
   
    
}


/**************************************************************************/
/* Main loop                                                              */
/**************************************************************************/

void loop() {

  Balboa.loop();

  checkInputs(setLightPin);

  client.loop();
  ElegantOTA.loop();
  server.handleClient();
  
  if (WiFi.status() != WL_CONNECTED){ setup_wifi(); }             // Check WiFi connnection reconnect otherwise 
  if (!client.connected()) { reconnect(); }                       // Check MQTT connnection reconnect otherwise 


 
    if(millis() - ReportTimerPrevMillis  > ReportTimerMillis) {
    
          ReportTimerPrevMillis = millis();
          
          client.publish(mqtt_Display_topic, String(Balboa.LCD_display).c_str());
          client.publish(mqtt_SetTemp_topic, String(Balboa.setTemperature).c_str());
          client.publish(mqtt_WaterTemp_topic, String(Balboa.waterTemperature).c_str());
          client.publish(mqtt_Heater_topic, String(Balboa.displayHeater).c_str());
          client.publish(mqtt_Pump1_topic , String(Balboa.displayPump1).c_str());
          client.publish(mqtt_Lights_topic , String(Balboa.displayLight).c_str());
          client.publish(mqtt_bit23_topic , String(Balboa.displayBit23).c_str());
   
    } 
     
}

/**************************************************************************/
/* Subscribe to MQTT topic                                                */
/**************************************************************************/

void callback(char* topic, byte* payload, unsigned int length) {

      char c_payload[length];
      memcpy(c_payload, payload, length);
      c_payload[length] = '\0';
  
      String s_topic = String(topic);
      String s_payload = String(c_payload);
  
    // Handling incoming messages

    Serial.println(s_topic);
    Serial.println(s_payload);

      if ( s_topic == mqtt_Subscribe_write_topic ) {
         
             if (s_payload == "TempUp") {
                  Balboa.writeDisplayData = true; 
                  Balboa.writeTempUp      = true;
                  pinMode(setUpPin, OUTPUT);
                  digitalWrite(setUpPin, HIGH);
                  delay(100);
                  digitalWrite(setUpPin, LOW);
                  pinMode(setUpPin, INPUT);
             }
             else if (s_payload == "TempDown") {
                  Balboa.writeDisplayData = true;
                  Balboa.writeTempDown    = true; 
                  pinMode(setDownPin, OUTPUT);
                  digitalWrite(setDownPin, HIGH);
                  delay(100);
                  digitalWrite(setDownPin, LOW);
                  pinMode(setDownPin, INPUT);
             }
             else if (s_payload == "Lights") {
                  Balboa.writeDisplayData = true;
                  Balboa.writeLight       = true;
                  pinMode(setLightPin, OUTPUT);
                  digitalWrite(setLightPin, HIGH);
                  delay(100);
                  digitalWrite(setLightPin, LOW);
                  pinMode(setLightPin, INPUT);
             }
             else if (s_payload == "Pump1") {
                  Balboa.writeDisplayData = true;
                  Balboa.writePump1       = true;
                  pinMode(setPumpPin, OUTPUT);
                  digitalWrite(setPumpPin, HIGH);
                  delay(100);
                  digitalWrite(setPumpPin, LOW);
                  pinMode(setPumpPin, INPUT); 
             }
            /* else if (s_payload == "Pump2") {
                  Balboa.writeDisplayData = true;
                  Balboa.writePump2       = true;  
             }
             else if (s_payload == "Pump3") {
                  Balboa.writeDisplayData = true;
                  Balboa.writePump3       = true;  
             }
             else if (s_payload == "Mode") {
                  Balboa.writeDisplayData = true;
                  Balboa.writeMode        = true;  
             }*/
             else if (s_payload == "Stop") {
                  Balboa.stop();
             }
             else if (s_payload == "Reset") {
                ESP.restart();
             }
             
           
      }

      if ( s_topic == mqtt_Subscribe_updateTemp_topic) {
        
            Balboa.updateTemperature(s_payload.toInt());
      }

      
}
      

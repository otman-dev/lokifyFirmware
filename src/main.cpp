#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
String currentFirmwareVersion = "1.0.14";

// ==== Device info ====
const char* deviceID = "lock_01";

// ==== MQTT Settings ====
const char* mqttServer = "192.168.1.102";
const int mqttPort = 1883;
const char* mqttUser = ""; // optional
const char* mqttPassword = ""; // optional
const char* mqttTopic = "farmlab/door";

// MQTT client
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ==== Wi-Fi & MQTT flags ====
bool wifiConnected = false;
bool mqttConnected = false;
unsigned long lastMqttAttempt = 0;
const unsigned long mqttInterval = 500;

// ==== Heartbeat ====
const unsigned long heartbeatInterval = 5000; // ms
unsigned long lastHeartbeat = 0;

// ==== Relay & Pulse ====
#define RELAY1 26
const unsigned long pulseDuration = 100;
bool pulseActive = false;
unsigned long pulseStartTime = 0;

// ==== RFID ====
#define RFID_SS 32
#define RFID_RST 33
MFRC522 mfrc522(RFID_SS, RFID_RST);
String lastRFIDUID = "";
unsigned long lastCardReadTime = 0;
const unsigned long cardTimeout = 1000;
const char* allowedUIDs[] = {
  "93:9B:D7:AA", "20:15:B8:4F", "D3:C6:F6:99"
};
const int allowedUIDCount = sizeof(allowedUIDs)/sizeof(allowedUIDs[0]);
unsigned long lastRFIDCheck = 0;
const unsigned long rfidInterval = 200;

// ==== TFT ====
#define TFT_CS    5
#define TFT_RST   4
#define TFT_DC    2
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
#define BG_COLOR   ST77XX_BLACK
#define LOCK_COLOR ST77XX_GREEN
#define RFID_COLOR ST77XX_MAGENTA
#define STATUS_COLOR ST77XX_WHITE
#define HEADER_HEIGHT 20
#define RFID_UID_Y 85
#define RFID_STATUS_Y 105
#define FOOTER_Y 140

// ==== OTA ====
const char* otaJsonURL = "http://adro.ddns.net/lokifyFirmware/manifest.json";
const char* otaBaseURL = "http://adro.ddns.net/lokifyFirmware/";
unsigned long lastOTACheck = 0;
const unsigned long otaInterval = 10000;
enum OTAStatus { OTA_IDLE, OTA_CHECKING, OTA_UPDATING, OTA_ERROR };
OTAStatus otaStatus = OTA_ERROR;

// ==== Function Prototypes ====
void connectWiFiNonBlocking();
void connectMQTTNonBlocking();
void publishHeartbeat();
void publishDoorEvent(String uid, String eventType, String status);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void handleRFID();
void handleRelayPulse();
void updateRFIDStatus(String status);
void checkOTA();
void drawStatusIndicators();

// ==== Setup ====
void setup() {
  Serial.begin(115200);
  
  // TFT init
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(BG_COLOR);
  
  tft.fillRect(0, 0, 160, HEADER_HEIGHT, ST77XX_BLUE);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLUE);
  tft.setCursor(30, 5);
  tft.print("FARM LAB");
  tft.setCursor(20, 15);
  tft.print("DOOR SYSTEM");
  tft.fillRect(0, FOOTER_Y, 160, 20, ST77XX_BLUE);
  tft.setCursor(40, FOOTER_Y + 5);
  tft.print("RFID READY");

  // Relay
  pinMode(RELAY1, OUTPUT);
  digitalWrite(RELAY1, HIGH);

  // RFID
  SPI.begin();
  mfrc522.PCD_Init();

  updateRFIDStatus("Ready");
}

// ==== Loop ====
void loop() {
  unsigned long now = millis();

  // RFID check
  if (now - lastRFIDCheck >= rfidInterval) {
    lastRFIDCheck = now;
    handleRFID();
  }

  // Heartbeat
  if (now - lastHeartbeat >= heartbeatInterval) {
    lastHeartbeat = now;
    publishHeartbeat();
  }

  handleRelayPulse();
  connectWiFiNonBlocking();
  connectMQTTNonBlocking();
  checkOTA();
  drawStatusIndicators();

  yield(); // allow ESP32 background tasks
}

// ==== Functions ====

// Wi-Fi
void connectWiFiNonBlocking() {
  static unsigned long lastWiFiCheck = 0;
  static int failCount = 0;
  static const int maxFailCount = 10; // Limit connection attempts
  unsigned long now = millis();

  // Exponential backoff: 1s + failCount*2s, max 20s
  unsigned long interval = 1000 + min(failCount, 10) * 2000;
  if (now - lastWiFiCheck < interval) return;
  lastWiFiCheck = now;

  if (WiFi.status() != WL_CONNECTED) {
    if (failCount < maxFailCount) {
      WiFi.begin("Tenda_2AAA80_Lab", "87654321");
      wifiConnected = false;
      failCount++;
      tft.fillRect(0, FOOTER_Y, 160, 20, ST77XX_BLUE);
      tft.setCursor(10, FOOTER_Y+5);
      tft.print("Connecting WiFi... (" + String(failCount) + ")");
    } else {
      tft.fillRect(0, FOOTER_Y, 160, 20, ST77XX_RED);
      tft.setCursor(10, FOOTER_Y+5);
      tft.print("WiFi failed, retry later");
      // Optionally, reset failCount after a long wait or user action
    }
  } else if (!wifiConnected) {
    wifiConnected = true;
    failCount = 0;
    tft.fillRect(0, FOOTER_Y, 160, 20, ST77XX_BLUE);
    tft.setCursor(10, FOOTER_Y+5);
    tft.print("WiFi Connected!");
    otaStatus = OTA_IDLE;
  }
}

// MQTT connect & loop
void connectMQTTNonBlocking() {
  if (!wifiConnected) return;

  if (!mqttClient.connected()) {
    mqttClient.setServer(mqttServer, mqttPort);
    mqttClient.setCallback(mqttCallback);
    if (millis() - lastMqttAttempt > mqttInterval) {
      lastMqttAttempt = millis();
      if (mqttClient.connect(deviceID, mqttUser, mqttPassword)) {
        mqttConnected = true;
        mqttClient.subscribe(mqttTopic);
      } else mqttConnected = false;
    }
  } else {
    mqttClient.loop();
  }
}

// Heartbeat
void publishHeartbeat() {
  if (!mqttClient.connected()) return;

  DynamicJsonDocument doc(256);
  doc["device_id"] = deviceID;
  doc["type"] = "heartbeat";
  doc["wifi"] = wifiConnected;
  doc["mqtt"] = mqttConnected;
  switch(otaStatus) {
    case OTA_IDLE: doc["ota"] = "idle"; break;
    case OTA_CHECKING: doc["ota"] = "checking"; break;
    case OTA_UPDATING: doc["ota"] = "updating"; break;
    case OTA_ERROR: doc["ota"] = "error"; break;
  }
  doc["fw_version"] = currentFirmwareVersion; // <-- Added firmware version
  doc["timestamp"] = millis();
  doc["source"] = "local";

  char buffer[256];
  size_t n = serializeJson(doc, buffer);
  mqttClient.publish("farmlab/esp32/heartbeat", buffer, n);
}


// Door event
void publishDoorEvent(String uid, String eventType, String status) {
  if (!mqttClient.connected()) return;

  DynamicJsonDocument doc(256);
  doc["device_id"] = deviceID;
  doc["type"] = "event";
  doc["event"] = eventType;        // access_granted / access_denied
  doc["status"] = status;          // locked / unlocked
  doc["uid"] = uid;
  doc["timestamp"] = millis();
  doc["source"] = "local";

  char buffer[256];
  size_t n = serializeJson(doc, buffer);
  mqttClient.publish(mqttTopic, buffer, n);
  Serial.println("MQTT Event: " + String(buffer));
}

// MQTT callback
// MQTT callback
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  Serial.println("Message arrived: " + message);

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, message)) return;

  String cmdType = doc["type"] | "";
  String command = doc["command"] | "";
  String target = doc["device_id"] | "";

  // Process any command targeted to this device, regardless of "source" or bridge
  if (cmdType == "command" && target == deviceID) {
    if (command == "unlock") {
      digitalWrite(RELAY1, LOW);
      pulseStartTime = millis();
      pulseActive = true;
      publishDoorEvent("", "remote_unlock", "unlocked"); // events still marked "local" source
    } else if (command == "lock") {
      digitalWrite(RELAY1, HIGH);
      pulseActive = false;
      publishDoorEvent("", "remote_lock", "locked"); // events still marked "local" source
    }
  }
}


// RFID
void handleRFID() {
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  String uidString = "";
  for (byte i=0;i<mfrc522.uid.size;i++){
    if(i>0) uidString+=":";
    if(mfrc522.uid.uidByte[i]<0x10) uidString+="0";
    uidString+=String(mfrc522.uid.uidByte[i], HEX);
  }
  uidString.toUpperCase();

  unsigned long now = millis();
  if (uidString != lastRFIDUID || (now - lastCardReadTime > cardTimeout)) {
    lastRFIDUID = uidString;
    lastCardReadTime = now;

    bool allowed = false;
    for (int i=0;i<allowedUIDCount;i++) if (uidString==allowedUIDs[i]) allowed=true;

    if (allowed) {
      updateRFIDStatus("Access Granted");
      digitalWrite(RELAY1, LOW);
      pulseStartTime = millis();
      pulseActive = true;
      publishDoorEvent(uidString, "access_granted", "unlocked");
    } else {
      updateRFIDStatus("Access Denied");
      publishDoorEvent(uidString, "access_denied", "locked");
    }

    tft.fillRect(0, RFID_UID_Y, 160, 15, BG_COLOR);
    tft.setCursor(10, RFID_UID_Y);
    tft.setTextColor(RFID_COLOR, BG_COLOR);
    tft.print("RFID: " + uidString);
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

// Relay pulse
void handleRelayPulse() {
  if (pulseActive && millis() - pulseStartTime >= pulseDuration) {
    digitalWrite(RELAY1, HIGH);
    pulseActive = false;
  }
}

// TFT
void updateRFIDStatus(String status) {
  tft.fillRect(0, RFID_STATUS_Y, 160, 15, BG_COLOR);
  tft.setCursor(10, RFID_STATUS_Y);
  tft.setTextColor(STATUS_COLOR, BG_COLOR);
  tft.print("Status: " + status);
}

// OTA
void checkOTA() {
  if (!wifiConnected) return;
  if (millis() - lastOTACheck < otaInterval) return;
  lastOTACheck = millis();

  otaStatus = OTA_CHECKING;

  HTTPClient http;
  http.begin(otaJsonURL);
  int code = http.GET();
  if (code != 200) { otaStatus = OTA_ERROR; http.end(); return; }
  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, payload)) { otaStatus=OTA_ERROR; return; }

  String newVer = doc["version"];
  String fwFile = doc["file"];
  if (newVer == currentFirmwareVersion) { otaStatus = OTA_IDLE; return; }

  otaStatus = OTA_UPDATING;
  String fwURL = String(otaBaseURL) + fwFile;
  Serial.println("Starting OTA: " + fwURL);

  HTTPClient fwHttp;
  fwHttp.begin(fwURL);
  int fwCode = fwHttp.GET();
  if (fwCode != 200) { otaStatus=OTA_ERROR; fwHttp.end(); return; }

  int contentLength = fwHttp.getSize();
  WiFiClient* stream = fwHttp.getStreamPtr();
  if (!Update.begin(contentLength)) { otaStatus=OTA_ERROR; fwHttp.end(); return; }

  size_t written = Update.writeStream(*stream);
  if (written != contentLength) Serial.println("OTA written: "+String(written));

  if(Update.end()){
    if(Update.isFinished()){
      Serial.println("OTA Complete, rebooting...");
      fwHttp.end();
      otaStatus = OTA_IDLE;
      ESP.restart();
    } else otaStatus=OTA_ERROR;
  } else otaStatus=OTA_ERROR;

  fwHttp.end();
}


// TFT indicators
void drawStatusIndicators() {
  uint16_t wifiColor = wifiConnected?ST77XX_GREEN:ST77XX_RED;
  tft.fillCircle(140,10,5,wifiColor);
  uint16_t mqttColor = mqttConnected?ST77XX_GREEN:ST77XX_RED;
  tft.fillCircle(150,10,5,mqttColor);
  uint16_t otaColor = ST77XX_RED;
  if(!wifiConnected) { otaColor = ST77XX_RED; otaStatus=OTA_ERROR; }
  else {
    switch(otaStatus){
      case OTA_IDLE: otaColor=ST77XX_GREEN; break;
      case OTA_CHECKING: otaColor=ST77XX_YELLOW; break;
      case OTA_UPDATING: otaColor=ST77XX_ORANGE; break;
      case OTA_ERROR: otaColor=ST77XX_RED; break;
    }
  }
  tft.fillCircle(160,10,5,otaColor);

  tft.fillRect(0, HEADER_HEIGHT, 160, 15, BG_COLOR);
  tft.setCursor(10, HEADER_HEIGHT+15);
  tft.setTextColor(ST77XX_WHITE, BG_COLOR);
  tft.setTextSize(1); 
  tft.print("FW: " + currentFirmwareVersion);
}

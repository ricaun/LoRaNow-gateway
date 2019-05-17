
#if ESP8266
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncTCP.h>
#include <Hash.h>
#include <FS.h>
#endif
#if ESP32
#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <SPIFFS.h>
#endif

#include <ESPAsyncWiFiManager.h> 
#include <ESPAsyncWebServer.h>
#include <SPIFFSEditor.h>
#include <ArduinoOTA.h>
#include <LoRaNow.h>
#include <StreamString.h>
#include <CayenneLPPDecode.h>

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dns;

const char* http_username = "admin";
const char* http_password = "admin";

void setup() {
  // init the serial
  Serial.begin(115200);

  // init the WiFi connection
  Serial.println();
  Serial.println();
  
  Serial.print("INFO: Connecting to ");

  AsyncWiFiManager wifiManager(&server,&dns);

  wifiManager.autoConnect();
  
  Serial.println("");
  Serial.println("INFO: WiFi connected");
  Serial.println("INFO: IP address: ");
  Serial.println(WiFi.localIP());

  #ifdef ESP8266
  ArduinoOTA.setHostname("LoRaNow_esp8266");
  #endif
  #ifdef ESP32
  ArduinoOTA.setHostname("LoRaNow_esp32");
  #endif

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
      ws.closeAll();
      LoRaNow.end();
    } else { // U_SPIFFS
      type = "filesystem";
    }

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
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
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });

  ArduinoOTA.begin();


  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  #ifdef ESP8266
  SPIFFS.begin();
  server.addHandler(new SPIFFSEditor(http_username,http_password));
  #endif
  #ifdef ESP32
  SPIFFS.begin(true);
  server.addHandler(new SPIFFSEditor(SPIFFS,http_username,http_password));
  #endif

  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", String(ESP.getFreeHeap()));
  });

  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.htm");

  server.begin();

  #if defined(ARDUINO_MH_ET_LIVE_ESP32MINIKIT)
    LoRaNow.setPinsSPI(18,19,23,26,5);
  #endif

  if (LoRaNow.begin()) {
    Serial.println("LoRa init ok.");
  }

  LoRaNow.onMessage(onMessage);
  LoRaNow.gateway();

}

void loop() {
  LoRaNow.loop();
  ArduinoOTA.handle();
  if (runEvery(10000)) {
    if (WiFi.status() != WL_CONNECTED){
      WiFi.begin();
    }
    String s = "";
    s += millis();
    s += " ";
    s += LoRaNow.state;
    ws_sendAll(s);
  }
  
}


boolean runEvery(unsigned long interval)
{
  static unsigned long previousMillis = 0;
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval)
  {
    previousMillis = currentMillis;
    return true;
  }
  return false;
}

void ws_sendAll(String payload)
{
  if (ws.count() > 0)
  {
    ws.textAll(payload);
  }
}


void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("ws[%s][%u] connect\n", server->url(), client->id());
    //client->printf("LoRaNow - %u :)", client->id());
    client->printf("LoRaNow Gateway");
    client->ping();
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("ws[%s][%u] disconnect: %u\n", server->url(), client->id());
  } else if (type == WS_EVT_ERROR) {
    Serial.printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
  } else if (type == WS_EVT_PONG) {
    Serial.printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char*)data : "");
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    String msg = "";
    if (info->final && info->index == 0 && info->len == len) {
      //the whole message is in a single frame and we got all of it's data
      Serial.printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT) ? "text" : "binary", info->len);

      if (info->opcode == WS_TEXT) {
        for (size_t i = 0; i < info->len; i++) {
          msg += (char) data[i];
        }
      } else {
        char buff[3];
        for (size_t i = 0; i < info->len; i++) {
          sprintf(buff, "%02x ", (uint8_t) data[i]);
          msg += buff ;
        }
      }
      Serial.printf("%s\n", msg.c_str());

      if (len == 0) return;

      if (info->opcode == WS_TEXT)
        client->text("I got your text message");
      else
        client->binary("I got your binary message");
    } else {
      //message is comprised of multiple frames or the frame is split into multiple packets
      if (info->index == 0) {
        if (info->num == 0)
          Serial.printf("ws[%s][%u] %s-message start\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
        Serial.printf("ws[%s][%u] frame[%u] start[%llu]\n", server->url(), client->id(), info->num, info->len);
      }

      Serial.printf("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT) ? "text" : "binary", info->index, info->index + len);

      if (info->opcode == WS_TEXT) {
        for (size_t i = 0; i < info->len; i++) {
          msg += (char) data[i];
        }
      } else {
        char buff[3];
        for (size_t i = 0; i < info->len; i++) {
          sprintf(buff, "%02x ", (uint8_t) data[i]);
          msg += buff ;
        }
      }
      Serial.printf("%s\n", msg.c_str());

      if ((info->index + len) == info->len) {
        Serial.printf("ws[%s][%u] frame[%u] end[%llu]\n", server->url(), client->id(), info->num, info->len);
        if (info->final) {
          Serial.printf("ws[%s][%u] %s-message end\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
          if (info->message_opcode == WS_TEXT)
            client->text("I got your text message");
          else
            client->binary("I got your binary message");
        }
      }
    }
  }
}


void onMessage(uint8_t *buffer, size_t size)
{

  
  unsigned long id = LoRaNow.id();
  byte count = LoRaNow.count();
  /*
  Serial.print("Node Id: ");
  Serial.println(id, HEX);
  Serial.print("Count: ");
  Serial.println(count);
  Serial.print("Message: ");
  Serial.write(buffer, size);
  Serial.println();
  Serial.println();
  */

  DynamicJsonDocument jsonBuffer(512);
  CayenneLPPDecode lppd;

  JsonObject root = jsonBuffer.to<JsonObject>();
  root["id"] = LoRaNow.id();
  root["count"] = LoRaNow.count();
  root["rssi"] = LoRa.packetRssi();
  root["snr"] = LoRa.packetSnr();
  
  if (size != 0) 
  {
    lppd.write(buffer, size);
    if (lppd.isValid()) {
      JsonObject payload = root.createNestedObject("payload");
      lppd.decode(payload);
      //printHex(Serial, buffer, size);
    }
    else
    {
      root["payload"] = lppd.readString();       
    }
  }
  serializeJsonPretty(root ,Serial);

  Serial.println();


  StreamString string;


  serializeJson(root, string);
  ws_sendAll(string);
  if (string.available()) {
    string.read();
  }


  LoRaNow.clear();
  LoRaNow.print("LoRaNow Gateway Message ");

  #ifdef ESP8266
  LoRaNow.print("LoRaNow_esp8266 ");
  #endif
  #ifdef ESP32
  LoRaNow.print("LoRaNow_esp32 ");
  #endif
  
  LoRaNow.print(millis());
  LoRaNow.send();

}

void printHex(Stream &p, byte * b, int c)
{
  for (int i = 0; i < c; i++)
  {
    byte d = b[i];
    if (d < 0x10) p.print('0');
    p.print(d, HEX);
  }
}

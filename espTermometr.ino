#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <time.h>
#include <esp_sleep.h>
#include <WebSocketsServer.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <FS.h>
#include <ArduinoJson.h>
//początek części pomiarowej
// GPIO where the DS18B20 is connected to
const int oneWireBus = D4;    
const int batteryMeasure = A0;
const int panelMeasure = A1; 
int connectedClients = 0;
float temp;
float pVolt;
float bVolt;
float lastbVolt[96];
float lastpVolt[96];
float lastTemp[96];
float mTime[96];
float hTime[96];
float battery;
float panel;
float mins;
float hr;
const int arraySize = 96;
const int lastIndex = 95;
// Setup a oneWire instance to communicate with any OneWire devices
OneWire oneWire(oneWireBus);

// Pass our oneWire reference to Dallas Temperature sensor 
DallasTemperature sensors(&oneWire);

//koniec czesci pomiarowek
//częsc wifi/ timer
const char* ssid = "UPC5743271";
const char* password = "wzrewP5avvhC";

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;     // Offset dla Twojej strefy czasowej (dla Polski GMT+1: 3600)
const int daylightOffset_sec = 3600; // Dla czasu letniego (1h = 3600s)
struct tm timeInfo;


AsyncWebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
volatile bool isAwokenByConnection = false;

unsigned long tim = 0;
void setup() {
  Serial.begin(115200);
  while(!LittleFS.begin(true))
  {
    Serial.println("waiting for SPIFFS");
  }
  listDir(LittleFS, "/", 3);
  if(!LittleFS.exists("/index.html")) {
    Serial.println("Plik index.html nie istnieje na SPIFFS!");
}
  //aby podczas inicjalizacji wykres ładnie wyglądał (zanim się zapełni) używam memset() by nadac tablicom wartosc 0
  memset(lastbVolt, 0, sizeof(lastbVolt));
  memset(lastpVolt, 0, sizeof(lastpVolt));
  memset(lastTemp, 0, sizeof(lastTemp));
  memset(mTime, 0, sizeof(mTime)); 
  pinMode(batteryMeasure, INPUT);
  pinMode(panelMeasure, INPUT);
  // Start the DS18B20 sensor
  sensors.begin();
  
  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED)
  {
    //Serial.println("Łączenie...");
  }

  Serial.println("Połączono, adres to:");
  Serial.println(WiFi.localIP());

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Synchronizacja z serwerem NTP...");

  // Odczytaj aktualny czas
  if (getLocalTime(&timeInfo)) {
    // Zapisanie godziny i minuty do zmiennych typu float
    hTime[lastIndex] = (float)timeInfo.tm_hour;
    mTime[lastIndex] = (float)timeInfo.tm_min;

    // Wyświetlenie wyników
    Serial.print("Godzina (float): ");
    Serial.println( hTime[lastIndex]);
    Serial.print("Minuta (float): ");
    Serial.println( mTime[lastIndex]);
  } else {
    Serial.println("Nie udało się zsynchronizować czasu.");
  }
  
// Obsługa WebSocket
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
 	 server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) { 
	   Serial.println("ESP32 Web Server: New request received:");  // for debugging 
	   Serial.println("GET /");        // for debugging 
	   request->send(LittleFS, "/index.html", "text/html"); 
	 }); 

   server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "File not found");
  });

  server.serveStatic("/", LittleFS, "/");
  server.begin();
}

void loop() {
 
    webSocket.loop();
    if((unsigned long)(millis() - tim) > 15*60*1000)
    {
    //Serial.println(esp_sleep_get_wakeup_cause());
    shiftArray(lastpVolt, arraySize);
    shiftArray(lastbVolt, arraySize);
    shiftArray(lastTemp, arraySize);
    shiftArray(mTime, arraySize);
    shiftArray(hTime, arraySize);
    takeTime(&hTime[lastIndex], &mTime[lastIndex]);
    takeMeasure(&lastTemp[lastIndex], &lastbVolt[lastIndex], &lastpVolt[lastIndex]);
    sendJsonArray("temp_update", lastTemp);
    sendJsonArray("bvolt_update", lastbVolt);
    sendJsonArray("pvolt_update", lastpVolt);
    sendJsonArray("minutes_update", mTime);
    sendJsonArray("houres_update", hTime);
    tim = millis();
  }
  if(connectedClients)
  {
    takeMeasure(&temp, &battery, &panel);
    takeTime(&hr, &mins);
    sendJson("current_hr", hr);
    sendJson("current_min", mins);
    sendJson("current_temp", temp);
    sendJson("current_battery", battery);
    sendJson("current_panel", panel);
    Serial.print("Liczba klientów: ");
    Serial.println(connectedClients);
  }
    //Serial.println(jsonString);
    //WiFi.setSleep(true);
    //esp_light_sleep_start();
}

void takeMeasure(float* temperature, float* batteryVoltage, float* panelVoltage) //funkcja jako argumenty przyjmuje wskazniki do zmiennych do których zwróci dane
{
  sensors.requestTemperatures(); 
  *temperature = sensors.getTempCByIndex(0);
  //odczyt napiecia na panelu i na baterii, nalezy obliczyc średnia n pomiarów, ostateczny wynim nalezy pomnozyc poniewarz mierzymy za pomoca dzielnika napiecia
  *batteryVoltage = 0;
  *panelVoltage = 0;
  int n = 16;
  for(int i = 0; i < n; i++)
  {
    *batteryVoltage = *batteryVoltage + analogReadMilliVolts(batteryMeasure);
    *panelVoltage = *panelVoltage + analogReadMilliVolts(panelMeasure);
  }

  //obliczenia średniej i zamiana na volty
  *batteryVoltage = 2 * (*batteryVoltage)/n/1000;
  *panelVoltage = 2 * (*panelVoltage)/n/1000;

} 
void shiftArray(float* array, int size)
{
  for(int i = 0; i < size - 1; i++)
  {
    array[i] = array[i + 1];
  }
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  // Obsługa zdarzeń WebSocket (niepotrzebne do odczytu danych)
  switch (type) {                                     // switch on the type of information sent
    case WStype_DISCONNECTED:                         // if a client is disconnected, then type == WStype_DISCONNECTED
      Serial.println("Client " + String(num) + " disconnected");
      connectedClients = 0;
      break;
    case WStype_CONNECTED:                            // if a client is connected, then type == WStype_CONNECTED
      Serial.println("Client " + String(num) + " connected");
      connectedClients = 1;
       updatesSite();
      break;
  }
}
 
void takeTime(float* hour, float* minute)
{
  getLocalTime(&timeInfo);
  *hour = (float)timeInfo.tm_hour;
  *minute = (float)timeInfo.tm_min;
}
void listDir(fs::FS &fs, const char * dirname, uint8_t levels){
    Serial.printf("Listing directory: %s\r\n", dirname);

    File root = fs.open(dirname);
    if(!root){
        Serial.println("- failed to open directory");
        return;
    }
    if(!root.isDirectory()){
        Serial.println(" - not a directory");
        return;
    }

    File file = root.openNextFile();
    while(file){
        if(file.isDirectory()){
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if(levels){
                listDir(fs, file.path(), levels -1);
            }
        } else {
            Serial.print("  FILE: ");
            Serial.print(file.name());
            Serial.print("\tSIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }
}

void sendJsonArray(String l_type, float l_array_values[]) {
    String jsonString = "";                           // create a JSON string for sending data to the client
    const size_t CAPACITY = JSON_ARRAY_SIZE(arraySize) + 100;
    StaticJsonDocument<CAPACITY> doc;                      // create JSON container
    
    JsonObject object = doc.to<JsonObject>();         // create a JSON Object
    object["type"] = l_type;                          // write data into the JSON object
    JsonArray value = object.createNestedArray("value");
    for(int i=0; i<arraySize; i++) {
      value.add(l_array_values[i]);
    }
    serializeJson(doc, jsonString);                   // convert JSON object to string
    webSocket.broadcastTXT(jsonString);               // send JSON string to all clients
    Serial.println(jsonString);
}


void sendJson(String l_type, float l_value) {
    String jsonString = "";                           // create a JSON string for sending data to the client
    StaticJsonDocument<200> doc;                      // create JSON container
    JsonObject object = doc.to<JsonObject>();         // create a JSON Object
    object["type"] = l_type;                          // write data into the JSON object
    object["value"] = l_value;
    serializeJson(doc, jsonString);                   // convert JSON object to string
    webSocket.broadcastTXT(jsonString);               // send JSON string to all clients
}
void updatesSite()
{
  sendJson("wakeup", 0);
  sendJsonArray("temp_update", lastTemp);
      sendJsonArray("bvolt_update", lastbVolt);
      sendJsonArray("pvolt_update", lastpVolt);
      sendJson("wake up", 0);
    sendJsonArray("minutes_update", mTime);
    sendJsonArray("houres_update", hTime);
  }
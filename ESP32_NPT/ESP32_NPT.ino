/*
  Rui Santos
  Complete project details at https://RandomNerdTutorials.com/esp32-date-time-ntp-client-server-arduino/
  
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files.
  
  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.
*/

#include <WiFi.h>
#include "time.h"
#include <NimBLEDevice.h>
#include <NimBLEAddress.h>
#include <LiquidCrystal.h>
#include <EEPROM.h>
#include "RTClib.h"

// initialize the library with the numbers of the interface pins
LiquidCrystal lcd(19, 23, 18, 17, 16, 15);

const char* ssid     = "Dhiren's iPhone";
const char* password = "dhirenw1";

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -21600;
const int   daylightOffset_sec = 3600;

// the number of the LED pin
const int buzzer = 21;  // 16 corresponds to GPIO16

// setting PWM properties
const int freq = 2000;
const int channel = 0;
const int resolution = 8;

TaskHandle_t NPT_Task;
TaskHandle_t Alarm_Task;
TaskHandle_t Clock_Task;
TaskHandle_t Set_Alarm_Task;

struct tm alarm_time;
struct tm current_time;
struct tm * timeinfo;
time_t current_t;

const TickType_t xDelay1s = pdMS_TO_TICKS(1000);

// Declare semaphores
static SemaphoreHandle_t time_mutex;     // Waits for parameter to be read
static SemaphoreHandle_t alarm_mutex;     // Mutex to control who get's to access alarm value

RTC_DS3231 rtc;


// See the following for generating UUIDs:
// https://www.uuidgenerator.net/

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC1_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHARACTERISTIC2_UUID "b52bd78e-48d7-11ec-81d3-0242ac130003"
// The remote service we wish to connect to.
#define serviceClientUUID "6381aa17-3dd7-4968-b543-9b75ff115b69"
// The characteristic of the remote service we are interested in.
#define startAlarmClientChar_UUID "d1bba8c7-d04b-4eb9-8ac6-d42a1be582e0"
#define stopAlarmClientChar_UUID "ae1e5e3c-1414-4fb6-846a-cfb19e2e464f"

#define EEPROM_SIZE 2

class StopCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pStopAlarmCharacteristic) {
      std::string value = pStopAlarmCharacteristic->getValue();
      if (value == "0") {
        Serial.println("Got new write");
        vTaskDelete(Alarm_Task);
        // Stop alarm if it isn't already stopped
        ledcSetup(channel, freq, resolution);
        ledcAttachPin(buzzer, channel);
        ledcWriteTone(channel, 2000);
        ledcWrite(channel, 0);
      }
    }
};

/**  None of these are required as they will be handled by the library with defaults. **
 **                       Remove as you see fit for your needs                        */  
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      BLEDevice::startAdvertising();
    };
};

static void notifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify) {
//    Serial.print("Notify callback for characteristic ");
//    Serial.print(pBLERemoteCharacteristic->getUUID().toString().c_str());
//    Serial.print(" of data length ");
//    Serial.println(length);
//    Serial.print("data: ");
//    Serial.println((char*)pData);
    Serial.println("Wearable told us to stop alarm");
    vTaskDelete(Alarm_Task);
    // Stop alarm if it isn't already stopped
    ledcSetup(channel, freq, resolution);
    ledcAttachPin(buzzer, channel);
    ledcWriteTone(channel, 2000);
    ledcWrite(channel, 0);
}

/**  None of these are required as they will be handled by the library with defaults. **
 **                       Remove as you see fit for your needs                        */  
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
  }

  void onDisconnect(BLEClient* pclient) {
    Serial.println("onDisconnect");
  }
};
// TODO: Fix how EEPROM is written to by making a higher priority task. It takes 3.3ms to write to EEPROm, so this thread probably isn't cutting it
void SetAlarm(void * pvParameters){ 
  while(1){
    Serial.println("Inside SetAlarm");
    std::string time_str = *((std::string*)pvParameters);
    xSemaphoreTake(alarm_mutex, portMAX_DELAY);
    int hr = std::atoi(time_str.substr(0,2).c_str());
    int m = std::atoi(time_str.substr(3,2).c_str());
    alarm_time.tm_hour = hr;
    alarm_time.tm_min = m;
    EEPROM.write(0, hr);
    EEPROM.write(1, m);
    EEPROM.commit();
    Serial.println(alarm_time.tm_hour);
    xSemaphoreGive(alarm_mutex);
    vTaskDelete(NULL);
  }
}

class SetCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pSetAlarmCharacteristic) {
      std::string value = pSetAlarmCharacteristic->getValue();
      if (sizeof(value) > 0) {
        Serial.print("Set alarm to: ");
        xTaskCreate(
                    SetAlarm,   /* Task function. */
                    "Set_AlarmTask",     /* name of task. */
                    7000,       /* Stack size of task */
                    (void *)&value,        /* parameter of the task */
                    3,           /* priority of the task */
                    &Set_Alarm_Task);      /* Task handle to keep track of created task */
      }
    }
};

NimBLERemoteCharacteristic *pRemoteStartCharacteristic;
NimBLERemoteCharacteristic *pRemoteStopCharacteristic;
void initBLEClient() {
    Serial.print("Forming a connection to ");
    
    BLEClient* pClient  = BLEDevice::createClient();
    Serial.println(" - Created client");

    pClient->setClientCallbacks(new MyClientCallback());

    // Connect to the remove BLE Server.
    NimBLEAddress anklet = NimBLEAddress("84:2E:14:31:BA:8D", BLE_ADDR_PUBLIC);
    pClient->connect(anklet);  // if you pass BLEAdvertisedDevice instead of address, it will be recognized type of peer device address (public or private)
    Serial.println(" - Connected to server");

    // Obtain a reference to the service we are after in the remote BLE server.
    BLERemoteService* pRemoteService = pClient->getService(serviceClientUUID);
    if (pRemoteService == nullptr) {
      Serial.print("Failed to find our service UUID: ");
//      Serial.println(serviceUUID.toString().c_str());
      pClient->disconnect();
      return;
    }
    Serial.println(" - Found our service");

    // Obtain a reference to the characteristic in the service of the remote BLE server.
    pRemoteStartCharacteristic = pRemoteService->getCharacteristic(startAlarmClientChar_UUID);
    pRemoteStopCharacteristic = pRemoteService->getCharacteristic(stopAlarmClientChar_UUID);
    if (pRemoteStopCharacteristic == nullptr) {
      Serial.print("Failed to find our characteristic UUID: ");
//      Serial.println(charUUID.toString().c_str());
      pClient->disconnect();
      return;
    }
    Serial.println(" - Found our characteristic");

//     Read the value of the characteristic.
    if(pRemoteStopCharacteristic->canRead()) {
      std::string value = pRemoteStopCharacteristic->readValue();
      Serial.print("The characteristic value was: ");
      Serial.println(value.c_str());
    }else{
      Serial.println("Can't read");
    }

    pRemoteStopCharacteristic->registerForNotify(notifyCallback);
//    return true;
}

bool initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(1000);
  }
  Serial.println(WiFi.localIP());
}

BLECharacteristic *pStopAlarmCharacteristic;

void initBLEServer(){
  BLEDevice::init("AlarmClock");
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);

  pStopAlarmCharacteristic = pService->createCharacteristic(
                                        CHARACTERISTIC1_UUID,
                                  /***** Enum Type NIMBLE_PROPERTY now *****      
                                        BLECharacteristic::PROPERTY_READ   |
                                        BLECharacteristic::PROPERTY_WRITE  
                                        );
                                  *****************************************/
                                        NIMBLE_PROPERTY::READ |
                                        NIMBLE_PROPERTY::WRITE 
                                       );

  pStopAlarmCharacteristic->setCallbacks(new StopCallbacks());

  pStopAlarmCharacteristic->setValue("0");

  BLECharacteristic *pSetAlarmCharacteristic = pService->createCharacteristic(
                                        CHARACTERISTIC2_UUID,
                                  /***** Enum Type NIMBLE_PROPERTY now *****      
                                        BLECharacteristic::PROPERTY_READ   |
                                        BLECharacteristic::PROPERTY_WRITE  
                                        );
                                  *****************************************/
                                        NIMBLE_PROPERTY::READ |
                                        NIMBLE_PROPERTY::WRITE 
                                       );

  pSetAlarmCharacteristic->setCallbacks(new SetCallbacks());

  pSetAlarmCharacteristic->setValue("07:00");

  // Start the service
  pService->start();

//  BLEAdvertising *pAdvertising = pServer->getAdvertising();
//  pAdvertising->start();
  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  BLEDevice::startAdvertising();
}

void initRtc() {
  if (! rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1) delay(10);  // stay here
  }

  // set time if power was lost
  if (rtc.lostPower()) {
    Serial.println("Setting RTC Time");
    // set based on current_time variable
    // rtc.adjust(DateTime(current_time));

    // set it to a time info struct to get the breakdown of second/minute/ etc...
    // turn that into a datetime to pass to RTC

    timeinfo = localtime( &current_t );
    rtc.adjust(DateTime(timeinfo->tm_year, timeinfo->tm_mon, timeinfo->tm_mday, timeinfo->tm_hour, timeinfo->tm_hour, timeinfo->tm_sec));
    
  }
}

void getLocalTime(void * pvParameters){
//  struct tm timeinfo;
  while(1){
    WiFi.begin(ssid, password);
    xSemaphoreTake(time_mutex, portMAX_DELAY);     // Waits for parameter to be read
    if(!getLocalTime(&current_time)){
      Serial.println("Failed to obtain time");
      xSemaphoreGive(time_mutex);
      return;
    }
    current_t = mktime(&current_time);   // Add 1s to time to account for overhead
    xSemaphoreGive(time_mutex);
    Serial.println("Updating Current Time");
    WiFi.mode( WIFI_OFF );
    vTaskDelete(NULL);
  }
}

void SoundAlarm(void * pvParameters){
  
  // configure LED PWM functionalitites
  ledcSetup(channel, freq, resolution);

  // attach the channel to the GPIO to be controlled
  ledcAttachPin(buzzer, channel);
  while(1){

    // Block until notification is received to sound alarm
      //Start making the sound
    ledcWriteTone(channel, 2000);
    ledcWrite(channel, 255);
    vTaskDelay( pdMS_TO_TICKS(100) );
    ledcWrite(channel, 0);
    vTaskDelay( pdMS_TO_TICKS(100) );
    ledcWrite(channel, 255);
    vTaskDelay( pdMS_TO_TICKS(500) );
    ledcWrite(channel, 0);
    vTaskDelay( pdMS_TO_TICKS(1000) );
    Serial.println("WAKE UP");
  }
}

void IncrementClock(void * pvParameters){
  // set up the LCD's number of columns and rows:
  lcd.begin(16, 2);
  while(1){

    // Increment current time by 1 second
    // If current time is 0:00 (midnight), poll the NPT to update current time
    // Uses semaphore for critical section when reading current time
    xSemaphoreTake(time_mutex, portMAX_DELAY);
    // current_t += 1;
    // DateTime rtc_time = rtc.now();
    // uint32_t unix_time = rtc_time.unixtime();
    // timeinfo = localtime ( &unix_time );

    // get time into a datetime
    // convert to the tm struct
    DateTime now = rtc.now();
    time_t unix_time = now.unixtime();
    timeinfo = localtime ( &unix_time );
    

    Serial.println(timeinfo, "%A, %B %d %Y %H:%M:%S");
    // Write time to LCD
    lcd.setCursor(0,0);
    lcd.print(timeinfo, "%a, %b %d %Y");
    lcd.setCursor(0,1);
    lcd.print(timeinfo, "%H:%M:%S");
    if(timeinfo->tm_hour == 0 && timeinfo->tm_min == 0 && timeinfo->tm_sec == 0){
        xSemaphoreGive(time_mutex);
        xTaskCreate(
                      getLocalTime,   /* Task function. */
                      "NPT_Task",     /* name of task. */
                      10000,       /* Stack size of task */
                      NULL,        /* parameter of the task */
                      1,           /* priority of the task */
                      &NPT_Task);      /* Task handle to keep track of created task */
    }else{
      xSemaphoreGive(time_mutex);
    }

    // check if alarm_time = current_time
    // If it does, create SoundAlarm task
    // Uses semaphore for critical section when reading alarm_time
    xSemaphoreTake(alarm_mutex, portMAX_DELAY);
    if(timeinfo->tm_hour == alarm_time.tm_hour && timeinfo->tm_min == alarm_time.tm_min && timeinfo->tm_sec == alarm_time.tm_sec){
      xSemaphoreGive(alarm_mutex);
//        pStopAlarmCharacteristic->setValue("1");
        uint8_t val = 1;
        pRemoteStartCharacteristic->writeValue(&val, sizeof(val),true);        
//        Serial.println("Reading start char:");
//        std::string v = pRemoteStartCharacteristic->readValue();
//        Serial.println(v.c_str());
        xTaskCreate(
                  SoundAlarm,   /* Task function. */
                  "Alarm_Task",     /* name of task. */
                  4000,       /* Stack size of task */
                  NULL,        /* parameter of the task */
                  1,           /* priority of the task */
                  &Alarm_Task);      /* Task handle to keep track of created task */
    }else{
      xSemaphoreGive(alarm_mutex);
    }
    vTaskDelay( xDelay1s );
  }
}

// setup for running the main program
void setup()
{      
    Serial.begin(115200);
    delay(1000);
    Serial.println("Starting...");
    

    // initialize EEPROM with predefined size
    EEPROM.begin(EEPROM_SIZE);
    alarm_time.tm_hour = EEPROM.read(0);
    alarm_time.tm_min = EEPROM.read(1);

    // Init WiFi and wait until it's started up
    initWiFi();
    delay(1000);
    Serial.println("Wifi Connected!");
    delay(5000);
    // Init and get the time
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    
    // Init semaphores
    time_mutex = xSemaphoreCreateMutex();
    alarm_mutex = xSemaphoreCreateMutex();
    
    // Create tasks
    // Get Time Task
    xTaskCreate(
                    getLocalTime,   /* Task function. */
                    "NPT_Task",     /* name of task. */
                    10000,       /* Stack size of task */
                    NULL,        /* parameter of the task */
                    1,           /* priority of the task */
                    &NPT_Task);      /* Task handle to keep track of created task */
    delay(500);
    xTaskCreate(
                    IncrementClock,   /* Task function. */
                    "Clock_Task",     /* name of task. */
                    10000,       /* Stack size of task */
                    NULL,        /* parameter of the task */
                    2,           /* priority of the task */
                    &Clock_Task);      /* Task handle to keep track of created task */

    delay(500);
    Serial.println();
    Serial.println();
    Serial.println("Waiting for wifi");
    initBLEServer();
    initBLEClient();
}

// setup for testing time functionality
void setup_rtc() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Starting...");
}

void loop(){

}

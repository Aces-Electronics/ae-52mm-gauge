#include <Arduino.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Wire.h>
#include <ui.h>
#include <WiFi.h>
#include <nvs_flash.h>
#include <Preferences.h>
#include <GeoIP.h>

#include "touch.h"
#include "passwords.h"
#include <aes/esp_aes.h>        // AES library for decrypting the Victron manufacturer data.

//#define USE_String

#define I2C_SDA_PIN 17
#define I2C_SCL_PIN 18
#define TOUCH_RST -1 // 38
#define TOUCH_IRQ -1 // 0

#define TFT_BL 38
#define BUTTON_PIN 14
#define ENCODER_CLK 13 // CLK
#define ENCODER_DT 10  // DT

/*Change to your screen resolution*/
static const uint16_t screenWidth = 480;
static const uint16_t screenHeight = 480;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * screenHeight / 5];

Preferences preferences;
GeoIP geoip; 
BLEScan *pBLEScan;

location_t loc;                               // data structure to hold results 

Arduino_ESP32RGBPanel *bus = new Arduino_ESP32RGBPanel(
    1 /* CS */, 46 /* SCK */, 0 /* SDA */,
    2 /* DE */, 42 /* VSYNC */, 3 /* HSYNC */, 45 /* PCLK */,
    11 /* R0 */, 15 /* R1 */, 12 /* R2 */, 16 /* R3 */, 21 /* R4 */,
    39 /* G0/P22 */, 7 /* G1/P23 */, 47 /* G2/P24 */, 8 /* G3/P25 */, 48 /* G4/P26 */, 9 /* G5 */,
    4 /* B0 */, 41 /* B1 */, 5 /* B2 */, 40 /* B3 */, 6 /* B4 */
);

// Uncomment for 2.1" round display
Arduino_ST7701_RGBPanel *gfx = new Arduino_ST7701_RGBPanel(
    bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */,
    false /* IPS */, 480 /* width */, 480 /* height */,
    st7701_type5_init_operations, sizeof(st7701_type5_init_operations),
    true /* BGR */,
    10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
    10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */);

bool bezel_right = false;
bool bezel_left = false;
bool connectWiFi = false;
bool disableWiFi = false;
bool checkIP = false;
bool wifiSetToOn = false;
bool settingsState = false;
bool toggleIP = true;
bool SSIDUpdated = false;
bool SSIDPasswordUpdated = false;
bool syncFlash = false;
bool validLocation = false;

int counter = 0;
int State;
int old_State;
int move_flag = 0;
int button_flag = 0;
int flesh_flag = 1;
int screen_index = 0;
int wifi_flag = 0;
int x = 0, y = 0;
int loopCounter = 0;
int geoRequestCounter = 0;
int keyBits=128;  // Number of bits for AES-CTR decrypt.
int scanTime = 1; // BLE scan time (seconds)

char savedDeviceName[32];   // cached copy of the device name (31 chars max) + \0

unsigned long lastDebounceTime = 0;  // the last time the output pin was toggled
unsigned long debounceDelay = 400;    // the debounce time; increase if the output 

#define COLOR_NUM 5
int ColorArray[COLOR_NUM] = {WHITE, BLUE, GREEN, RED, YELLOW};

// Must use the "packed" attribute to make sure the compiler doesn't add any padding to deal with
// word alignment.
typedef struct {
  uint16_t vendorID;                    // vendor ID
  uint8_t beaconType;                   // Should be 0x10 (Product Advertisement) for the packets we want
  uint8_t unknownData1[3];              // Unknown data
  uint8_t victronRecordType;            // Should be 0x01 (Solar Charger) for the packets we want
  uint16_t nonceDataCounter;            // Nonce
  uint8_t encryptKeyMatch;              // Should match pre-shared encryption key byte 0
  uint8_t victronEncryptedData[21];     // (31 bytes max per BLE spec - size of previous elements)
  uint8_t nullPad;                      // extra byte because toCharArray() adds a \0 byte.
} __attribute__((packed)) victronManufacturerData;

typedef struct {
   uint8_t deviceState;
   uint8_t outputState;
   uint8_t errorCode;
   uint16_t alarmReason;
   uint16_t warningReason;
   uint16_t inputVoltage;
   uint16_t outputVoltage;
   uint32_t offReason;
   uint8_t  unused[32];                  // Not currently used by Victron, but it could be in the future.
} __attribute__((packed)) victronPanelData;

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {


      #define manDataSizeMax 31


      // See if we have manufacturer data and then look to see if it's coming from a Victron device.
      if (advertisedDevice.haveManufacturerData() == true) {

        uint8_t manCharBuf[manDataSizeMax+1];

        #ifdef USE_String
          String manData = advertisedDevice.getManufacturerData().c_str();  ;      // lib code returns String.
        #else
          std::string manData = advertisedDevice.getManufacturerData(); // lib code returns std::string
        #endif
        int manDataSize=manData.length(); // This does not count the null at the end.

        // Copy the data from the String to a byte array. Must have the +1 so we
        // don't lose the last character to the null terminator.
        #ifdef USE_String
          manData.toCharArray((char *)manCharBuf,manDataSize+1);
        #else
          manData.copy((char *)manCharBuf, manDataSize+1);
        #endif

        // Now let's setup a pointer to a struct to get to the data more cleanly.
        victronManufacturerData * vicData=(victronManufacturerData *)manCharBuf;

        // ignore this packet if the Vendor ID isn't Victron.
        if (vicData->vendorID!=0x02e1) {
          return;
        } 

        // ignore this packet if it isn't type 0x01 (Solar Charger).
        if (vicData->victronRecordType != 0x09) {
          Serial.printf("Packet victronRecordType was 0x%x doesn't match 0x09\n",
               vicData->victronRecordType);
          return;
        }

        // Not all packets contain a device name, so if we get one we'll save it and use it from now on.
        if (advertisedDevice.haveName()) {
          // This works the same whether getName() returns String or std::string.
          strcpy(savedDeviceName,advertisedDevice.getName().c_str());
        }
        
        if (vicData->encryptKeyMatch != key[0]) {
          Serial.printf("Packet encryption key byte 0x%2.2x doesn't match configured key[0] byte 0x%2.2x\n",
              vicData->encryptKeyMatch, key[0]);
          return;
        }

        uint8_t inputData[16];
        uint8_t outputData[16]={0};  // i don't really need to initialize the output.

        // The number of encrypted bytes is given by the number of bytes in the manufacturer
        // data as a whole minus the number of bytes (10) in the header part of the data.
        int encrDataSize=manDataSize-10;
        for (int i=0; i<encrDataSize; i++) {
          inputData[i]=vicData->victronEncryptedData[i];   // copy for our decrypt below while I figure this out.
        }

        esp_aes_context ctx;
        esp_aes_init(&ctx);

        auto status = esp_aes_setkey(&ctx, key, keyBits);
        if (status != 0) {
          Serial.printf("  Error during esp_aes_setkey operation (%i).\n",status);
          esp_aes_free(&ctx);
          return;
        }
        
        // construct the 16-byte nonce counter array by piecing it together byte-by-byte.
        uint8_t data_counter_lsb=(vicData->nonceDataCounter) & 0xff;
        uint8_t data_counter_msb=((vicData->nonceDataCounter) >> 8) & 0xff;
        u_int8_t nonce_counter[16] = {data_counter_lsb, data_counter_msb, 0};
        
        u_int8_t stream_block[16] = {0};

        size_t nonce_offset=0;
        status = esp_aes_crypt_ctr(&ctx, encrDataSize, &nonce_offset, nonce_counter, stream_block, inputData, outputData);
        if (status != 0) {
          Serial.printf("Error during esp_aes_crypt_ctr operation (%i).",status);
          esp_aes_free(&ctx);
          return;
        }
        esp_aes_free(&ctx);

        // Now do our same struct magic so we can get to the data more easily.
        victronPanelData * victronData = (victronPanelData *) outputData;

        // Getting to these elements is easier using the struct instead of
        // hacking around with outputData[x] references.
        uint8_t deviceState=victronData->deviceState;
        uint8_t outputState=victronData->outputState;
        uint8_t errorCode=victronData->errorCode;
        uint16_t alarmReason=victronData->alarmReason;
        uint16_t warningReason=victronData->warningReason;
        float inputVoltage=float(victronData->inputVoltage)*0.01;
        float outputVoltage=float(victronData->outputVoltage)*0.01;
        uint32_t offReason=victronData->offReason;


        Serial.printf("%-31s, Battery: %6.2f Volts, Load: %6.2f Volts, Alarm Reason: %6d, Device State: %6d, Error Code: %6d, Warning Reason: %6d, Off Reason: %6d\n",
          savedDeviceName,
          inputVoltage, outputVoltage,
          alarmReason, deviceState,
          errorCode, warningReason,
          offReason
        );
      }
    }
};

void flashErase() {
  nvs_flash_erase(); // erase the NVS partition and...
  nvs_flash_init(); // initialize the NVS partition.
  while(true);
}

void encoder_irq()
{
    if ((millis() - lastDebounceTime) > debounceDelay) {
        State = digitalRead(ENCODER_CLK);
        if (State != old_State)
        {
            lastDebounceTime = millis(); //Reset the debouncing timer
            if (digitalRead(ENCODER_DT) == State)
            {
                bezel_right = true;
                bezel_left = false;
            }
            else
            {
                bezel_left = true;
                bezel_right = false;
            }
        }
        old_State = State; // the first position was changed
    }
}

void pin_init()
{
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    pinMode(ENCODER_CLK, INPUT_PULLUP);
    pinMode(ENCODER_DT, INPUT_PULLUP);
    old_State = digitalRead(ENCODER_CLK);

    attachInterrupt(ENCODER_CLK, encoder_irq, CHANGE);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
}

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif

    lv_disp_flush_ready(disp);
}

/*Read the touchpad*/
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    int touchX = 0, touchY = 0;

    if (read_touch(&touchX, &touchY) == 1)
    {
        data->state = LV_INDEV_STATE_PR;

        data->point.x = (uint16_t)touchX;
        data->point.y = (uint16_t)touchY;
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

void page_1()
{
    String temp = "";
    gfx->fillScreen(ColorArray[((unsigned)counter / 2 % COLOR_NUM)]);
    gfx->setTextSize(4);
    gfx->setTextColor(BLACK);
    gfx->setCursor(120, 100);
    gfx->println(F("Makerfabs"));

    gfx->setTextSize(3);
    gfx->setCursor(30, 160);
    gfx->println(F("2.1inch TFT with Touch "));

    gfx->setTextSize(4);
    gfx->setCursor(60, 200);
    temp = temp + "Encoder: " + counter;
    gfx->println(temp);

    gfx->setTextSize(3);
    gfx->setCursor(60, 240);
    if (wifi_flag == 1)
    {
        gfx->println("Wifi OK!");
    }
    else
    {
        gfx->println("Wifi Connecting..");
    }
    gfx->setTextSize(4);
    gfx->setCursor(60, 280);
    temp = "";
    temp = temp + "Touch X: " + x;
    gfx->println(temp);

    gfx->setTextSize(4);
    gfx->setCursor(60, 320);
    temp = "";
    temp = temp + "Touch Y: " + y;
    gfx->println(temp);

    // gfx->fillRect(240, 400, 30, 30, ColorArray[(((unsigned)counter / 2 + 1) % COLOR_NUM)]);

    flesh_flag = 0;
}

void getGeoLocation()
{
	Serial.println("Trying to infer GeoLocation from IP...");
  if (WiFi.status() == WL_CONNECTED) 
  {

    // Use one of the following function calls. If using API key it must be inside double quotation marks.
    
    loc = geoip.getGeoFromWiFi(false);                   // no key, results not shown on serial monitor
    //loc = geoip.getGeoFromWiFi(true);                    // no key, show results on on serial monitor
    //loc = geoip.getGeoFromWiFi("Your API Key", false);   // use API key, results not shown on serial monitor
    //loc = geoip.getGeoFromWiFi(googleApiKey, true);    // use API key, show results on on serial monitor    
    if (loc.status)                          // Check to see if the data came in from the server.
    {
      // Display information from GeoIP. The library can do this too if true is added to the function call.   
      //Serial.print("\nLatitude: ");               Serial.println(loc.latitude);      // float
      //Serial.print("Longitude: ");                Serial.println(loc.longitude);     // float
      //Serial.print("City: ");                     Serial.println(loc.city);          // char[24]
      //Serial.print("Region: ");                   Serial.println(loc.region);        // char[24]
      //Serial.print("Country: ");                  Serial.println(loc.country);       // char[24]    
      //Serial.print("Timezone: ");                 Serial.println(loc.timezone);      // char[24]
      //Serial.print("UTC Offset: ");               Serial.println(loc.offset);        // int  (eg. -1000 means -10 hours, 0 minutes)
      //Serial.print("Offset Seconds: ");           Serial.println(loc.offsetSeconds); // long    

      validLocation = true;

      lv_obj_clear_flag(ui_aeLandingBottomLabel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(ui_aeLandingBottomIcon, LV_OBJ_FLAG_HIDDEN);
      char myNewCombinedArray[48];
      strcpy(myNewCombinedArray, loc.city);
      strcat(myNewCombinedArray, ", ");
      strcat(myNewCombinedArray, loc.region);
      lv_label_set_text(ui_aeLandingBottomLabel, myNewCombinedArray);


      geoRequestCounter = 0;
    } 
    else
    {
      Serial.println("Location data received was not valid, trying again...");
      validLocation = false;
      lv_obj_add_flag(ui_aeLandingBottomLabel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui_aeLandingBottomIcon, LV_OBJ_FLAG_HIDDEN);
      if (geoRequestCounter < 6)
      {
        getGeoLocation();
        geoRequestCounter++;
      }
    }                                 
  } 
}

//---------------------------------------------------
static void updateTopStatus( String text){ // ToDo: fix this
  lv_label_set_text(ui_feedbackLabel, text.c_str());
}

void updateWiFiState()
{
  int connectionStatus = WiFi.status();
  if (!settingsState)
  {
    if (wifiSetToOn)
    {
      if(connectionStatus == WL_CONNECTED)
      {
        lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
        if (toggleIP)
        {
          lv_label_set_text(ui_feedbackLabel,"WiFi: CONNECTED");
          lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
          updateTopStatus("IP: " +  WiFi.localIP().toString());
        }
      }
      else if(connectionStatus == WL_IDLE_STATUS)
      {
        lv_label_set_text(ui_feedbackLabel,"WiFi: IDLE");
        lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      else if(connectionStatus == WL_CONNECT_FAILED)
      {
        lv_label_set_text(ui_feedbackLabel,"WiFi: ERROR");
        lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      else if(connectionStatus == WL_NO_SSID_AVAIL)
      {
        lv_label_set_text(ui_feedbackLabel,"WiFi: UNAVAILABLE");
        lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      else if(connectionStatus == WL_SCAN_COMPLETED)
      {
        lv_label_set_text(ui_feedbackLabel,"WiFi: SCANNING");
        lv_obj_clear_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      else if(connectionStatus == WL_CONNECTION_LOST)
      {
        if (!wifiSetToOn)
        {
          lv_label_set_text(ui_feedbackLabel,"WiFi: DISABLED");
        
        }
        else
        { 
          lv_label_set_text(ui_feedbackLabel,"WiFi: DISCONNECTED");
        }
        lv_obj_clear_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
        
      }
      else if(connectionStatus == WL_DISCONNECTED)
      {
        lv_label_set_text(ui_feedbackLabel,"WiFi: PASS ERROR"); //ToDo: might need to check if SSID is "none"
        lv_obj_clear_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      else
      {
        if (PWD == "none")
        {
          lv_label_set_text(ui_feedbackLabel,"WiFi: UNCONFIGURED");
          lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
        }  
        printf("WiFi State: %", connectionStatus);
      }
    }
    else
    {
      lv_img_set_src(ui_wifiIcon, &ui_img_2104900491); // WiFi off
      if (PWD == "none")
      {
        lv_label_set_text(ui_feedbackLabel,"WiFi: UNCONFIGURED");
        lv_obj_clear_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }  
      else
      {
        lv_label_set_text(ui_feedbackLabel,"WiFi: DISABLED");
        lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void disableWifi(){
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
}

void Task_TFT(void *pvParameters)
{
    while (1)
    {
        lv_timer_handler();
        vTaskDelay(10);
    }
}

void Task_main(void *pvParameters)
{
    while (1)
    {
        if ((digitalRead(BUTTON_PIN) == 0) || (bezel_left) || (bezel_right))
        {
            if (!bezel_left && !bezel_right)
            {
                bezel_right = true;
            }
            if (bezel_left)
            {
                screen_index--;
                Serial.println("Bezel rotating: left (down)");
            }
            else
            {
                screen_index++;
                Serial.println("Bezel rotating: right (up)");
            }
            
            if (screen_index > 4)
            {
                screen_index = 1;
            }
            if (screen_index < 1)
            {
                screen_index = 4;
            }
            
            if (screen_index == 1)
            {
                if (bezel_left)
                {
                    _ui_screen_change(ui_batteryScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0);
                }
                else
                {
                    _ui_screen_change(ui_batteryScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 500, 0);
                }
                Serial.println("Displaying: Battery Screen");
            }
            else if (screen_index == 2)
            {
                if (bezel_left)
                {
                    _ui_screen_change(ui_oilScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0);
                }
                else
                {
                    _ui_screen_change(ui_oilScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 500, 0);
                }
                Serial.println("Displaying: Oil Screen");
            }
            else if (screen_index == 3)
            {
                if (bezel_left)
                {
                    _ui_screen_change(ui_coolantScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0);
                }
                else
                {
                    _ui_screen_change(ui_coolantScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 500, 0);
                }
                Serial.println("Displaying: Coolant Screen");
            }
            else if (screen_index == 4)
            {
                if (bezel_left)
                {
                    _ui_screen_change(ui_turboExhaustScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0);
                }
                else
                {
                    _ui_screen_change(ui_turboExhaustScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 500, 0);
                }
                Serial.println("Displaying: EGR/Turbo Screen");
            }

            bezel_left = false;
            bezel_right = false;

            while (digitalRead(BUTTON_PIN) == 0)
            {
                vTaskDelay(100);
            }
            
        }
         if (syncFlash)
        {
          if (!wifiSetToOn)
          {
            disableWifi();
          }
          preferences.begin("ae", false);
          preferences.putBool("p_wifiOn", wifiSetToOn);
          wifiSetToOn = preferences.getBool("p_wifiOn");
          preferences.end();
          syncFlash = false;
        }

        if (connectWiFi && wifiSetToOn)
        {
          lv_img_set_src(ui_wifiIcon, &ui_img_807091229); // WiFi on
          WiFi.begin(SSID, PWD);
          connectWiFi = false;
          checkIP = true;
        }

        if (checkIP)
        {
          if (WiFi.localIP().toString() != "0.0.0.0")
          {
            checkIP = false;
            getGeoLocation();
          }
          else
          {
            lv_label_set_text(ui_feedbackLabel,"WiFi: NO IP ADDRESS");
          }
        }

        if (ARDUINO_EVENT_WIFI_STA_GOT_IP)

        if (SSIDUpdated)
        {
          preferences.begin("ae", false);
          preferences.putString("p_ssid", SSID);
          preferences.end();
          SSIDUpdated = false;
        }

        if (SSIDPasswordUpdated)
        {
          preferences.begin("ae", false);
          preferences.putString("p_pwd", PWD);
          preferences.end();
          SSIDPasswordUpdated = false;
        }

        loopCounter++;
        if (loopCounter > 50) // ~ 5 seconds
        {
          toggleIP = !toggleIP;
          updateWiFiState();
          loopCounter = 0;
        }

        vTaskDelay(100);
    }
}

void setup()
{
    //flashErase();
    Serial.begin(115200); /* prepare for possible serial debug */

    delay(1800); // sit here for a bit to give USB Serial a chance to enumerate

    Serial.printf("Using encryption key: ");
    for (int i=0; i<16; i++) {
      Serial.printf(" %2.2x",key[i]);
    }

    Serial.println();

    strcpy(savedDeviceName,"(unknown device name)");

    preferences.begin("ae", true);
    wifiSetToOn = preferences.getBool("p_wifiOn");
    SSID = preferences.getString("p_ssid", "no_p_ssid");
    PWD = preferences.getString("p_pwd", "no_p_pwd");
    preferences.end();

    if (wifiSetToOn)
    {
      connectWiFi = true;
    }

    if ((SSID != "none") && (PWD != "none"))
    {
      WiFi.mode(WIFI_STA);
      WiFi.begin(SSID, PWD);
    }    

    String LVGL_Arduino = "Hello from an AE 52mm Gauge using LVGL: ";
    LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();

    Serial.println(LVGL_Arduino);

    pin_init();

    // Init Display
    gfx->begin();

    lv_init();

    lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * screenHeight / 5);

    /*Initialize the display*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    /*Change the following line to your display resolution*/
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /*Initialize the (dummy) input device driver*/
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    ui_init();

    updateWiFiState();

    // Code from Examples->BLE->Beacon_Scanner. This sets up a timed scan watching for BLE beacons.
    // During a scan the receipt of a beacon will trigger a call to MyAdvertisedDeviceCallbacks().
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan(); //create new scan
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true); //active scan uses more power, but get results faster
    pBLEScan->setInterval(1000);
    pBLEScan->setWindow(99); // less or equal setInterval value

    Serial.println("AE 52mm Gauge: Operational!");

    xTaskCreatePinnedToCore(Task_TFT, "Task_TFT", 20480, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(Task_main, "Task_main", 40960, NULL, 3, NULL, 1);
}

void loop()
{
  BLEScanResults foundDevices = pBLEScan->start(scanTime, false);
  pBLEScan->clearResults(); // delete results fromBLEScan buffer to release memory
}
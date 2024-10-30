#include <Arduino.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <ui.h>
#include <WiFi.h>
//#include <WiFiMulti.h>
#include <nvs_flash.h>
#include <Preferences.h>
#include <GeoIP.h>

#include "touch.h"
#include "passwords.h"

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
//WiFiMulti wifiMulti;                   // create WiFiMulti object 'wifiMulti' 

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
bool wifiSetToOn = false; // ToDo: sync me from flash
bool settingsState = false;
bool toggleIP = true;
bool SSIDUpdated = false;
bool SSIDPasswordUpdated = false;
bool syncFlash = false;

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

unsigned long lastDebounceTime = 0;  // the last time the output pin was toggled
unsigned long debounceDelay = 400;    // the debounce time; increase if the output 

#define COLOR_NUM 5
int ColorArray[COLOR_NUM] = {WHITE, BLUE, GREEN, RED, YELLOW};

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
    
    //loc = geoip.getGeoFromWiFi(false);                   // no key, results not shown on serial monitor
    loc = geoip.getGeoFromWiFi(true);                    // no key, show results on on serial monitor
    //loc = geoip.getGeoFromWiFi("Your API Key", false);   // use API key, results not shown on serial monitor
    //loc = geoip.getGeoFromWiFi(googleApiKey, true);    // use API key, show results on on serial monitor    
    if (loc.status)                          // Check to see if the data came in from the server.
    {
      // Display information from GeoIP. The library can do this too if true is added to the function call.   
      Serial.print("\nLatitude: ");               Serial.println(loc.latitude);      // float
      Serial.print("Longitude: ");                Serial.println(loc.longitude);     // float
      Serial.print("City: ");                     Serial.println(loc.city);          // char[24]
      Serial.print("Region: ");                   Serial.println(loc.region);        // char[24]
      Serial.print("Country: ");                  Serial.println(loc.country);       // char[24]    
      Serial.print("Timezone: ");                 Serial.println(loc.timezone);      // char[24]
      Serial.print("UTC Offset: ");               Serial.println(loc.offset);        // int  (eg. -1000 means -10 hours, 0 minutes)
      Serial.print("Offset Seconds: ");           Serial.println(loc.offsetSeconds); // long    

      lv_label_set_text(ui_aeLandingBottomLabel,loc.city);

      geoRequestCounter = 0;
    } 
    else
    {
      Serial.println("Data received was not valid, trying again...");
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
          getGeoLocation();
        }

        if (checkIP)
        {
          if (WiFi.localIP().toString() != "0.0.0.0")
          {
            Serial.println(WiFi.localIP());
            checkIP = false;
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

    delay(5000);

    WiFi.mode(WIFI_STA);

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

    Serial.println("AE 52mm Gauge: Operational!");

    xTaskCreatePinnedToCore(Task_TFT, "Task_TFT", 20480, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(Task_main, "Task_main", 40960, NULL, 3, NULL, 1);
}

void loop()
{

}
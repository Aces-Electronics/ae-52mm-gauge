#pragma once
#define HTTP_MAX_HEADERS 30 

#include <Arduino.h>

// libs
#include <Hard-Stuff-Http.hpp>
#include <Update.h>
#include <ArduinoJson.h>
#include "ota-github-defaults.h" // Now contains custom config

#ifndef OTA_VERSION
#define OTA_VERSION "local_development"
#endif

#pragma region HelperFunctions
inline String getMacAddress()
{
    uint8_t baseMac[6];
    // Get MAC address for WiFi station
    esp_read_mac(baseMac, ESP_MAC_WIFI_STA);
    char baseMacChr[18] = {0};
    sprintf(baseMacChr, "%02X:%02X:%02X:%02X:%02X:%02X", baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
    return String(baseMacChr);
}
#pragma endregion

namespace OTA
{
#pragma region WorkingVariabls
    inline HardStuffHttpClient *http_ota;
    inline Client *underlying_client;
#pragma endregion

#pragma region UsefulStructs
    enum UpdateCondition
    {
        NO_UPDATE,     
        OLD_DIFFERENT, 
        NEW_SAME,      
        NEW_DIFFERENT  
    };

    enum InstallCondition
    {
        FAILED_TO_DOWNLOAD, 
        REDIRECT_REQUIRED,  
        SUCCESS             
    };

    struct UpdateObject
    {
        UpdateCondition condition;
        String name;
        String tag_name;
        String firmware_asset_endpoint;
        String redirect_server;

        void print(Stream *print_stream = &Serial)
        {
            const char *condition_strings[] = {
                "NO_UPDATE",
                "OLD_DIFFERENT",
                "NEW_DIFFERENT",
                "NEW_SAME"};

            print_stream->println("------------------------");
            print_stream->println("Condition: " + String(condition_strings[condition]));
            print_stream->println("tag_name: " + tag_name);
            print_stream->println("endpoint: " + String(firmware_asset_endpoint));
            print_stream->println("------------------------");
        }
    };
#pragma endregion

#pragma region SupportFunctions
    inline InstallCondition continueRedirect(UpdateObject *details, bool restart = true, std::function<void(size_t, size_t)> progress_callback = nullptr);

    inline void printFirmwareDetails(Stream *print_stream = &Serial, const char *latest_tag = nullptr)
    {
        print_stream->println("------------------------");
        print_stream->println("Device MAC: " + getMacAddress());
        print_stream->println("Firmware Version (OTA_VERSION): " + String(OTA_VERSION));
        
        if (latest_tag != nullptr)
        {
            print_stream->println("Latest Release (Server): " + String(latest_tag));
        }

        print_stream->println("------------------------");
    }

    inline void deinit()
    {
        if (http_ota != nullptr)
        {
            http_ota->stop();
            delete http_ota;
            http_ota = nullptr;
        }
    }

    inline void reinit(Client &set_underlying_client, const char *server, uint16_t port)
    {
        deinit();
        Serial.print("Server: ");
        Serial.println(server);
        underlying_client = &set_underlying_client;
        http_ota = new HardStuffHttpClient(set_underlying_client, server, port);
    }

    inline void init(Client &set_underlying_client)
    {
        printFirmwareDetails();
        reinit(set_underlying_client, OTA_SERVER, OTA_PORT);
    }

#pragma endregion

#pragma region CoreFunctions

    /**
     * @brief Check your server to see if an update is available
     * API RESPONSE EXPECTED:
     * {
     *    "version": "1.0.1",
     *    "available": true,
     *    "url": "/api/firmware/check?type=...&download=true"
     * }
     */
    inline UpdateObject isUpdateAvailable()
    {
        UpdateObject return_object;
        return_object.condition = NO_UPDATE;

        HardStuffHttpRequest request;
        request.addHeader("accept", "application/json");

        // Inject MAC address
        String checkPath = String(OTA_CHECK_PATH);
        // Only append MAC if not already in macro (it is in our new macro, but good for safety)
        if (checkPath.indexOf("mac=") == -1) {
             checkPath += "&mac=" + getMacAddress();
        }
        // Also append current_version
        checkPath += "&current_version=" + String(OTA_VERSION);

        Serial.println("GET " + String(OTA_SERVER) + checkPath);

        HardStuffHttpResponse response = http_ota->getFromHTTPServer(checkPath.c_str(), &request);

        if (response.success())
        {
            JsonDocument doc; 
            DeserializationError error = deserializeJson(doc, response.body);

            if (error) {
                Serial.print(F("deserializeJson() failed: "));
                Serial.println(error.c_str());
                Serial.println("Body: " + response.body);
                return return_object;
            }

            // Parse Custom Backend Response
            if (!doc["version"].is<String>() || !doc["available"].is<bool>())
            {
                Serial.println("Invalid API response format");
                return return_object;
            }

            bool available = doc["available"];
            String serverVersion = doc["version"].as<String>();
            String url = doc["url"].as<String>();

            return_object.tag_name = serverVersion;
            printFirmwareDetails(&Serial, serverVersion.c_str());

            if (available && url.length() > 0) {
                 return_object.condition = NEW_DIFFERENT; // Assume if available=true, it's what we want
                 return_object.firmware_asset_endpoint = url;
            } else {
                 return_object.condition = NO_UPDATE;
            }

            return return_object;
        }

        Serial.println("Failed to connect to Update Server.");
        return return_object;
    }

    /**
     * @brief Download and perform update
     */
    inline InstallCondition performUpdate(UpdateObject *details, bool follow_redirects = true, bool restart = true, std::function<void(size_t, size_t)> progress_callback = nullptr)
    {
        String path = details->firmware_asset_endpoint;
        // If path is full URL, we might need to reinit client? 
        // For now, assume relative path on same server as configured, OR handle redirect logic.
        // Our backend returns a relative path like /api/firmware/check?...

        Serial.println("Fetching update from: " + (details->redirect_server.isEmpty() ? String(OTA_SERVER) : details->redirect_server) + path);

        HardStuffHttpRequest request;
        request.addHeader("Accept", "application/octet-stream");

        HardStuffHttpResponse response = http_ota->getFromHTTPServer(path, &request, true);

        if (response.status_code == 302 || response.status_code == 301)
        {
            String URL = "";
            for (int i = 0; i < response.header_count; i++)
            {
                if (response.headers[i].key.equalsIgnoreCase("Location"))
                {
                    URL = response.headers[i].value;
                    break;
                }
            }
            if (URL.isEmpty())
            {
                Serial.println("Redirection URL extraction error...");
                return FAILED_TO_DOWNLOAD;
            }
            
            // Basic parsing for redirect
            // Assume format https://domain.com/path
            int protocolEnd = URL.indexOf("://");
            if (protocolEnd > 0) {
                int serverStart = protocolEnd + 3;
                int pathStart = URL.indexOf("/", serverStart);
                if (pathStart > 0) {
                     details->redirect_server = URL.substring(serverStart, pathStart);
                     details->firmware_asset_endpoint = URL.substring(pathStart);
                }
            } else {
                 // Relative redirect?
                 details->firmware_asset_endpoint = URL;
            }

             http_ota->stop();
             delay(250);

            if (follow_redirects)
            {
                Serial.println("Redirect required, handling internally...");
                return continueRedirect(details, restart, progress_callback);
            }
            else
            {
                return REDIRECT_REQUIRED;
            }
        }

        if (response.status_code >= 200 && response.status_code < 300)
        {
            Serial.println("Binary found. Checking validity.");
            int contentLength = 0;
            bool isValidContentType = false;

            for (int i_header = 0; i_header < response.header_count; i_header++)
            {
                if (response.headers[i_header].key.equalsIgnoreCase("Content-Length"))
                {
                    contentLength = response.headers[i_header].value.toInt();
                }
                if (response.headers[i_header].key.equalsIgnoreCase("Content-Type"))
                {
                    String contentType = response.headers[i_header].value;
                    if (contentType.startsWith("application/octet-stream") || contentType.startsWith("application/macbinary")) {
                        isValidContentType = true;
                    }
                }
            }
            
            // Allow if valid content type OR if content length looks reasonable (sometimes types differ)
            if (contentLength > 10000) { 
                 isValidContentType = true; 
            }

            if (contentLength > 0 || isValidContentType)
            {
                size_t updateSize = (contentLength > 0) ? contentLength : UPDATE_SIZE_UNKNOWN;
                Serial.printf("Size: %d bytes (UNKNOWN=%d). Beginning Update...\n", updateSize, UPDATE_SIZE_UNKNOWN);
                
                if (progress_callback) {
                    Update.onProgress(progress_callback);
                }
                if (Update.begin(updateSize))
                {
                    Update.writeStream(*http_ota);
                    if (Update.end())
                    {
                        if (Update.isFinished())
                        {
                            Serial.println("OTA done!");
                            if (restart)
                            {
                                Serial.println("Reboot...");
                                ESP.restart();
                            }
                            http_ota->stop();
                            return SUCCESS;
                        }
                    }
                }

                Serial.printf("OTA Error: %d\n", Update.getError());
            }
            else
            {
                Serial.println("Content isn't a valid binary (Size/Type check failed).");
            }
        }
        else
        {
            Serial.printf("HTTP Error: %d\n", response.status_code);
            // response.print(); 
        }
        http_ota->stop();
        return FAILED_TO_DOWNLOAD;
    }

    inline InstallCondition continueRedirect(UpdateObject *details, bool restart, std::function<void(size_t, size_t)> progress_callback)
    {
        reinit(*underlying_client, details->redirect_server.c_str(), OTA_PORT);
        return performUpdate(details, false, restart, progress_callback);
    }
#pragma endregion
}
#pragma once

// OTA server settings (Custom Backend)
#define OTA_SERVER "aenv.aceselectronics.com.au"
#define OTA_PORT 443

// Helpers to stringify macro values safely
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

// Expect these macros to be provided at build time:
//   -DOTA_DEVICE_TYPE="ae-smart-shunt"
//   -DHW_VERSION=2

#ifndef OTA_DEVICE_TYPE
#define OTA_DEVICE_TYPE "unknown"
#endif

// Construct API paths
// API Check: /api/firmware/check?type=ae-smart-shunt&hw_version=2
#define OTA_CHECK_PATH "/api/firmware/check?type=" STR(OTA_DEVICE_TYPE) "&hw_version=" STR(HW_VERSION)

#include <Arduino.h>

// Helper to construct a full asset endpoint if the API returns a relative path
inline String OTA_ASSET_ENDPOINT_CONSTRUCTOR(const String &url)
{
    // If it's already absolute (http...), return as is. 
    // If relative, prepend /
    if (url.startsWith("http")) return url;
    if (url.startsWith("/")) return url; 
    return "/" + url;
}

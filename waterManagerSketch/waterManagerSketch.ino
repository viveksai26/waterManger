#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>

#include "config.h"

// ============================================================
// VERSION
// ============================================================

#define FIRMWARE_VERSION "4.3"

// ============================================================
// mDNS
// ============================================================

#define MDNS_HOSTNAME "watermanager"

// ============================================================
// TIMING
// ============================================================

const unsigned long WIFI_TIMEOUT =
    15000UL;

const unsigned long DUCKDNS_UPDATE_INTERVAL =
    10UL * 60UL * 1000UL;

const unsigned long SENSOR_INTERVAL =
    1000UL;

const unsigned long WATER_DEBOUNCE_TIME =
    3000UL;

const unsigned long LEVEL_DEBOUNCE_TIME =
    3000UL;

const unsigned long TELEGRAM_COOLDOWN =
    30000UL;

const unsigned long TELEGRAM_POLL_INTERVAL =
    5000UL;

// ============================================================
// SENSOR FILTERING
// ============================================================
//
// Each sensor is sampled 15 times.
//
// 12 or more HIGH readings = WET
// 11 or fewer HIGH readings = DRY
//
// The result must then remain unchanged for 3 seconds before
// being accepted as a confirmed state.
//
// ============================================================

const int SENSOR_SAMPLE_COUNT =
    15;

const int SENSOR_WET_REQUIRED =
    12;

const unsigned long SENSOR_SAMPLE_DELAY_US =
    500;

// ============================================================
// WATER PRESENCE SENSOR
// ============================================================

#define WATER_DRIVE_PIN 25
#define WATER_SENSE_PIN 26

// ============================================================
// WATER LEVEL SENSORS
// ============================================================

#define LEVEL_COMMON_PIN 27

#define LEVEL1_PIN 32
#define LEVEL2_PIN 33
#define LEVEL3_PIN 16
#define LEVEL4_PIN 17

// ============================================================
// OBJECTS
// ============================================================

WebServer server(80);

Preferences preferences;

// ============================================================
// STATE
// ============================================================

bool wifiConnected = false;

bool mdnsRunning = false;

// ------------------------------------------------------------
// Water presence
// ------------------------------------------------------------

bool waterPresence = false;
bool lastRawWaterPresence = false;

unsigned long waterCandidateSince = 0;

// ------------------------------------------------------------
// Water levels
// ------------------------------------------------------------

int currentWaterLevel = 0;
int lastRawWaterLevel = 0;

unsigned long levelCandidateSince = 0;

// Confirmed individual level sensor states
bool confirmedLevelSensors[4] = {
    false,
    false,
    false,
    false
};

// Pending filtered states
bool pendingLevelSensors[4] = {
    false,
    false,
    false,
    false
};

// Time each pending level state started
unsigned long levelSensorCandidateSince[4] = {
    0,
    0,
    0,
    0
};

// ============================================================
// NETWORK STATE
// ============================================================

String currentIPv4 = "";
String currentIPv6 = "";

String duckDNSStatus =
    "Not updated";

String lastDuckDNSIPv6 =
    "";

// ============================================================
// TELEGRAM STATE
// ============================================================

String telegramStatus =
    "Not tested";

unsigned long lastTelegramSent =
    0;

unsigned long lastTelegramPoll =
    0;

long telegramUpdateOffset =
    0;

// ============================================================
// GENERAL STATE
// ============================================================

unsigned long lastDuckDNSUpdate =
    0;

unsigned long lastSensorRead =
    0;

bool otaRunning =
    false;

unsigned long bootTime =
    0;

// ============================================================
// LOGGING
// ============================================================

#define MAX_LOGS 50

String logs[MAX_LOGS];

int logCount = 0;

// ============================================================
// ADD LOG
// ============================================================

void addLog(String message) {

  String entry =
      "[" +
      String(millis() / 1000) +
      "s] " +
      message;

  if (logCount < MAX_LOGS) {

    logs[logCount++] =
        entry;

  } else {

    for (
        int i = 0;
        i < MAX_LOGS - 1;
        i++
    ) {

      logs[i] =
          logs[i + 1];
    }

    logs[MAX_LOGS - 1] =
        entry;
  }

  Serial.println(entry);
}

// ============================================================
// URL ENCODE
// ============================================================

String urlEncode(
    const String &text
) {

  String encoded = "";

  const char *hex =
      "0123456789ABCDEF";

  for (
      size_t i = 0;
      i < text.length();
      i++
  ) {

    char c =
        text.charAt(i);

    if (
        (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' ||
        c == '_' ||
        c == '.' ||
        c == '~'
    ) {

      encoded += c;

    } else {

      encoded += '%';

      encoded +=
          hex[(c >> 4) & 0x0F];

      encoded +=
          hex[c & 0x0F];
    }
  }

  return encoded;
}

// ============================================================
// AUTHENTICATION
// ============================================================

bool authenticate() {

  if (
      !server.authenticate(
          WEB_USERNAME,
          WEB_PASSWORD
      )
  ) {

    server.requestAuthentication();

    return false;
  }

  return true;
}

// ============================================================
// mDNS
// ============================================================

void stopMDNS() {

  if (mdnsRunning) {

    MDNS.end();

    mdnsRunning = false;

    addLog(
        "mDNS stopped"
    );
  }
}

// ============================================================

void startMDNS() {

  stopMDNS();

  if (
      MDNS.begin(
          MDNS_HOSTNAME
      )
  ) {

    mdnsRunning = true;

    MDNS.addService(
        "http",
        "tcp",
        80
    );

    addLog(
        "mDNS started: http://" +
        String(MDNS_HOSTNAME) +
        ".local"
    );

  } else {

    addLog(
        "mDNS failed to start"
    );
  }
}

// ============================================================
// IPV6
// ============================================================

String getIPv6Address() {

  if (
      !WiFi.STA.hasGlobalIPv6()
  ) {

    return "";
  }

  IPAddress ipv6 =
      WiFi.STA.globalIPv6();

  return ipv6.toString();
}

// ============================================================
// WIFI CONNECTION
// ============================================================

bool connectToWiFi() {

  String ssid =
      preferences.getString(
          "ssid",
          ""
      );

  String password =
      preferences.getString(
          "password",
          ""
      );

  if (
      ssid.length() == 0
  ) {

    addLog(
        "No saved Wi-Fi credentials"
    );

    return false;
  }

  addLog(
      "Connecting to Wi-Fi: " +
      ssid
  );

  WiFi.mode(
      WIFI_STA
  );

  WiFi.STA.enableIPv6(
      true
  );

  WiFi.begin(
      ssid.c_str(),
      password.c_str()
  );

  unsigned long start =
      millis();

  while (
      WiFi.status() !=
          WL_CONNECTED &&
      millis() - start <
          WIFI_TIMEOUT
  ) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  if (
      WiFi.status() !=
      WL_CONNECTED
  ) {

    wifiConnected =
        false;

    addLog(
        "Wi-Fi connection failed"
    );

    return false;
  }

  wifiConnected =
      true;

  currentIPv4 =
      WiFi.localIP().toString();

  addLog(
      "Wi-Fi connected"
  );

  addLog(
      "IPv4: " +
      currentIPv4
  );

  // ----------------------------------------------------------
  // WAIT FOR GLOBAL IPV6
  // ----------------------------------------------------------

  start =
      millis();

  while (
      !WiFi.STA.hasGlobalIPv6() &&
      millis() - start <
          5000
  ) {

    delay(250);
  }

  currentIPv6 =
      getIPv6Address();

  if (
      currentIPv6.length() > 0
  ) {

    addLog(
        "Global IPv6: " +
        currentIPv6
    );

  } else {

    addLog(
        "Global IPv6 not available"
    );
  }

  // ----------------------------------------------------------
  // mDNS
  // ----------------------------------------------------------

  startMDNS();

  return true;
}

// ============================================================
// SETUP ACCESS POINT
// ============================================================

void startSetupAP() {

  stopMDNS();

  WiFi.mode(
      WIFI_AP
  );

  WiFi.softAP(
      AP_SSID,
      AP_PASSWORD
  );

  IPAddress apIP =
      WiFi.softAPIP();

  addLog(
      "Setup AP started: " +
      String(AP_SSID)
  );

  addLog(
      "AP IP: " +
      apIP.toString()
  );

  startMDNS();

  addLog(
      "AP dashboard: http://" +
      apIP.toString()
  );

  addLog(
      "mDNS dashboard: http://" +
      String(MDNS_HOSTNAME) +
      ".local"
  );
}

// ============================================================
// DUCKDNS
// ============================================================

bool updateDuckDNS() {

  if (!wifiConnected) {

    duckDNSStatus =
        "Wi-Fi disconnected";

    return false;
  }

  String ipv6 =
      getIPv6Address();

  if (
      ipv6.length() == 0
  ) {

    duckDNSStatus =
        "No global IPv6";

    addLog(
        "DuckDNS skipped: no IPv6"
    );

    return false;
  }

  // ----------------------------------------------------------
  // IPv6 unchanged
  // ----------------------------------------------------------

  if (
      ipv6 ==
          lastDuckDNSIPv6 &&
      lastDuckDNSUpdate != 0
  ) {

    duckDNSStatus =
        "IPv6 unchanged";

    // Reset the timer even when the address is unchanged.
    // This prevents continuous retries every loop.
    lastDuckDNSUpdate =
        millis();

    return true;
  }

  addLog(
      "Updating DuckDNS: " +
      ipv6
  );

  WiFiClientSecure client;

  client.setInsecure();

  client.setTimeout(5000);

  HTTPClient http;

  String url =
      "https://www.duckdns.org/update"
      "?domains=" +
      String(DUCKDNS_DOMAIN) +
      "&token=" +
      String(DUCKDNS_TOKEN) +
      "&ipv6=" +
      ipv6 +
      "&verbose=true";

  if (
      !http.begin(
          client,
          url
      )
  ) {

    duckDNSStatus =
        "HTTPS connection failed";

    addLog(
        "DuckDNS HTTP begin failed"
    );

    client.stop();

    return false;
  }

  http.setReuse(false);

  http.setConnectTimeout(5000);

  http.setTimeout(5000);

  int httpCode =
      http.GET();

  String response =
      http.getString();

  http.end();

  client.stop();

  if (
      httpCode ==
          HTTP_CODE_OK &&
      response.indexOf("OK") >= 0
  ) {

    lastDuckDNSIPv6 =
        ipv6;

    lastDuckDNSUpdate =
        millis();

    duckDNSStatus =
        "Updated successfully";

    addLog(
        "DuckDNS updated successfully"
    );

    return true;
  }

  duckDNSStatus =
      "Update failed: HTTP " +
      String(httpCode);

  addLog(
      "DuckDNS update failed: " +
      String(httpCode) +
      " " +
      response
  );

  return false;
}

// ============================================================
// TELEGRAM GENERIC REQUEST
// ============================================================

bool telegramRequest(
    const String &url,
    String &response
) {

  if (!wifiConnected) {

    telegramStatus =
        "Wi-Fi disconnected";

    return false;
  }

  // ----------------------------------------------------------
  // Try twice.
  //
  // A negative HTTPClient result means that the ESP32 failed
  // at the connection level before receiving an HTTP response.
  // ----------------------------------------------------------

  for (
      int attempt = 1;
      attempt <= 2;
      attempt++
  ) {

    WiFiClientSecure client;

    client.setInsecure();

    client.setTimeout(5000);

    HTTPClient http;

    // --------------------------------------------------------
    // HTTPS connection
    // --------------------------------------------------------

    if (
        !http.begin(
            client,
            url
        )
    ) {

      telegramStatus =
          "HTTPS connection failed";

      addLog(
          "Telegram HTTP begin failed "
          "(attempt " +
          String(attempt) +
          "/2)"
      );

      http.end();

      client.stop();

      if (attempt == 1) {

        delay(250);
      }

      continue;
    }

    // --------------------------------------------------------
    // Do not reuse old Telegram HTTPS connections.
    // --------------------------------------------------------

    http.setReuse(false);

    http.setConnectTimeout(5000);

    http.setTimeout(5000);

    // --------------------------------------------------------
    // GET
    // --------------------------------------------------------

    int httpCode =
        http.GET();

    // --------------------------------------------------------
    // Successful HTTP response
    // --------------------------------------------------------

    if (httpCode > 0) {

      response =
          http.getString();

      http.end();

      client.stop();

      // ------------------------------------------------------
      // HTTP status
      // ------------------------------------------------------

      if (
          httpCode !=
          HTTP_CODE_OK
      ) {

        telegramStatus =
            "HTTP error: " +
            String(httpCode);

        addLog(
            "Telegram HTTP error: " +
            String(httpCode)
        );

        return false;
      }

      // ------------------------------------------------------
      // Telegram API status
      // ------------------------------------------------------

      if (
          response.indexOf(
              "\"ok\":true"
          ) < 0
      ) {

        telegramStatus =
            "Telegram API error";

        addLog(
            "Telegram API returned error"
        );

        return false;
      }

      telegramStatus =
          "Telegram request successful";

      return true;
    }

    // --------------------------------------------------------
    // Connection-level failure
    //
    // Example:
    // -1
    //
    // Telegram did not return an HTTP response.
    // --------------------------------------------------------

    addLog(
        "Telegram connection failed: " +
        String(httpCode) +
        " attempt " +
        String(attempt) +
        "/2"
    );

    http.end();

    client.stop();

    // --------------------------------------------------------
    // Retry once
    // --------------------------------------------------------

    if (attempt == 1) {

      delay(250);
    }
  }

  telegramStatus =
      "Telegram connection failed";

  return false;
}

// ============================================================
// TELEGRAM SEND MESSAGE
// ============================================================

bool sendTelegramMessage(
    const String &message,
    bool ignoreCooldown,
    const String &replyMarkup
) {

  if (!wifiConnected) {

    telegramStatus =
        "Wi-Fi disconnected";

    addLog(
        "Telegram skipped: Wi-Fi disconnected"
    );

    return false;
  }

  if (
      !ignoreCooldown &&
      lastTelegramSent != 0 &&
      millis() -
          lastTelegramSent <
          TELEGRAM_COOLDOWN
  ) {

    addLog(
        "Telegram skipped: cooldown"
    );

    return false;
  }

  String url =
      "https://api.telegram.org/bot" +
      String(TELEGRAM_BOT_TOKEN) +
      "/sendMessage"
      "?chat_id=" +
      urlEncode(
          String(TELEGRAM_CHAT_ID)
      ) +
      "&text=" +
      urlEncode(
          message
      );

  if (
      replyMarkup.length() > 0
  ) {

    url +=
        "&reply_markup=" +
        urlEncode(
            replyMarkup
        );
  }

  String response;

  if (
      !telegramRequest(
          url,
          response
      )
  ) {

    addLog(
        "Telegram send failed"
    );

    return false;
  }

  lastTelegramSent =
      millis();

  telegramStatus =
      "Last message sent successfully";

  addLog(
      "Telegram message sent"
  );

  return true;
}

// ============================================================
// TELEGRAM NORMAL SEND
// ============================================================

bool sendTelegram(
    const String &message,
    bool ignoreCooldown = false
) {

  return sendTelegramMessage(
      message,
      ignoreCooldown,
      ""
  );
}

// ============================================================
// TELEGRAM STATUS KEYBOARD
// ============================================================

bool sendTelegramStatusKeyboard() {

  String keyboard =
      "{\"keyboard\":[[{\"text\":\"/status\"}]],"
      "\"resize_keyboard\":true,"
      "\"one_time_keyboard\":false,"
      "\"is_persistent\":true}";

  return sendTelegramMessage(
      "📊 Water Monitor commands\n"
      "Press /status to get the current status.",
      true,
      keyboard
  );
}

// ============================================================
// TELEGRAM COMMAND REGISTRATION
// ============================================================

bool setupTelegramCommands() {

  String commands =
      "{\"commands\":["
      "{\"command\":\"status\","
      "\"description\":\"Get current water status\"}"
      "]}";

  String url =
      "https://api.telegram.org/bot" +
      String(TELEGRAM_BOT_TOKEN) +
      "/setMyCommands"
      "?commands=" +
      urlEncode(
          commands
      );

  String response;

  if (
      !telegramRequest(
          url,
          response
      )
  ) {

    addLog(
        "Telegram command registration failed"
    );

    return false;
  }

  addLog(
      "Telegram /status command registered"
  );

  return true;
}

// ============================================================
// WATER PRESENCE FILTER
// ============================================================

bool readWaterPresenceFiltered() {

  int highCount = 0;

  // Make absolutely sure the sensing circuit starts
  // unpowered.
  digitalWrite(
      WATER_DRIVE_PIN,
      LOW
  );

  delayMicroseconds(100);

  // Briefly energize the sensing circuit.
  digitalWrite(
      WATER_DRIVE_PIN,
      HIGH
  );

  delay(5);

  // ----------------------------------------------------------
  // Multiple samples
  // ----------------------------------------------------------

  for (
      int i = 0;
      i < SENSOR_SAMPLE_COUNT;
      i++
  ) {

    if (
        digitalRead(
            WATER_SENSE_PIN
        ) == HIGH
    ) {

      highCount++;
    }

    delayMicroseconds(
        SENSOR_SAMPLE_DELAY_US
    );
  }

  // Immediately remove voltage.
  digitalWrite(
      WATER_DRIVE_PIN,
      LOW
  );

  return (
      highCount >=
      SENSOR_WET_REQUIRED
  );
}

// ============================================================
// LEVEL SENSOR FILTER
// ============================================================

void readAllLevelSensorsFiltered(
    bool states[4]
) {

  int highCount[4] = {
      0,
      0,
      0,
      0
  };

  // The bottom electrode is the common/reference electrode.
  // Energize it only while taking the level measurements.
  digitalWrite(
      LEVEL_COMMON_PIN,
      LOW
  );

  delayMicroseconds(100);

  digitalWrite(
      LEVEL_COMMON_PIN,
      HIGH
  );

  delayMicroseconds(100);

  // Sample all four independent level electrodes while the
  // common/bottom electrode is energized.
  for (
      int i = 0;
      i < SENSOR_SAMPLE_COUNT;
      i++
  ) {

    if (
        digitalRead(
            LEVEL1_PIN
        ) == HIGH
    )
      highCount[0]++;

    if (
        digitalRead(
            LEVEL2_PIN
        ) == HIGH
    )
      highCount[1]++;

    if (
        digitalRead(
            LEVEL3_PIN
        ) == HIGH
    )
      highCount[2]++;

    if (
        digitalRead(
            LEVEL4_PIN
        ) == HIGH
    )
      highCount[3]++;

    delayMicroseconds(
        SENSOR_SAMPLE_DELAY_US
    );
  }

  // Immediately remove voltage from the electrodes.
  digitalWrite(
      LEVEL_COMMON_PIN,
      LOW
  );

  for (
      int i = 0;
      i < 4;
      i++
  ) {

    states[i] =
        highCount[i] >=
        SENSOR_WET_REQUIRED;
  }
}

// ============================================================
// CALCULATE LEVEL FROM CONFIRMED SENSOR STATES
// ============================================================

int calculateConfirmedWaterLevel() {

  if (
      confirmedLevelSensors[3]
  )
    return 4;

  if (
      confirmedLevelSensors[2]
  )
    return 3;

  if (
      confirmedLevelSensors[1]
  )
    return 2;

  if (
      confirmedLevelSensors[0]
  )
    return 1;

  return 0;
}

// ============================================================
// WATER LEVEL TEXT
// ============================================================

String waterLevelText(
    int level
) {

  switch (level) {

    case 0:
      return "EMPTY";

    case 1:
      return "LEVEL 1";

    case 2:
      return "LEVEL 2";

    case 3:
      return "LEVEL 3";

    case 4:
      return "FULL";

    case 5:
      return "FULL";
  }

  return "UNKNOWN";
}

// ============================================================
// WATER PRESENCE CHANGE
// ============================================================

void handleWaterPresenceChange(
    bool newState
) {

  waterPresence =
      newState;

  if (newState) {

    addLog(
        "Water presence detected"
    );

    sendTelegram(
        "💧Manjeera WATER DETECTED  ",
        true
    );

  } else {

    addLog(
        "Water presence cleared"
    );

    sendTelegram(
        "🔵Manjeera WATER CLEARED",
        true
    );
  }
}

// ============================================================
// WATER LEVEL CHANGE
// ============================================================

void handleWaterLevelChange(
    int newLevel
) {

  currentWaterLevel =
      newLevel;

  String message;

  if (
      newLevel == 0
  ) {

    message =
        "🔵 WATER LEVEL: EMPTY";

  } else if (
      newLevel == 4
  ) {

    message =
        "🔴 WATER LEVEL: FULL";

  } else {

    message =
        "💧Tank WATER LEVEL: " +
        String(newLevel) +
        "/4";
  }

  addLog(
      "Water level changed: " +
      waterLevelText(
          newLevel
      )
  );

  sendTelegram(
      message
  );
}

// ============================================================
// SENSOR PROCESSING
// ============================================================

void updateSensors() {

  if (
      millis() -
          lastSensorRead <
      SENSOR_INTERVAL
  ) {

    return;
  }

  lastSensorRead =
      millis();

  // ==========================================================
  // WATER PRESENCE
  // ==========================================================

  bool filteredWater =
      readWaterPresenceFiltered();

  if (
      filteredWater !=
      lastRawWaterPresence
  ) {

    lastRawWaterPresence =
        filteredWater;

    waterCandidateSince =
        millis();

    addLog(
        "Water presence candidate: " +
        String(
            filteredWater
                ? "WET"
                : "DRY"
        )
    );
  }

  if (
      filteredWater !=
          waterPresence &&
      millis() -
          waterCandidateSince >=
          WATER_DEBOUNCE_TIME
  ) {

    handleWaterPresenceChange(
        filteredWater
    );
  }

  // ==========================================================
  // WATER LEVEL SENSORS
  // ==========================================================

  bool filteredLevels[4];

  readAllLevelSensorsFiltered(
      filteredLevels
  );

  // ----------------------------------------------------------
  // Process each level independently
  // ----------------------------------------------------------

  for (
      int i = 0;
      i < 4;
      i++
  ) {

    if (
        filteredLevels[i] !=
        pendingLevelSensors[i]
    ) {

      pendingLevelSensors[i] =
          filteredLevels[i];

      levelSensorCandidateSince[i] =
          millis();

      addLog(
          "Level " +
          String(i + 1) +
          " candidate: " +
          String(
              filteredLevels[i]
                  ? "WET"
                  : "DRY"
          )
      );
    }

    // --------------------------------------------------------
    // Confirm individual sensor after 3 seconds
    // --------------------------------------------------------

    if (
        filteredLevels[i] !=
            confirmedLevelSensors[i] &&
        millis() -
            levelSensorCandidateSince[i] >=
            LEVEL_DEBOUNCE_TIME
    ) {

      confirmedLevelSensors[i] =
          filteredLevels[i];

      addLog(
          "Level " +
          String(i + 1) +
          " confirmed: " +
          String(
              confirmedLevelSensors[i]
                  ? "WET"
                  : "DRY"
          )
      );
    }
  }

  // ==========================================================
  // CALCULATE OVERALL CONFIRMED LEVEL
  // ==========================================================

  int confirmedLevel =
      calculateConfirmedWaterLevel();

  if (
      confirmedLevel !=
      currentWaterLevel
  ) {

    handleWaterLevelChange(
        confirmedLevel
    );
  }

  lastRawWaterLevel =
      confirmedLevel;
}

// ============================================================
// UPTIME
// ============================================================

String getUptime() {

  unsigned long seconds =
      (millis() -
       bootTime) /
      1000;

  unsigned long days =
      seconds / 86400;

  seconds %= 86400;

  unsigned long hours =
      seconds / 3600;

  seconds %= 3600;

  unsigned long minutes =
      seconds / 60;

  seconds %= 60;

  char buffer[64];

  snprintf(
      buffer,
      sizeof(buffer),
      "%lu days %02lu:%02lu:%02lu",
      days,
      hours,
      minutes,
      seconds
  );

  return String(buffer);
}

// ============================================================
// TELEGRAM STATUS MESSAGE
// ============================================================

String buildTelegramStatus() {

  String message =
      "💧 WATER MONITOR STATUS\n\n";

  // ----------------------------------------------------------
  // Presence
  // ----------------------------------------------------------

  message +=
      "Manjeera (P25,P26): ";

  message +=
      waterPresence
          ? "💧 WATER DETECTED"
          : "🔵 DRY";

  message += "\n";

  // ----------------------------------------------------------
  // Overall level
  // ----------------------------------------------------------

  message +=
      "Water Level: " +
      waterLevelText(
          currentWaterLevel
      ) +
      " (" +
      String(currentWaterLevel) +
      "/4)\n\n";

  // ----------------------------------------------------------
  // Individual sensors
  // ----------------------------------------------------------

  message +=
      "LEVEL SENSORS (27, 32, 33, 16, 17)\n";

  for (
      int i = 0;
      i < 4;
      i++
  ) {

    message +=
        "L" +
        String(i + 1) +
        ": ";

    message +=
        confirmedLevelSensors[i]
            ? "WET"
            : "DRY";

    message += "\n";
  }

  message += "\n";

  // ----------------------------------------------------------
  // Wi-Fi
  // ----------------------------------------------------------

  message +=
      "NETWORK\n";

  if (
      wifiConnected &&
      WiFi.status() ==
          WL_CONNECTED
  ) {

    String ssid =
        preferences.getString(
            "ssid",
            ""
        );

    message +=
        "WiFi: CONNECTED";

    if (
        ssid.length() > 0
    ) {

      message +=
          " (" +
          ssid +
          ")";
    }

  } else {

    message +=
        "WiFi: DISCONNECTED\n";
  }

  message +=
      "IPv4: " +
      (
          currentIPv4.length()
              ? currentIPv4
              : "Unavailable"
      ) +
      "\n";

  // ----------------------------------------------------------
  // DuckDNS
  // ----------------------------------------------------------

  message += "\n";

  message +=
      "Domain: " +
      String(
          DUCKDNS_DOMAIN
      ) +
      ".duckdns.org\n";

  // ----------------------------------------------------------
  // System
  // ----------------------------------------------------------

  message += "\n";

  message +=
      "SYSTEM\n";

  message +=
      "Firmware: V" +
      String(
          FIRMWARE_VERSION
      ) +
      "\n";

  return message;
}

// ============================================================
// TELEGRAM JSON NUMBER EXTRACTION
// ============================================================

String extractJsonNumber(
    const String &json,
    const String &key,
    int startAt
) {

  String searchKey =
      "\"" +
      key +
      "\"";

  int keyPos =
      json.indexOf(
          searchKey,
          startAt
      );

  if (
      keyPos < 0
  ) {

    return "";
  }

  int colonPos =
      json.indexOf(
          ':',
          keyPos +
          searchKey.length()
      );

  if (
      colonPos < 0
  ) {

    return "";
  }

  int start =
      colonPos + 1;

  while (
      start <
          (int)json.length() &&
      (
          json.charAt(start) ==
              ' ' ||
          json.charAt(start) ==
              '\t'
      )
  ) {

    start++;
  }

  int end =
      start;

  if (
      end <
          (int)json.length() &&
      json.charAt(end) ==
          '-'
  ) {

    end++;
  }

  while (
      end <
          (int)json.length() &&
      isDigit(
          json.charAt(end)
      )
  ) {

    end++;
  }

  return json.substring(
      start,
      end
  );
}

// ============================================================
// TELEGRAM JSON STRING EXTRACTION
// ============================================================

String extractJsonString(
    const String &json,
    const String &key,
    int startAt
) {

  String searchKey =
      "\"" +
      key +
      "\"";

  int keyPos =
      json.indexOf(
          searchKey,
          startAt
      );

  if (
      keyPos < 0
  ) {

    return "";
  }

  int colonPos =
      json.indexOf(
          ':',
          keyPos +
          searchKey.length()
      );

  if (
      colonPos < 0
  ) {

    return "";
  }

  int quoteStart =
      json.indexOf(
          '"',
          colonPos + 1
      );

  if (
      quoteStart < 0
  ) {

    return "";
  }

  String result = "";

  bool escaped =
      false;

  for (
      int i =
          quoteStart + 1;
      i < (int)json.length();
      i++
  ) {

    char c =
        json.charAt(i);

    if (escaped) {

      switch (c) {

        case 'n':
          result += '\n';
          break;

        case 'r':
          result += '\r';
          break;

        case 't':
          result += '\t';
          break;

        case '"':
          result += '"';
          break;

        case '\\':
          result += '\\';
          break;

        default:
          result += c;
          break;
      }

      escaped =
          false;

      continue;
    }

    if (c == '\\') {

      escaped =
          true;

      continue;
    }

    if (c == '"') {

      break;
    }

    result += c;
  }

  return result;
}

// ============================================================
// TELEGRAM POLLING
// ============================================================

void pollTelegram() {

  if (!wifiConnected) {

    return;
  }

  if (
      millis() -
          lastTelegramPoll <
      TELEGRAM_POLL_INTERVAL
  ) {

    return;
  }

  lastTelegramPoll =
      millis();

  String url =
      "https://api.telegram.org/bot" +
      String(TELEGRAM_BOT_TOKEN) +
      "/getUpdates"
      "?offset=" +
      String(
          telegramUpdateOffset
      ) +
      "&limit=5"
      "&timeout=0";

  String response;

  if (
      !telegramRequest(
          url,
          response
      )
  ) {

    addLog(
        "Telegram polling failed"
    );

    return;
  }

  // ----------------------------------------------------------
  // Find all returned updates
  // ----------------------------------------------------------

  int searchPosition =
      0;

  bool foundUpdate =
      false;

  while (true) {

    int updatePosition =
        response.indexOf(
            "\"update_id\"",
            searchPosition
        );

    if (
        updatePosition < 0
    ) {

      break;
    }

    foundUpdate =
        true;

    String updateIdString =
        extractJsonNumber(
            response,
            "update_id",
            updatePosition
        );

    long updateId =
        updateIdString.toInt();

    if (
        updateId >=
        telegramUpdateOffset
    ) {

      telegramUpdateOffset =
          updateId + 1;
    }

    // --------------------------------------------------------
    // Find message object belonging to this update
    // --------------------------------------------------------

    int messagePosition =
        response.indexOf(
            "\"message\"",
            updatePosition
        );

    if (
        messagePosition < 0
    ) {

      searchPosition =
          updatePosition + 1;

      continue;
    }

    // --------------------------------------------------------
    // Find chat object
    // --------------------------------------------------------

    int chatPosition =
        response.indexOf(
            "\"chat\"",
            messagePosition
        );

    if (
        chatPosition < 0
    ) {

      searchPosition =
          updatePosition + 1;

      continue;
    }

    // --------------------------------------------------------
    // Extract chat ID
    // --------------------------------------------------------

    String chatId =
        extractJsonNumber(
            response,
            "id",
            chatPosition
        );

    // --------------------------------------------------------
    // Extract message text
    // --------------------------------------------------------

    String text =
        extractJsonString(
            response,
            "text",
            messagePosition
        );

    // --------------------------------------------------------
    // Only accept our configured chat
    // --------------------------------------------------------

    if (
        chatId !=
        String(
            TELEGRAM_CHAT_ID
        )
    ) {

      searchPosition =
          updatePosition + 1;

      continue;
    }

    if (
        text.length() == 0
    ) {

      searchPosition =
          updatePosition + 1;

      continue;
    }

    addLog(
        "Telegram command received: " +
        text
    );

    // --------------------------------------------------------
    // /status
    //
    // Accept:
    // /status
    // /status@BotName
    // --------------------------------------------------------

    if (
        text == "/status" ||
        text.startsWith(
            "/status@"
        )
    ) {

      String statusMessage =
          buildTelegramStatus();

      sendTelegram(
          statusMessage,
          true
      );
    }

    // --------------------------------------------------------
    // /start
    // --------------------------------------------------------

    else if (
        text == "/start" ||
        text.startsWith(
            "/start@"
        )
    ) {

      sendTelegramStatusKeyboard();

      sendTelegram(
          "🟢 ESP32 Water Monitor\n"
          "Use /status to get the current water status.",
          true
      );
    }

    searchPosition =
        updatePosition + 1;
  }

  if (
      foundUpdate
  ) {

    telegramStatus =
        "Listening for /status";
  }
}

// ============================================================
// HTML HEADER
// ============================================================

String htmlHeader(
    const String &title
) {

  String html =
      R"rawliteral(
<!DOCTYPE html>
<html>
<head>

<meta name="viewport"
      content="width=device-width,initial-scale=1">

<title>)rawliteral";

  html +=
      title;

  html +=
      R"rawliteral(</title>

<style>

body {
  font-family: Arial, sans-serif;
  margin: 0;
  padding: 20px;
  background: #f2f4f7;
  color: #222;
}

.container {
  max-width: 1000px;
  margin: auto;
}

.card {
  background: white;
  padding: 18px;
  margin-bottom: 15px;
  border-radius: 12px;
  box-shadow: 0 2px 8px rgba(0,0,0,.08);
}

.grid {
  display: grid;
  grid-template-columns:
    repeat(auto-fit,minmax(220px,1fr));
  gap: 15px;
}

.value {
  font-size: 24px;
  font-weight: bold;
  margin-top: 8px;
}

.good {
  color: #198754;
}

.warning {
  color: #d97706;
}

.danger {
  color: #dc2626;
}

.blue {
  color: #2563eb;
}

button {
  padding: 11px 16px;
  border: 0;
  border-radius: 8px;
  cursor: pointer;
  margin: 4px;
  font-size: 15px;
}

input {
  width: 100%;
  box-sizing: border-box;
  padding: 10px;
  margin-top: 5px;
  margin-bottom: 12px;
  border: 1px solid #ccc;
  border-radius: 7px;
}

pre {
  white-space: pre-wrap;
  word-break: break-word;
  background: #111;
  color: #eee;
  padding: 12px;
  border-radius: 8px;
  max-height: 400px;
  overflow-y: auto;
}

a {
  text-decoration: none;
}

</style>

</head>

<body>

<div class="container">

)rawliteral";

  return html;
}

// ============================================================
// DASHBOARD
// ============================================================

void handleRoot() {

  if (!authenticate())
    return;

  String html =
      htmlHeader(
          "ESP32 Water Monitor"
      );

  html +=
      R"rawliteral(

<div class="card">

<h1>ESP32 Water Monitor</h1>

<p>
Firmware:
<b>)rawliteral";

  html +=
      FIRMWARE_VERSION;

  html +=
      R"rawliteral(</b>
</p>

<p>
Local address:
<b>
http://)rawliteral";

  html +=
      MDNS_HOSTNAME;

  html +=
      R"rawliteral(.local
</b>
</p>

</div>

<div class="grid">

<div class="card">

<h2>Water Presence</h2>

<div class="value">
)rawliteral";

  if (waterPresence) {

    html +=
        "<span class=\"danger\">"
        "WATER DETECTED"
        "</span>";

    // ----------------------------------------------------------
    // Presence
    // ----------------------------------------------------------
    html += "\n";
    html +=
        "Manjeera (P25,P26):";
    html +=
        waterPresence
            ? "💧 WATER DETECTED"
            : "🔵 DRY";

    html += "\n";

    // ----------------------------------------------------------
    // Overall level
    // ----------------------------------------------------------

    html +=
        "Water Level: " +
        waterLevelText(
            currentWaterLevel
        ) +
        " (" +
        String(currentWaterLevel) +
        "/4)\n\n";

    // ----------------------------------------------------------
    // Individual sensors
    // ----------------------------------------------------------

    html +=
        "LEVEL SENSORS (27, 32, 33, 16, 17) \n";

    for (
        int i = 0;
        i < 4;
        i++
    ) {

        html +=
            "L" +
            String(i + 1) +
            ": ";

        html +=
            confirmedLevelSensors[i]
                ? "WET"
                : "DRY ";

        html += "\n";
    }

    html += "\n";

  } else {

    html +=
        "<span class=\"blue\">"
        "DRY"
        "</span>";
            // ----------------------------------------------------------
    // Presence
    // ----------------------------------------------------------

    html +=
        "Manjeera (P25,P26): ";

    html +=
        waterPresence
            ? "💧 WATER DETECTED"
            : "🔵 DRY";

    html += "\n";

    // ----------------------------------------------------------
    // Overall level
    // ----------------------------------------------------------

    html +=
        "Water Level: " +
        waterLevelText(
            currentWaterLevel
        ) +
        " (" +
        String(currentWaterLevel) +
        "/4)\n\n";

    // ----------------------------------------------------------
    // Individual sensors
    // ----------------------------------------------------------

    html +=
        "LEVEL SENSORS (27, 32, 33, 16, 17)\n";

    for (
        int i = 0;
        i < 4;
        i++
    ) {

        html +=
            "L" +
            String(i + 1) +
            ": ";

        html +=
            confirmedLevelSensors[i]
                ? "WET"
                : "DRY";

        html += "\n";
    }

    html += "\n";
  }

  html +=
      R"rawliteral(
</div>

</div>

<div class="card">

<h2>Water Level</h2>

<div class="value">
)rawliteral";

  html +=
      waterLevelText(
          currentWaterLevel
      );

  html +=
      " (" +
      String(
          currentWaterLevel
      ) +
      "/4)";

  html +=
      R"rawliteral(
</div>

</div>

</div>

<div class="card">

<h2>Level Sensors</h2>

<div class="grid">

)rawliteral";

  for (
      int i = 0;
      i < 4;
      i++
  ) {

    bool active =
        confirmedLevelSensors[i];

    html +=
        "<div class=\"card\">";

    html +=
        "<h2>Level " +
        String(i + 1) +
        "</h2>";

    if (active) {

      html +=
          "<div class=\"value good\">"
          "WET"
          "</div>";

    } else {

      html +=
          "<div class=\"value\">"
          "DRY"
          "</div>";
    }

    html +=
        "</div>";
  }

  html +=
      R"rawliteral(

</div>

</div>

<div class="card">

<h2>Network</h2>

<p>
mDNS:
<b>
http://)rawliteral";

  html +=
      MDNS_HOSTNAME;

  html +=
      R"rawliteral(.local
</b>
</p>

<p>
IPv4:
<b>)rawliteral";

  html +=
      currentIPv4.length()
          ? currentIPv4
          : "Unavailable";

  html +=
      R"rawliteral(</b>
</p>

<p>
IPv6:
<b>)rawliteral";

  html +=
      currentIPv6.length()
          ? currentIPv6
          : "Unavailable";

  html +=
      R"rawliteral(</b>
</p>

<p>
RSSI:
<b>)rawliteral";

  if (
      WiFi.status() ==
      WL_CONNECTED
  ) {

    html +=
        String(
            WiFi.RSSI()
        ) +
        " dBm";

  } else {

    html +=
        "N/A";
  }

  html +=
      R"rawliteral(
</b>
</p>

<p>
DuckDNS:
<b>)rawliteral";

  html +=
      String(
          DUCKDNS_DOMAIN
      ) +
      ".duckdns.org";

  html +=
      R"rawliteral(</b>
</p>

<p>
DuckDNS status:
<b>)rawliteral";

  html +=
      duckDNSStatus;

  html +=
      R"rawliteral(
</b>
</p>

</div>

<div class="card">

<h2>Telegram</h2>

<p>
Status:
<b>)rawliteral";

  html +=
      telegramStatus;

  html +=
      R"rawliteral(
</b>
</p>

<p>
Commands:
<b>/status</b>
</p>

<a href="/telegram-test">

<button>
Test Telegram
</button>

</a>

</div>

<div class="card">

<h2>System</h2>

<p>
Uptime:
<b>)rawliteral";

  html +=
      getUptime();

  html +=
      R"rawliteral(
</b>
</p>

<p>
Free heap:
<b>)rawliteral";

  html +=
      String(
          ESP.getFreeHeap()
      );

  html +=
      R"rawliteral(
 bytes
</b>
</p>

<p>
Chip:
<b>)rawliteral";

  html +=
      ESP.getChipModel();

  html +=
      R"rawliteral(
</b>
</p>

<p>
CPU:
<b>)rawliteral";

  html +=
      String(
          ESP.getCpuFreqMHz()
      ) +
      " MHz";

  html +=
      R"rawliteral(
</b>
</p>

</div>

<div class="card">

<h2>Actions</h2>

<a href="/wifi">
<button>
Wi-Fi Settings
</button>
</a>

<a href="/update">
<button>
Firmware Update
</button>
</a>

<a href="/restart"
   onclick="return confirm('Restart ESP32?');">

<button>
Restart
</button>

</a>

</div>

<div class="card">

<h2>Event Logs</h2>

<pre>
)rawliteral";

  for (
      int i = 0;
      i < logCount;
      i++
  ) {

    html +=
        logs[i];

    html +=
        "\n";
  }

  html +=
      R"rawliteral(
</pre>

</div>

</div>

<script>

setTimeout(function() {
  location.reload();
}, 10000);

</script>

</body>
</html>

)rawliteral";

  server.send(
      200,
      "text/html",
      html
  );
}

// ============================================================
// WIFI PAGE
// ============================================================

void handleWiFiPage() {

  if (!authenticate())
    return;

  String savedSSID =
      preferences.getString(
          "ssid",
          ""
      );

  String html =
      htmlHeader(
          "Wi-Fi Settings"
      );

  html +=
      R"rawliteral(

<div class="card">

<h1>Wi-Fi Settings</h1>

<form method="POST"
      action="/savewifi">

<label>
Wi-Fi SSID
</label>

<input
  type="text"
  name="ssid"
  value=")rawliteral";

  html +=
      savedSSID;

  html +=
      R"rawliteral("
  required
>

<label>
Wi-Fi Password
</label>

<input
  type="password"
  name="password"
  placeholder="Enter Wi-Fi password"
>

<button type="submit">
Save & Restart
</button>

</form>

<p>
After saving, the ESP32 will restart and connect
using the new credentials.
</p>

<a href="/">
Back to Dashboard
</a>

</div>

</div>

</body>
</html>

)rawliteral";

  server.send(
      200,
      "text/html",
      html
  );
}

// ============================================================
// SAVE WIFI
// ============================================================

void handleSaveWiFi() {

  if (!authenticate())
    return;

  if (
      !server.hasArg("ssid") ||
      !server.hasArg("password")
  ) {

    server.send(
        400,
        "text/plain",
        "Missing Wi-Fi credentials"
    );

    return;
  }

  String ssid =
      server.arg("ssid");

  String password =
      server.arg("password");

  ssid.trim();

  if (
      ssid.length() == 0
  ) {

    server.send(
        400,
        "text/plain",
        "SSID cannot be empty"
    );

    return;
  }

  preferences.putString(
      "ssid",
      ssid
  );

  preferences.putString(
      "password",
      password
  );

  addLog(
      "Wi-Fi credentials saved"
  );

  server.send(
      200,
      "text/html",
      "<html><body>"
      "<h2>Wi-Fi credentials saved.</h2>"
      "<p>Restarting...</p>"
      "</body></html>"
  );

  delay(1000);

  ESP.restart();
}

// ============================================================
// RESTART
// ============================================================

void handleRestart() {

  if (!authenticate())
    return;

  server.send(
      200,
      "text/html",
      "<html><body>"
      "<h2>ESP32 restarting...</h2>"
      "</body></html>"
  );

  delay(500);

  ESP.restart();
}

// ============================================================
// TELEGRAM TEST
// ============================================================

void handleTelegramTest() {

  if (!authenticate())
    return;

  bool success =
      sendTelegram(
          "🟢 ESP32 Water Monitor\n"
          "Telegram test successful.\n"
          "Firmware: " +
          String(
              FIRMWARE_VERSION
          ),
          true
      );

  if (success) {

    server.send(
        200,
        "text/html",
        "<html><body>"
        "<h2>Telegram test sent successfully.</h2>"
        "<a href='/'>Back</a>"
        "</body></html>"
    );

  } else {

    server.send(
        500,
        "text/html",
        "<html><body>"
        "<h2>Telegram test failed.</h2>"
        "<p>Check the event log.</p>"
        "<a href='/'>Back</a>"
        "</body></html>"
    );
  }
}

// ============================================================
// OTA PAGE
// ============================================================

void handleUpdatePage() {

  if (!authenticate())
    return;

  String html =
      htmlHeader(
          "Firmware Update"
      );

  html +=
      R"rawliteral(

<div class="card">

<h1>Firmware Update</h1>

<p>
Current firmware:
<b>)rawliteral";

  html +=
      FIRMWARE_VERSION;

  html +=
      R"rawliteral(
</b>
</p>

<form
  method="POST"
  action="/update"
  enctype="multipart/form-data"
>

<input
  type="file"
  name="firmware"
  accept=".bin"
  required
>

<br><br>

<button type="submit">
Upload Firmware
</button>

</form>

<p>
Select the compiled ESP32
<b>.bin</b> file.
</p>

<a href="/">
Back to Dashboard
</a>

</div>

</div>

</body>
</html>

)rawliteral";

  server.send(
      200,
      "text/html",
      html
  );
}

// ============================================================
// OTA UPLOAD
// ============================================================

void handleFirmwareUpload() {

  HTTPUpload &upload =
      server.upload();

  if (
      upload.status ==
      UPLOAD_FILE_START
  ) {

    otaRunning =
        true;

    addLog(
        "OTA upload started: " +
        upload.filename
    );

    if (
        !Update.begin(
            UPDATE_SIZE_UNKNOWN
        )
    ) {

      Update.printError(
          Serial
      );

      addLog(
          "OTA Update.begin failed"
      );
    }

  } else if (
      upload.status ==
      UPLOAD_FILE_WRITE
  ) {

    if (
        Update.write(
            upload.buf,
            upload.currentSize
        ) !=
        upload.currentSize
    ) {

      Update.printError(
          Serial
      );

      addLog(
          "OTA write failed"
      );
    }

  } else if (
      upload.status ==
      UPLOAD_FILE_END
  ) {

    if (
        Update.end(true)
    ) {

      addLog(
          "OTA upload completed"
      );

    } else {

      Update.printError(
          Serial
      );

      addLog(
          "OTA finalization failed"
      );
    }

    otaRunning =
        false;

  } else if (
      upload.status ==
      UPLOAD_FILE_ABORTED
  ) {

    Update.abort();

    otaRunning =
        false;

    addLog(
        "OTA upload aborted"
    );
  }
}

// ============================================================
// OTA HANDLER
// ============================================================

void handleFirmwareUpdate() {

  if (!authenticate())
    return;

  if (otaRunning) {

    server.send(
        500,
        "text/plain",
        "OTA already running"
    );

    return;
  }

  server.send(
      200,
      "text/html",
      "<html><body>"
      "<h2>Firmware updated.</h2>"
      "<p>Restarting ESP32...</p>"
      "</body></html>"
  );

  delay(1000);

  ESP.restart();
}

// ============================================================
// 404
// ============================================================

void handleNotFound() {

  if (!authenticate())
    return;

  server.send(
      404,
      "text/plain",
      "404 - Not Found"
  );
}

// ============================================================
// WEB SERVER
// ============================================================

void setupWebServer() {

  server.on(
      "/",
      HTTP_GET,
      handleRoot
  );

  server.on(
      "/wifi",
      HTTP_GET,
      handleWiFiPage
  );

  server.on(
      "/savewifi",
      HTTP_POST,
      handleSaveWiFi
  );

  server.on(
      "/restart",
      HTTP_GET,
      handleRestart
  );

  server.on(
      "/telegram-test",
      HTTP_GET,
      handleTelegramTest
  );

  server.on(
      "/update",
      HTTP_GET,
      handleUpdatePage
  );

  server.on(
      "/update",
      HTTP_POST,
      handleFirmwareUpdate,
      handleFirmwareUpload
  );

  server.onNotFound(
      handleNotFound
  );

  server.begin();

  addLog(
      "Web server started on port 80"
  );
}

// ============================================================
// INITIALIZE FILTERED SENSOR STATES
// ============================================================

void initializeSensors() {

  addLog(
      "Taking initial filtered sensor readings..."
  );

  // ----------------------------------------------------------
  // Presence
  // ----------------------------------------------------------

  waterPresence =
      readWaterPresenceFiltered();

  lastRawWaterPresence =
      waterPresence;

  waterCandidateSince =
      millis();

  // ----------------------------------------------------------
  // Level sensors
  // ----------------------------------------------------------

  bool initialLevels[4];

  readAllLevelSensorsFiltered(
      initialLevels
  );

  for (
      int i = 0;
      i < 4;
      i++
  ) {

    confirmedLevelSensors[i] =
        initialLevels[i];

    pendingLevelSensors[i] =
        initialLevels[i];

    levelSensorCandidateSince[i] =
        millis();

    addLog(
        "Initial Level " +
        String(i + 1) +
        ": " +
        String(
            initialLevels[i]
                ? "WET"
                : "DRY"
        )
    );
  }

  currentWaterLevel =
      calculateConfirmedWaterLevel();

  lastRawWaterLevel =
      currentWaterLevel;

  addLog(
      "Initial water presence: " +
      String(
          waterPresence
              ? "WET"
              : "DRY"
      )
  );

  addLog(
      "Initial water level: " +
      waterLevelText(
          currentWaterLevel
      )
  );
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(
      115200
  );

  delay(500);

  bootTime =
      millis();

  addLog(
      "================================"
  );

  addLog(
      "ESP32 Water Monitor V" +
      String(
          FIRMWARE_VERSION
      )
  );

  addLog(
      "Booting..."
  );

  // ----------------------------------------------------------
  // PREFERENCES
  // ----------------------------------------------------------

  preferences.begin(
      "watermon",
      false
  );

  // ----------------------------------------------------------
  // SENSOR PINS
  // ----------------------------------------------------------

  pinMode(
      WATER_DRIVE_PIN,
      OUTPUT
  );

  digitalWrite(
      WATER_DRIVE_PIN,
      LOW
  );

  pinMode(
      WATER_SENSE_PIN,
      INPUT_PULLDOWN
  );

  pinMode(
      LEVEL_COMMON_PIN,
      OUTPUT
  );

  digitalWrite(
      LEVEL_COMMON_PIN,
      LOW
  );

  pinMode(
      LEVEL1_PIN,
      INPUT_PULLDOWN
  );

  pinMode(
      LEVEL2_PIN,
      INPUT_PULLDOWN
  );

  pinMode(
      LEVEL3_PIN,
      INPUT_PULLDOWN
  );

  pinMode(
      LEVEL4_PIN,
      INPUT_PULLDOWN
  );

  addLog(
      "Sensor pins initialized"
  );

  // ----------------------------------------------------------
  // WIFI
  // ----------------------------------------------------------

  if (
      !connectToWiFi()
  ) {

    startSetupAP();

  } else {

    // --------------------------------------------------------
    // DUCKDNS
    // --------------------------------------------------------

    updateDuckDNS();

    // --------------------------------------------------------
    // TELEGRAM COMMANDS
    // --------------------------------------------------------

    setupTelegramCommands();

    // --------------------------------------------------------
    // TELEGRAM STARTUP MESSAGE
    // --------------------------------------------------------

    String startupMessage =
        "🟢 ESP32 Water Monitor started\n"
        "Firmware: " +
        String(
            FIRMWARE_VERSION
        ) +
        "\nIPv4: " +
        currentIPv4;

    sendTelegram(
        startupMessage,
        true
    );

    // --------------------------------------------------------
    // Show /status button
    // --------------------------------------------------------

    sendTelegramStatusKeyboard();
  }

  // ----------------------------------------------------------
  // WEB SERVER
  // ----------------------------------------------------------

  setupWebServer();

  // ----------------------------------------------------------
  // INITIAL FILTERED SENSOR STATE
  // ----------------------------------------------------------

  initializeSensors();

  addLog(
      "System ready"
  );
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // ----------------------------------------------------------
  // WEB SERVER
  // ----------------------------------------------------------

  server.handleClient();

  // ----------------------------------------------------------
  // SENSOR MONITORING
  // ----------------------------------------------------------

  updateSensors();

  // ----------------------------------------------------------
  // TELEGRAM POLLING
  // ----------------------------------------------------------

  pollTelegram();

  // ----------------------------------------------------------
  // WIFI RECONNECT
  // ----------------------------------------------------------

  if (
      WiFi.getMode() ==
          WIFI_STA &&
      WiFi.status() !=
          WL_CONNECTED
  ) {

    if (wifiConnected) {

      wifiConnected =
          false;

      currentIPv4 =
          "";

      currentIPv6 =
          "";

      stopMDNS();

      addLog(
          "Wi-Fi disconnected"
      );
    }

  } else if (
      WiFi.getMode() ==
          WIFI_STA &&
      WiFi.status() ==
          WL_CONNECTED
  ) {

    if (!wifiConnected) {

      wifiConnected =
          true;

      currentIPv4 =
          WiFi.localIP().toString();

      currentIPv6 =
          getIPv6Address();

      addLog(
          "Wi-Fi reconnected"
      );

      addLog(
          "IPv4: " +
          currentIPv4
      );

      if (
          currentIPv6.length() > 0
      ) {

        addLog(
            "IPv6: " +
            currentIPv6
        );
      }

      startMDNS();

      updateDuckDNS();

      setupTelegramCommands();
    }
  }

  // ----------------------------------------------------------
  // DUCKDNS PERIODIC UPDATE
  // ----------------------------------------------------------

  if (
      wifiConnected &&
      millis() -
          lastDuckDNSUpdate >=
          DUCKDNS_UPDATE_INTERVAL
  ) {

    updateDuckDNS();
  }

  // ----------------------------------------------------------
  // Small yield
  // ----------------------------------------------------------

  delay(5);
}
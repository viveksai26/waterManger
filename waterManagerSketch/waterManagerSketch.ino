#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <time.h>
#include <esp_task_wdt.h>

#include "config.h"

// ============================================================
// VERSION
// ============================================================

#define FIRMWARE_VERSION "4.9"

// ============================================================
// CLOCK (NTP)
//
// POSIX timezone string. India Standard Time = UTC+5:30.
// Can be overridden in config.h.
// ============================================================

#ifndef TIMEZONE
#define TIMEZONE "IST-5:30"
#endif

#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"

// ============================================================
// DAILY SUMMARY
//
// Hour of the day (0-23, local time) to send the daily
// Telegram summary. Can be overridden in config.h.
// ============================================================

#ifndef DAILY_SUMMARY_HOUR
#define DAILY_SUMMARY_HOUR 8
#endif

// ============================================================
// WATCHDOG
//
// Restart the ESP32 if the main loop stops running for this
// long (for example a stuck network call).
// ============================================================

const uint32_t WATCHDOG_TIMEOUT_MS =
    60000;

// ============================================================
// SUPPLY HISTORY
// ============================================================

#define HISTORY_SIZE 20

// Number of entries shown by /history and on the dashboard
#define HISTORY_SHOWN 10

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

// Set by Telegram /restart; handled after the poll finishes
bool restartRequested =
    false;

// ============================================================
// SUPPLY HISTORY STATE
//
// One entry per Manjeera supply (water detected -> cleared).
// Stored in flash so it survives restarts.
//
// start / end are Unix times (0 = unknown, clock not synced).
// ============================================================

struct SupplyEvent {
  uint32_t start;
  uint32_t end;
  uint32_t durationSec;
  uint8_t open;
  uint8_t reserved[3];
};

struct SupplyHistory {
  uint8_t count;
  uint8_t reserved[3];
  SupplyEvent events[HISTORY_SIZE];
};

// events[0] = oldest, events[count - 1] = newest
SupplyHistory supplyHistory;

// millis() when the current supply started (this boot only)
unsigned long supplyStartMs =
    0;

bool supplyStartMsValid =
    false;

// ============================================================
// TANK FILL HISTORY STATE
//
// One entry per fill: tank level rising from a lower level
// all the way to FULL. Stored in flash.
// ============================================================

#define FILL_HISTORY_SIZE 10

struct FillEvent {
  uint32_t end;          // Unix time FULL reached (0 = unknown)
  uint32_t durationSec;  // time from first rise to FULL
  uint8_t fromLevel;     // level the fill started from (0-3)
  uint8_t reserved[3];
};

struct FillHistory {
  uint8_t count;
  uint8_t reserved[3];
  FillEvent events[FILL_HISTORY_SIZE];
};

// events[0] = oldest, events[count - 1] = newest
FillHistory fillHistory;

// Current fill in progress (this boot only)
bool fillInProgress =
    false;

int fillFromLevel =
    0;

unsigned long fillStartMs =
    0;

// ============================================================
// PENDING LEVEL ALERT
//
// Level changes that could not be sent (cooldown or network
// failure) are retried instead of being dropped.
// ============================================================

bool levelAlertPending =
    false;

String pendingLevelAlert =
    "";

unsigned long lastLevelAlertAttempt =
    0;

// ============================================================
// CLOCK HELPERS
// ============================================================

bool timeValid() {

  // Any time after 2023 means NTP has synced
  return time(nullptr) >
         1700000000;
}

String formatTime(
    time_t t,
    const char *format
) {

  if (t == 0) {

    return "unknown";
  }

  struct tm timeInfo;

  localtime_r(
      &t,
      &timeInfo
  );

  char buffer[32];

  strftime(
      buffer,
      sizeof(buffer),
      format,
      &timeInfo
  );

  return String(buffer);
}

String nowText() {

  if (!timeValid()) {

    return "not synced";
  }

  return formatTime(
      time(nullptr),
      "%d %b %Y %H:%M:%S"
  );
}

String durationText(
    uint32_t seconds
) {

  uint32_t minutes =
      (seconds + 30) / 60;

  if (minutes < 60) {

    return String(minutes) +
           " min";
  }

  return String(minutes / 60) +
         "h " +
         String(minutes % 60) +
         "m";
}

// ============================================================
// WATCHDOG HELPERS
// ============================================================

bool watchdogActive =
    false;

void feedWatchdog() {

  if (watchdogActive) {

    esp_task_wdt_reset();
  }
}

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

  String stamp =
      timeValid()
          ? formatTime(
                time(nullptr),
                "%d %b %H:%M:%S"
            )
          : String(millis() / 1000) +
                "s";

  String entry =
      "[" +
      stamp +
      "] " +
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

    feedWatchdog();

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

  feedWatchdog();

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

    // Each attempt is bounded by its own timeouts
    feedWatchdog();

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

  // Every message ends with a tappable /status command
  String fullMessage =
      message;

  if (
      !fullMessage.endsWith(
          "/status"
      )
  ) {

    fullMessage +=
        "\n\n/status";
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
          fullMessage
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
      "{\"keyboard\":[[{\"text\":\"/status\"},"
      "{\"text\":\"/history\"}]],"
      "\"resize_keyboard\":true,"
      "\"one_time_keyboard\":false,"
      "\"is_persistent\":true}";

  return sendTelegramMessage(
      "📊 Water Monitor commands\n"
      "/status - current status\n"
      "/history - recent Manjeera supply times\n"
      "/restart - restart the ESP32",
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
      "\"description\":\"Get current water status\"},"
      "{\"command\":\"history\","
      "\"description\":\"Recent Manjeera supply times\"},"
      "{\"command\":\"restart\","
      "\"description\":\"Restart the ESP32\"}"
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
      "Telegram commands registered"
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
// SUPPLY HISTORY STORAGE
// ============================================================

void loadSupplyHistory() {

  memset(
      &supplyHistory,
      0,
      sizeof(supplyHistory)
  );

  if (
      preferences.getBytesLength(
          "history"
      ) !=
      sizeof(supplyHistory)
  ) {

    addLog(
        "Supply history: none stored"
    );

    return;
  }

  preferences.getBytes(
      "history",
      &supplyHistory,
      sizeof(supplyHistory)
  );

  if (
      supplyHistory.count >
      HISTORY_SIZE
  ) {

    supplyHistory.count =
        0;
  }

  addLog(
      "Supply history loaded: " +
      String(supplyHistory.count) +
      " entries"
  );
}

void saveSupplyHistory() {

  preferences.putBytes(
      "history",
      &supplyHistory,
      sizeof(supplyHistory)
  );
}

SupplyEvent *latestSupply() {

  if (
      supplyHistory.count == 0
  ) {

    return nullptr;
  }

  return &supplyHistory.events[
      supplyHistory.count - 1
  ];
}

// ============================================================
// SUPPLY HISTORY: START / END
// ============================================================

void startSupplyRecord() {

  // Drop the oldest entry when full
  if (
      supplyHistory.count >=
      HISTORY_SIZE
  ) {

    for (
        int i = 0;
        i < HISTORY_SIZE - 1;
        i++
    ) {

      supplyHistory.events[i] =
          supplyHistory.events[i + 1];
    }

    supplyHistory.count =
        HISTORY_SIZE - 1;
  }

  SupplyEvent &event =
      supplyHistory.events[
          supplyHistory.count++
      ];

  memset(
      &event,
      0,
      sizeof(event)
  );

  event.start =
      timeValid()
          ? (uint32_t)time(nullptr)
          : 0;

  event.open =
      1;

  supplyStartMs =
      millis();

  supplyStartMsValid =
      true;

  saveSupplyHistory();
}

// Returns the closed event, or nullptr if none was open
SupplyEvent *endSupplyRecord() {

  SupplyEvent *event =
      latestSupply();

  if (
      event == nullptr ||
      !event->open
  ) {

    return nullptr;
  }

  event->open =
      0;

  event->end =
      timeValid()
          ? (uint32_t)time(nullptr)
          : 0;

  if (
      supplyStartMsValid
  ) {

    event->durationSec =
        (millis() -
         supplyStartMs) /
        1000;

  } else if (
      event->start != 0 &&
      event->end != 0
  ) {

    event->durationSec =
        event->end -
        event->start;
  }

  supplyStartMsValid =
      false;

  saveSupplyHistory();

  return event;
}

// ------------------------------------------------------------
// If the supply started before NTP synced, fill in the start
// time once the clock becomes valid.
// ------------------------------------------------------------

void backfillSupplyStart() {

  SupplyEvent *event =
      latestSupply();

  if (
      event == nullptr ||
      !event->open ||
      event->start != 0 ||
      !supplyStartMsValid ||
      !timeValid()
  ) {

    return;
  }

  event->start =
      (uint32_t)time(nullptr) -
      (millis() -
       supplyStartMs) /
          1000;

  saveSupplyHistory();
}

// ------------------------------------------------------------
// On boot: reconcile an entry left open by a restart
// ------------------------------------------------------------

void reconcileSupplyOnBoot() {

  SupplyEvent *event =
      latestSupply();

  bool openRecord =
      event != nullptr &&
      event->open;

  if (
      waterPresence &&
      !openRecord
  ) {

    addLog(
        "Water present at boot: starting supply record"
    );

    startSupplyRecord();

  } else if (
      !waterPresence &&
      openRecord
  ) {

    // Supply ended while the ESP32 was off/restarting.
    // End time is not known exactly.
    event->open =
        0;

    event->end =
        0;

    event->durationSec =
        0;

    saveSupplyHistory();

    addLog(
        "Open supply record closed (ended during restart)"
    );
  }

  // Water present and record open: keep it open.
  // Duration is computed from start/end Unix times.
}

// ============================================================
// TANK FILL HISTORY STORAGE
// ============================================================

void loadFillHistory() {

  memset(
      &fillHistory,
      0,
      sizeof(fillHistory)
  );

  if (
      preferences.getBytesLength(
          "fills"
      ) !=
      sizeof(fillHistory)
  ) {

    return;
  }

  preferences.getBytes(
      "fills",
      &fillHistory,
      sizeof(fillHistory)
  );

  if (
      fillHistory.count >
      FILL_HISTORY_SIZE
  ) {

    fillHistory.count =
        0;
  }

  addLog(
      "Fill history loaded: " +
      String(fillHistory.count) +
      " entries"
  );
}

FillEvent *recordFill(
    int fromLevel,
    uint32_t durationSec
) {

  // Drop the oldest entry when full
  if (
      fillHistory.count >=
      FILL_HISTORY_SIZE
  ) {

    for (
        int i = 0;
        i < FILL_HISTORY_SIZE - 1;
        i++
    ) {

      fillHistory.events[i] =
          fillHistory.events[i + 1];
    }

    fillHistory.count =
        FILL_HISTORY_SIZE - 1;
  }

  FillEvent &event =
      fillHistory.events[
          fillHistory.count++
      ];

  memset(
      &event,
      0,
      sizeof(event)
  );

  event.end =
      timeValid()
          ? (uint32_t)time(nullptr)
          : 0;

  event.durationSec =
      durationSec;

  event.fromLevel =
      fromLevel;

  preferences.putBytes(
      "fills",
      &fillHistory,
      sizeof(fillHistory)
  );

  return &event;
}

String fillLevelName(
    int level
) {

  return level == 0
             ? String("EMPTY")
             : "L" + String(level);
}

String fillEventText(
    const FillEvent &event
) {

  return formatTime(
             event.end,
             "%d %b %H:%M"
         ) +
         ": " +
         fillLevelName(
             event.fromLevel
         ) +
         " → FULL in " +
         durationText(
             event.durationSec
         );
}

// ============================================================
// SUPPLY HISTORY TEXT
// ============================================================

String supplyEventText(
    const SupplyEvent &event
) {

  String text =
      formatTime(
          event.start,
          "%d %b %H:%M"
      );

  if (event.open) {

    text += " → now";

    if (
        event.start != 0 &&
        timeValid()
    ) {

      text +=
          " (" +
          durationText(
              (uint32_t)time(nullptr) -
              event.start
          ) +
          ", ongoing)";

    } else {

      text += " (ongoing)";
    }

  } else if (
      event.end == 0 &&
      event.durationSec == 0
  ) {

    text +=
        " → ? (ended during restart)";

  } else {

    text +=
        " → " +
        (
            event.end != 0
                ? formatTime(
                      event.end,
                      "%H:%M"
                  )
                : String("?")
        ) +
        " (" +
        durationText(
            event.durationSec
        ) +
        ")";
  }

  return text;
}

String buildHistoryMessage() {

  String message =
      "📜 MANJEERA SUPPLY HISTORY\n\n";

  if (
      supplyHistory.count == 0
  ) {

    message +=
        "No supply recorded yet.\n";
  }

  // Newest first
  int shown = 0;

  for (
      int i =
          supplyHistory.count - 1;
      i >= 0 &&
      shown < HISTORY_SHOWN;
      i--, shown++
  ) {

    message +=
        "💧 " +
        supplyEventText(
            supplyHistory.events[i]
        ) +
        "\n";
  }

  // ----------------------------------------------------------
  // Tank fills
  // ----------------------------------------------------------

  message +=
      "\n🪣 TANK FILLS\n\n";

  if (
      fillHistory.count == 0
  ) {

    message +=
        "No fill recorded yet.\n";
  }

  for (
      int i =
          fillHistory.count - 1;
      i >= 0;
      i--
  ) {

    message +=
        "🔴 " +
        fillEventText(
            fillHistory.events[i]
        ) +
        "\n";
  }

  return message;
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

    startSupplyRecord();

    String message =
        "💧 Manjeera WATER DETECTED";

    if (timeValid()) {

      message +=
          "\nStarted at " +
          formatTime(
              time(nullptr),
              "%H:%M"
          );
    }

    sendTelegram(
        message,
        true
    );

  } else {

    addLog(
        "Water presence cleared"
    );

    SupplyEvent *event =
        endSupplyRecord();

    String message =
        "🔵 Manjeera WATER CLEARED";

    if (event != nullptr) {

      if (event->end != 0) {

        message +=
            "\nStopped at " +
            formatTime(
                event->end,
                "%H:%M"
            );
      }

      message +=
          "\nDuration: " +
          durationText(
              event->durationSec
          );
    }

    sendTelegram(
        message,
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

  int oldLevel =
      currentWaterLevel;

  currentWaterLevel =
      newLevel;

  addLog(
      "Water level changed: " +
      waterLevelText(
          newLevel
      )
  );

  // ----------------------------------------------------------
  // Fill tracking
  //
  // A fill starts on the first rise and is recorded when the
  // tank reaches FULL. Any drop cancels it.
  // ----------------------------------------------------------

  FillEvent *completedFill =
      nullptr;

  if (
      newLevel > oldLevel
  ) {

    if (!fillInProgress) {

      fillInProgress =
          true;

      fillFromLevel =
          oldLevel;

      fillStartMs =
          millis();

      addLog(
          "Fill started from " +
          fillLevelName(oldLevel)
      );
    }

    if (
        newLevel == 4
    ) {

      completedFill =
          recordFill(
              fillFromLevel,
              (millis() -
               fillStartMs) /
                  1000
          );

      fillInProgress =
          false;

      addLog(
          "Fill completed: " +
          fillEventText(
              *completedFill
          )
      );
    }

  } else if (
      newLevel < oldLevel &&
      fillInProgress
  ) {

    fillInProgress =
        false;

    addLog(
        "Fill cancelled (level dropped)"
    );
  }

  // ----------------------------------------------------------
  // Alert message
  // ----------------------------------------------------------

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

    if (completedFill != nullptr) {

      message +=
          "\nFilled " +
          fillLevelName(
              completedFill->fromLevel
          ) +
          " → FULL in " +
          durationText(
              completedFill->durationSec
          );
    }

  } else {

    message =
        "💧 Tank WATER LEVEL: " +
        String(newLevel) +
        "/4";
  }

  // ----------------------------------------------------------
  // FULL / EMPTY are important: send right away, ignoring
  // the cooldown. Other levels respect the cooldown.
  // Anything not sent is queued and retried, and a newer
  // level replaces an older queued one.
  // ----------------------------------------------------------

  bool important =
      newLevel == 0 ||
      newLevel == 4;

  if (
      sendTelegram(
          message,
          important
      )
  ) {

    levelAlertPending =
        false;

    return;
  }

  levelAlertPending =
      true;

  // Delayed alert: show when the change actually happened
  if (timeValid()) {

    message +=
        "\n(at " +
        formatTime(
            time(nullptr),
            "%H:%M"
        ) +
        ")";
  }

  pendingLevelAlert =
      message;

  lastLevelAlertAttempt =
      millis();

  addLog(
      "Level alert queued for retry"
  );
}

// ------------------------------------------------------------
// Retry a queued level alert once the cooldown has passed
// ------------------------------------------------------------

void flushPendingLevelAlert() {

  if (
      !levelAlertPending ||
      !wifiConnected
  ) {

    return;
  }

  if (
      millis() -
          lastLevelAlertAttempt <
      TELEGRAM_COOLDOWN
  ) {

    return;
  }

  if (
      lastTelegramSent != 0 &&
      millis() -
              lastTelegramSent <
          TELEGRAM_COOLDOWN
  ) {

    return;
  }

  lastLevelAlertAttempt =
      millis();

  if (
      sendTelegram(
          pendingLevelAlert,
          true
      )
  ) {

    levelAlertPending =
        false;

    addLog(
        "Queued level alert sent"
    );
  }
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
  // Overall level (percentage: each level = 25%)
  //
  // 0% 🔴  25% 🟠  50% 🟡  75% 🟢  100% 🔵
  // ----------------------------------------------------------

  const char *levelColours[5] = {
      "🔴",
      "🟠",
      "🟡",
      "🟢",
      "🔵"
  };

  int level =
      constrain(
          currentWaterLevel,
          0,
          4
      );

  message +=
      "Water Level: " +
      String(levelColours[level]) +
      " " +
      String(level * 25) +
      "%\n";

  // Tank gauge, e.g. 🟦🟦⬜⬜
  for (
      int i = 0;
      i < 4;
      i++
  ) {

    message +=
        i < level
            ? "🟦"
            : "⬜";
  }

  message += "\n\n";

  // ----------------------------------------------------------
  // Individual sensors, top of tank first
  // ----------------------------------------------------------

  for (
      int i = 3;
      i >= 0;
      i--
  ) {

    message +=
        "L" +
        String(i + 1) +
        " (" +
        String((i + 1) * 25) +
        "%): ";

    message +=
        confirmedLevelSensors[i]
            ? "🟢 WET"
            : "⚪ DRY";

    message += "\n";
  }

  return message;
}

// ============================================================
// TELEGRAM SYSTEM INFO (startup message)
// ============================================================

String buildSystemInfo() {

  String message = "";

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

    message += "\n";

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

  message +=
      "Time: " +
      nowText() +
      "\n";

  message +=
      "Uptime: " +
      getUptime() +
      "\n";

  return message;
}

// ============================================================
// DAILY SUMMARY
// ============================================================

String buildDailySummary() {

  time_t now =
      time(nullptr);

  uint32_t windowStart =
      (uint32_t)now -
      86400UL;

  String message =
      "☀️ DAILY SUMMARY - " +
      formatTime(
          now,
          "%d %b %Y"
      ) +
      "\n\n";

  message +=
      "Manjeera supply (last 24h):\n";

  int supplies = 0;

  uint32_t totalSec = 0;

  for (
      int i = 0;
      i < supplyHistory.count;
      i++
  ) {

    const SupplyEvent &event =
        supplyHistory.events[i];

    bool inWindow =
        event.open ||
        event.start >= windowStart ||
        event.end >= windowStart;

    if (!inWindow) {

      continue;
    }

    supplies++;

    totalSec +=
        event.open &&
                event.start != 0
            ? (uint32_t)now -
                  event.start
            : event.durationSec;

    message +=
        "💧 " +
        supplyEventText(event) +
        "\n";
  }

  if (supplies == 0) {

    message +=
        "No supply in the last 24 hours.\n";

  } else {

    message +=
        "Total: " +
        String(supplies) +
        (supplies == 1
             ? " supply, "
             : " supplies, ") +
        durationText(totalSec) +
        "\n";
  }

  // ----------------------------------------------------------
  // Tank fills in the last 24h
  // ----------------------------------------------------------

  message +=
      "\nTank fills (last 24h):\n";

  int fills = 0;

  for (
      int i = 0;
      i < fillHistory.count;
      i++
  ) {

    const FillEvent &fill =
        fillHistory.events[i];

    if (
        fill.end <
        windowStart
    ) {

      continue;
    }

    fills++;

    message +=
        "🔴 " +
        fillEventText(fill) +
        "\n";
  }

  if (fills == 0) {

    message +=
        "No fill in the last 24 hours.\n";
  }

  message +=
      "\nTank level: " +
      waterLevelText(
          currentWaterLevel
      ) +
      " (" +
      String(currentWaterLevel) +
      "/4)\n";

  message +=
      "Manjeera now: " +
      String(
          waterPresence
              ? "💧 WATER DETECTED"
              : "🔵 DRY"
      ) +
      "\n";

  message +=
      "Uptime: " +
      getUptime() +
      "\n";

  return message;
}

// ------------------------------------------------------------
// Send the summary once per day at DAILY_SUMMARY_HOUR.
// The last sent day is stored so a restart does not resend.
// ------------------------------------------------------------

void checkDailySummary() {

  static unsigned long lastCheck =
      0;

  if (
      millis() -
          lastCheck <
      30000UL
  ) {

    return;
  }

  lastCheck =
      millis();

  backfillSupplyStart();

  if (
      !timeValid() ||
      !wifiConnected
  ) {

    return;
  }

  time_t now =
      time(nullptr);

  struct tm timeInfo;

  localtime_r(
      &now,
      &timeInfo
  );

  if (
      timeInfo.tm_hour !=
      DAILY_SUMMARY_HOUR
  ) {

    return;
  }

  // Unique per day, e.g. 2026 * 1000 + day of year
  int32_t today =
      (timeInfo.tm_year + 1900) *
          1000 +
      timeInfo.tm_yday;

  if (
      preferences.getInt(
          "summaryDay",
          0
      ) == today
  ) {

    return;
  }

  if (
      sendTelegram(
          buildDailySummary(),
          true
      )
  ) {

    preferences.putInt(
        "summaryDay",
        today
    );

    addLog(
        "Daily summary sent"
    );
  }
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
// TELEGRAM JSON OBJECT SKIP
//
// Starting at a key, find its "{...}" value and return the
// index just after the matching '}'. Returns -1 on failure.
// ============================================================

int skipJsonObject(
    const String &json,
    int startAt
) {

  int braceStart =
      json.indexOf(
          '{',
          startAt
      );

  if (
      braceStart < 0
  ) {

    return -1;
  }

  int depth =
      0;

  bool inString =
      false;

  bool escaped =
      false;

  for (
      int i =
          braceStart;
      i < (int)json.length();
      i++
  ) {

    char c =
        json.charAt(i);

    if (
        inString
    ) {

      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
      }

      continue;
    }

    if (c == '"') {
      inString = true;
    } else if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;

      if (
          depth == 0
      ) {

        return i + 1;
      }
    }
  }

  return -1;
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
    //
    // When the user replies to a message, Telegram nests the
    // original message in "reply_to_message" before our own
    // "text". Skip that object so we read the reply's text.
    // --------------------------------------------------------

    int textSearchFrom =
        messagePosition;

    int nextUpdatePosition =
        response.indexOf(
            "\"update_id\"",
            updatePosition + 1
        );

    int replyPosition =
        response.indexOf(
            "\"reply_to_message\"",
            messagePosition
        );

    if (
        replyPosition >= 0 &&
        (
            nextUpdatePosition < 0 ||
            replyPosition <
                nextUpdatePosition
        )
    ) {

      int replyEnd =
          skipJsonObject(
              response,
              replyPosition
          );

      if (
          replyEnd > 0
      ) {

        textSearchFrom =
            replyEnd;
      }
    }

    String text =
        extractJsonString(
            response,
            "text",
            textSearchFrom
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
    // /history
    // --------------------------------------------------------

    else if (
        text == "/history" ||
        text.startsWith(
            "/history@"
        )
    ) {

      sendTelegram(
          buildHistoryMessage(),
          true
      );
    }

    // --------------------------------------------------------
    // /restart
    //
    // Restart happens after this poll, once the update has
    // been acknowledged, so it is not processed again on boot.
    // --------------------------------------------------------

    else if (
        text == "/restart" ||
        text.startsWith(
            "/restart@"
        )
    ) {

      restartRequested =
          true;
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

  // ----------------------------------------------------------
  // Telegram /restart
  // ----------------------------------------------------------

  if (
      restartRequested
  ) {

    restartRequested =
        false;

    // Acknowledge processed updates so /restart is not
    // received again after reboot (restart loop).
    String ackUrl =
        "https://api.telegram.org/bot" +
        String(TELEGRAM_BOT_TOKEN) +
        "/getUpdates"
        "?offset=" +
        String(
            telegramUpdateOffset
        ) +
        "&limit=1"
        "&timeout=0";

    String ackResponse;

    if (
        !telegramRequest(
            ackUrl,
            ackResponse
        )
    ) {

      addLog(
          "Restart cancelled: could not acknowledge Telegram update"
      );

      sendTelegram(
          "⚠️ Restart cancelled: Telegram acknowledge failed. Try again.",
          true
      );

      return;
    }

    addLog(
        "Restart requested via Telegram"
    );

    sendTelegram(
        "🔄 Restarting ESP32...",
        true
    );

    delay(1000);

    ESP.restart();
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

<meta charset="UTF-8">

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
        "💧 WATER DETECTED"
        "</span>";

  } else {

    html +=
        "<span class=\"blue\">"
        "🔵 DRY"
        "</span>";
  }

  html +=
      "<p style=\"font-size:14px;"
      "font-weight:normal;color:#666;\">"
      "Manjeera sensor (P25, P26)"
      "</p>";

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
Time:
<b>)rawliteral";

  html +=
      nowText();

  html +=
      R"rawliteral(
</b>
</p>

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

<h2>Manjeera Supply History</h2>
)rawliteral";

  if (
      supplyHistory.count == 0
  ) {

    html +=
        "<p>No supply recorded yet.</p>";

  } else {

    html +=
        "<ul style=\"padding-left:20px;line-height:1.7;\">";

    int shown = 0;

    for (
        int i =
            supplyHistory.count - 1;
        i >= 0 &&
        shown < HISTORY_SHOWN;
        i--, shown++
    ) {

      html +=
          "<li>" +
          supplyEventText(
              supplyHistory.events[i]
          ) +
          "</li>";
    }

    html +=
        "</ul>";
  }

  html +=
      "<h2>Tank Fills</h2>";

  if (
      fillHistory.count == 0
  ) {

    html +=
        "<p>No fill recorded yet.</p>";

  } else {

    html +=
        "<ul style=\"padding-left:20px;line-height:1.7;\">";

    for (
        int i =
            fillHistory.count - 1;
        i >= 0;
        i--
    ) {

      html +=
          "<li>" +
          fillEventText(
              fillHistory.events[i]
          ) +
          "</li>";
    }

    html +=
        "</ul>";
  }

  html +=
      R"rawliteral(
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

    feedWatchdog();

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
  // WATCHDOG
  //
  // The core already starts the task watchdog; reconfigure it
  // with a longer timeout and also watch the loop task.
  // ----------------------------------------------------------

  esp_task_wdt_config_t watchdogConfig = {
      .timeout_ms = WATCHDOG_TIMEOUT_MS,
      .idle_core_mask = 0,
      .trigger_panic = true
  };

  esp_task_wdt_reconfigure(
      &watchdogConfig
  );

  if (
      esp_task_wdt_add(NULL) ==
      ESP_OK
  ) {

    watchdogActive =
        true;

    addLog(
        "Watchdog enabled (" +
        String(WATCHDOG_TIMEOUT_MS / 1000) +
        "s)"
    );
  }

  esp_reset_reason_t resetReason =
      esp_reset_reason();

  bool watchdogReset =
      resetReason == ESP_RST_TASK_WDT ||
      resetReason == ESP_RST_INT_WDT ||
      resetReason == ESP_RST_WDT;

  if (watchdogReset) {

    addLog(
        "Previous restart was caused by the watchdog"
    );
  }

  // ----------------------------------------------------------
  // PREFERENCES
  // ----------------------------------------------------------

  preferences.begin(
      "watermon",
      false
  );

  loadSupplyHistory();

  loadFillHistory();

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
    // CLOCK (NTP)
    //
    // Wait up to 5 seconds so the startup message and first
    // logs have a real time. Sync continues in background.
    // --------------------------------------------------------

    configTzTime(
        TIMEZONE,
        NTP_SERVER_1,
        NTP_SERVER_2
    );

    unsigned long ntpStart =
        millis();

    while (
        !timeValid() &&
        millis() - ntpStart <
            5000
    ) {

      delay(100);
    }

    addLog(
        timeValid()
            ? "Clock synced: " +
                  nowText()
            : String(
                  "Clock not synced yet, will retry in background"
              )
    );

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
        "🟢 ESP32 Water Monitor started\n\n" +
        buildSystemInfo();

    if (watchdogReset) {

      startupMessage +=
          "\n⚠️ Restarted by watchdog (system was stuck)";
    }
      startupMessage += "\n Manjeera: (WATER_DRIVE_PIN - 25, WATER_SENSE_PIN - 26)\n";
      startupMessage += "\n LEVEL SENSORS (COMMON_PIN - 27, L1-32, L2-33, L3-16, L4-17 )\n";


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

  reconcileSupplyOnBoot();

  addLog(
      "System ready"
  );
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // ----------------------------------------------------------
  // WATCHDOG
  // ----------------------------------------------------------

  feedWatchdog();

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
  // DAILY SUMMARY
  // ----------------------------------------------------------

  checkDailySummary();

  // ----------------------------------------------------------
  // QUEUED LEVEL ALERTS
  // ----------------------------------------------------------

  flushPendingLevelAlert();

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
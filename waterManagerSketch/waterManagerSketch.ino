#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "config.h"

// ============================================================
// ESP32 Water Monitor - V3
//
// Features:
//   - Persistent Wi-Fi credentials
//   - Wi-Fi scanning
//   - Setup Access Point if Wi-Fi connection fails
//   - Web dashboard
//   - Wi-Fi configuration
//   - Browser OTA firmware update
//   - System logs
//   - Restart
//   - IPv6 address detection
//   - Automatic DuckDNS IPv6 update
//
// Secrets/configuration are stored in config.h
// config.h MUST NOT be committed to Git.
// ============================================================

WebServer server(80);
Preferences preferences;

// ============================================================
// DuckDNS
// ============================================================

const unsigned long DUCKDNS_UPDATE_INTERVAL =
  10UL * 60UL * 1000UL;   // 10 minutes

unsigned long lastDuckDNSUpdate = 0;

String lastDuckDNSIPv6 = "";
String duckDNSStatus = "Not updated yet";

// ============================================================
// Wi-Fi credentials
// ============================================================

String wifiSSID = "";
String wifiPassword = "";

bool setupMode = false;

// ============================================================
// Logs
// ============================================================

const int MAX_LOGS = 50;

String logs[MAX_LOGS];
int logCount = 0;

void addLog(String message) {

  String entry =
    "[" + String(millis() / 1000) + "s] " +
    message;

  Serial.println(entry);

  if (logCount < MAX_LOGS) {

    logs[logCount] = entry;
    logCount++;

  } else {

    for (int i = 0; i < MAX_LOGS - 1; i++) {
      logs[i] = logs[i + 1];
    }

    logs[MAX_LOGS - 1] = entry;
  }
}

// ============================================================
// Authentication
// ============================================================

bool authenticate() {

  if (!server.authenticate(
        WEB_USERNAME,
        WEB_PASSWORD
      )) {

    server.requestAuthentication();

    return false;
  }

  return true;
}

// ============================================================
// Load Wi-Fi credentials
// ============================================================

void loadWiFiCredentials() {

  preferences.begin("wifi", true);

  wifiSSID =
    preferences.getString("ssid", "");

  wifiPassword =
    preferences.getString("password", "");

  preferences.end();

  if (wifiSSID.length() > 0) {

    addLog(
      "Saved Wi-Fi credentials loaded"
    );

  } else {

    addLog(
      "No saved Wi-Fi credentials"
    );
  }
}

// ============================================================
// Save Wi-Fi credentials
// ============================================================

void saveWiFiCredentials(
  String ssid,
  String password
) {

  preferences.begin("wifi", false);

  preferences.putString(
    "ssid",
    ssid
  );

  preferences.putString(
    "password",
    password
  );

  preferences.end();

  wifiSSID = ssid;
  wifiPassword = password;

  addLog(
    "Wi-Fi credentials saved"
  );
}

// ============================================================
// Start setup Access Point
// ============================================================

void startSetupMode() {

  setupMode = true;

  WiFi.mode(WIFI_AP);

  WiFi.softAP(
    AP_SSID,
    AP_PASSWORD
  );

  IPAddress ip =
    WiFi.softAPIP();

  addLog(
    "Setup mode started"
  );

  addLog(
    "AP SSID: " +
    String(AP_SSID)
  );

  addLog(
    "AP IP: " +
    ip.toString()
  );
}

// ============================================================
// Get global IPv6
// ============================================================

String getIPv6Address() {

  if (setupMode) {
    return "Not available in setup mode";
  }

  if (!WiFi.STA.hasGlobalIPv6()) {
    return "No global IPv6";
  }

  IPAddress ipv6 =
    WiFi.STA.globalIPv6();

  return ipv6.toString();
}

// ============================================================
// Update DuckDNS
// ============================================================

bool updateDuckDNS() {

  if (WiFi.status() != WL_CONNECTED) {

    duckDNSStatus =
      "Wi-Fi not connected";

    addLog(
      "DuckDNS skipped: Wi-Fi not connected"
    );

    return false;
  }

  if (!WiFi.STA.hasGlobalIPv6()) {

    duckDNSStatus =
      "No global IPv6";

    addLog(
      "DuckDNS skipped: no global IPv6"
    );

    return false;
  }

  String ipv6 =
    WiFi.STA.globalIPv6().toString();

  // ----------------------------------------------------------
  // Don't update if address hasn't changed
  // ----------------------------------------------------------

  if (
    ipv6 == lastDuckDNSIPv6 &&
    lastDuckDNSUpdate != 0
  ) {

    duckDNSStatus =
      "IPv6 unchanged";

    addLog(
      "DuckDNS skipped: IPv6 unchanged"
    );

    return true;
  }

  // ----------------------------------------------------------
  // Build DuckDNS request
  // ----------------------------------------------------------

  String url =
    "https://www.duckdns.org/update"
    "?domains=" +
    String(DUCKDNS_DOMAIN) +
    "&token=" +
    String(DUCKDNS_TOKEN) +
    "&ipv6=" +
    ipv6 +
    "&verbose=true";

  addLog(
    "Updating DuckDNS..."
  );

  addLog(
    "IPv6: " + ipv6
  );

  // ----------------------------------------------------------
  // HTTPS
  // ----------------------------------------------------------

  WiFiClientSecure client;

  // HTTPS without storing a CA certificate.
  // Suitable for this initial DDNS implementation.
  client.setInsecure();

  HTTPClient http;

  http.setTimeout(15000);

  if (!http.begin(client, url)) {

    duckDNSStatus =
      "HTTPS connection failed";

    addLog(
      "DuckDNS HTTPS connection failed"
    );

    return false;
  }

  // ----------------------------------------------------------
  // Send request
  // ----------------------------------------------------------

  int httpCode =
    http.GET();

  if (httpCode <= 0) {

    duckDNSStatus =
      "HTTP error: " +
      String(httpCode);

    addLog(
      "DuckDNS HTTP error: " +
      String(httpCode)
    );

    http.end();

    return false;
  }

  // ----------------------------------------------------------
  // Read response
  // ----------------------------------------------------------

  String response =
    http.getString();

  response.trim();

  http.end();

  addLog(
    "DuckDNS response: " +
    response
  );

  // ----------------------------------------------------------
  // Check result
  // ----------------------------------------------------------

  if (response.startsWith("OK")) {

    lastDuckDNSIPv6 = ipv6;

    lastDuckDNSUpdate =
      millis();

    duckDNSStatus =
      "Updated successfully";

    addLog(
      "DuckDNS update successful"
    );

    return true;
  }

  duckDNSStatus =
    "Update failed: " +
    response;

  addLog(
    "DuckDNS update failed"
  );

  return false;
}

// ============================================================
// Connect to Wi-Fi
// ============================================================

bool connectToWiFi() {

  if (wifiSSID.length() == 0) {

    addLog(
      "No Wi-Fi configured"
    );

    return false;
  }

  setupMode = false;

  WiFi.mode(WIFI_STA);

  // Enable IPv6
  WiFi.STA.enableIPv6(true);

  WiFi.begin(
    wifiSSID.c_str(),
    wifiPassword.c_str()
  );

  addLog(
    "Connecting to Wi-Fi: " +
    wifiSSID
  );

  int attempts = 0;

  while (
    WiFi.status() != WL_CONNECTED &&
    attempts < 30
  ) {

    delay(500);

    Serial.print(".");

    attempts++;
  }

  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {

    addLog(
      "Wi-Fi connection failed"
    );

    return false;
  }

  addLog(
    "Wi-Fi connected"
  );

  addLog(
    "IP address: " +
    WiFi.localIP().toString()
  );

  // ----------------------------------------------------------
  // Wait for global IPv6
  // ----------------------------------------------------------

  int ipv6Attempts = 0;

  while (
    !WiFi.STA.hasGlobalIPv6() &&
    ipv6Attempts < 20
  ) {

    delay(500);

    ipv6Attempts++;
  }

  if (WiFi.STA.hasGlobalIPv6()) {

    addLog(
      "Global IPv6: " +
      WiFi.STA.globalIPv6().toString()
    );

  } else {

    addLog(
      "No global IPv6 address"
    );
  }

  addLog(
    "Signal: " +
    String(WiFi.RSSI()) +
    " dBm"
  );

  // ----------------------------------------------------------
  // Initial DuckDNS update
  // ----------------------------------------------------------

  updateDuckDNS();

  return true;
}

// ============================================================
// HTML helpers
// ============================================================

String getUptime() {

  unsigned long seconds =
    millis() / 1000;

  unsigned long days =
    seconds / 86400;

  seconds %= 86400;

  unsigned long hours =
    seconds / 3600;

  seconds %= 3600;

  unsigned long minutes =
    seconds / 60;

  seconds %= 60;

  return
    String(days) + "d " +
    String(hours) + "h " +
    String(minutes) + "m " +
    String(seconds) + "s";
}

String getIPAddress() {

  if (setupMode) {

    return WiFi.softAPIP().toString();

  }

  return WiFi.localIP().toString();
}

String getSSID() {

  if (setupMode) {

    return String(AP_SSID);

  }

  return WiFi.SSID();
}

String getRSSI() {

  if (setupMode) {

    return "Setup mode";

  }

  return String(WiFi.RSSI()) +
         " dBm";
}

// ============================================================
// Dashboard
// ============================================================

void handleRoot() {

  if (!authenticate()) {
    return;
  }

  String html = R"rawliteral(
<!DOCTYPE html>

<html>

<head>

<meta name="viewport"
      content="width=device-width, initial-scale=1">

<title>ESP32 Water Monitor</title>

<style>

body {
  font-family: Arial, sans-serif;
  margin: 0;
  padding: 20px;
  background: #f3f3f3;
}

.container {
  max-width: 850px;
  margin: auto;
}

.card {
  background: white;
  padding: 20px;
  margin-bottom: 15px;
  border-radius: 12px;
  box-shadow: 0 2px 8px rgba(0,0,0,0.08);
}

h1 {
  margin-top: 0;
}

.status {
  font-size: 20px;
  font-weight: bold;
}

.online {
  color: green;
}

.setup {
  color: orange;
}

.info {
  display: grid;
  grid-template-columns: 150px 1fr;
  gap: 8px;
}

.logs {
  background: #111;
  color: #00ff88;
  padding: 15px;
  border-radius: 8px;
  font-family: monospace;
  white-space: pre-wrap;
  max-height: 400px;
  overflow-y: auto;
}

button {
  padding: 11px 16px;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  font-size: 15px;
  margin: 4px;
}

.primary {
  background: #1976d2;
  color: white;
}

.danger {
  background: #d32f2f;
  color: white;
}

a {
  text-decoration: none;
}

</style>

</head>

<body>

<div class="container">

<h1>ESP32 Water Monitor</h1>

<div class="card">

<div class="status %STATUS_CLASS%">
%STATUS%
</div>

</div>

<div class="card">

<h2>Wi-Fi</h2>

<div class="info">

<b>SSID</b>
<span>%SSID%</span>

<b>IPv4 Address</b>
<span>%IP%</span>

<b>IPv6 Address</b>
<span>%IPV6%</span>

<b>Signal</b>
<span>%RSSI%</span>

</div>

<br>

<a href="/wifi">
<button class="primary">
Wi-Fi Settings
</button>
</a>

</div>

<div class="card">

<h2>Internet / DDNS</h2>

<div class="info">

<b>Hostname</b>
<span>watermanager.duckdns.org</span>

<b>DuckDNS Status</b>
<span>%DUCKDNS_STATUS%</span>

</div>

</div>

<div class="card">

<h2>System</h2>

<div class="info">

<b>Chip</b>
<span>ESP32</span>

<b>Uptime</b>
<span>%UPTIME%</span>

<b>Free Heap</b>
<span>%HEAP% bytes</span>

<b>Firmware</b>
<span>V3.0</span>

</div>

</div>

<div class="card">

<h2>Logs</h2>

<div class="logs">%LOGS%</div>

</div>

<div class="card">

<h2>Maintenance</h2>

<a href="/update">
<button class="primary">
OTA Firmware Update
</button>
</a>

<button class="danger"
        onclick="restartESP()">
Restart ESP32
</button>

</div>

</div>

<script>

function restartESP() {

  if (confirm("Restart ESP32?")) {

    fetch("/restart");

    alert("ESP32 is restarting...");

  }

}

</script>

</body>

</html>
)rawliteral";

  String status;
  String statusClass;

  if (setupMode) {

    status = "🟠 SETUP MODE";
    statusClass = "setup";

  } else if (
    WiFi.status() == WL_CONNECTED
  ) {

    status = "🟢 ESP32 ONLINE";
    statusClass = "online";

  } else {

    status = "🔴 OFFLINE";
    statusClass = "setup";
  }

  html.replace(
    "%STATUS%",
    status
  );

  html.replace(
    "%STATUS_CLASS%",
    statusClass
  );

  html.replace(
    "%SSID%",
    getSSID()
  );

  html.replace(
    "%IP%",
    getIPAddress()
  );

  html.replace(
    "%IPV6%",
    getIPv6Address()
  );

  html.replace(
    "%RSSI%",
    getRSSI()
  );

  html.replace(
    "%DUCKDNS_STATUS%",
    duckDNSStatus
  );

  html.replace(
    "%UPTIME%",
    getUptime()
  );

  html.replace(
    "%HEAP%",
    String(ESP.getFreeHeap())
  );

  String logText;

  for (
    int i = 0;
    i < logCount;
    i++
  ) {

    logText += logs[i];

    if (i < logCount - 1) {
      logText += "\n";
    }
  }

  html.replace(
    "%LOGS%",
    logText
  );

  server.send(
    200,
    "text/html",
    html
  );
}

// ============================================================
// Wi-Fi settings page
// ============================================================

void handleWiFiPage() {

  if (!authenticate()) {
    return;
  }

  int networkCount =
    WiFi.scanNetworks();

  String html = R"rawliteral(
<!DOCTYPE html>

<html>

<head>

<meta name="viewport"
      content="width=device-width, initial-scale=1">

<title>Wi-Fi Settings</title>

<style>

body {
  font-family: Arial;
  margin: 20px;
  background: #f3f3f3;
}

.container {
  max-width: 700px;
  margin: auto;
}

.card {
  background: white;
  padding: 20px;
  border-radius: 12px;
}

input, select {
  width: 100%;
  padding: 12px;
  margin: 8px 0 15px;
  box-sizing: border-box;
  font-size: 16px;
}

button {
  padding: 12px 18px;
  border: none;
  border-radius: 8px;
  background: #1976d2;
  color: white;
  font-size: 16px;
}

</style>

</head>

<body>

<div class="container">

<div class="card">

<h2>Wi-Fi Settings</h2>

<form method="POST"
      action="/savewifi">

<label>Wi-Fi Network</label>

<select name="ssid">

<option value="">
-- Select Network --
</option>

%NETWORKS%

</select>

<label>Password</label>

<input
  type="password"
  name="password"
  placeholder="Wi-Fi password">

<button type="submit">
Save & Connect
</button>

</form>

<br>

<a href="/">
Back to Dashboard
</a>

</div>

</div>

</body>

</html>
)rawliteral";

  String networks;

  for (
    int i = 0;
    i < networkCount;
    i++
  ) {

    String ssid =
      WiFi.SSID(i);

    if (ssid.length() == 0) {
      continue;
    }

    networks +=
      "<option value=\"";

    networks += ssid;

    networks +=
      "\">";

    networks += ssid;

    networks += " (";

    networks +=
      String(WiFi.RSSI(i));

    networks +=
      " dBm)";

    networks +=
      "</option>";
  }

  html.replace(
    "%NETWORKS%",
    networks
  );

  WiFi.scanDelete();

  server.send(
    200,
    "text/html",
    html
  );
}

// ============================================================
// Save Wi-Fi
// ============================================================

void handleSaveWiFi() {

  if (!authenticate()) {
    return;
  }

  String ssid =
    server.arg("ssid");

  String password =
    server.arg("password");

  if (ssid.length() == 0) {

    server.send(
      400,
      "text/plain",
      "SSID cannot be empty"
    );

    return;
  }

  saveWiFiCredentials(
    ssid,
    password
  );

  String html = R"rawliteral(
<!DOCTYPE html>

<html>

<head>

<meta name="viewport"
      content="width=device-width, initial-scale=1">

<title>Wi-Fi</title>

</head>

<body>

<h2>Wi-Fi saved</h2>

<p>
ESP32 will restart and attempt to connect.
</p>

</body>

</html>
)rawliteral";

  server.send(
    200,
    "text/html",
    html
  );

  delay(1500);

  ESP.restart();
}

// ============================================================
// Restart
// ============================================================

void handleRestart() {

  if (!authenticate()) {
    return;
  }

  server.send(
    200,
    "text/plain",
    "Restarting..."
  );

  delay(500);

  ESP.restart();
}

// ============================================================
// OTA page
// ============================================================

void handleUpdatePage() {

  if (!authenticate()) {
    return;
  }

  String html = R"rawliteral(
<!DOCTYPE html>

<html>

<head>

<meta name="viewport"
      content="width=device-width, initial-scale=1">

<title>OTA Update</title>

<style>

body {
  font-family: Arial;
  margin: 20px;
  background: #f3f3f3;
}

.container {
  max-width: 600px;
  margin: auto;
}

.card {
  background: white;
  padding: 20px;
  border-radius: 12px;
}

input {
  width: 100%;
  margin: 15px 0;
}

button {
  padding: 12px 20px;
  background: #1976d2;
  color: white;
  border: none;
  border-radius: 8px;
}

</style>

</head>

<body>

<div class="container">

<div class="card">

<h2>OTA Firmware Update</h2>

<p>
Select the compiled ESP32 firmware (.bin)
</p>

<form
  method="POST"
  action="/update"
  enctype="multipart/form-data">

<input
  type="file"
  name="firmware"
  accept=".bin"
  required>

<br>

<button type="submit">
Upload Firmware
</button>

</form>

<br>

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
// OTA upload handler
// ============================================================

void handleUpdateUpload() {

  HTTPUpload& upload =
    server.upload();

  if (
    upload.status ==
    UPLOAD_FILE_START
  ) {

    if (!authenticate()) {
      return;
    }

    addLog(
      "OTA update started: " +
      upload.filename
    );

    if (
      !Update.begin(
        UPDATE_SIZE_UNKNOWN
      )
    ) {

      Update.printError(Serial);

      addLog(
        "OTA begin failed"
      );
    }
  }

  else if (
    upload.status ==
    UPLOAD_FILE_WRITE
  ) {

    if (
      Update.write(
        upload.buf,
        upload.currentSize
      ) != upload.currentSize
    ) {

      Update.printError(Serial);
    }
  }

  else if (
    upload.status ==
    UPLOAD_FILE_END
  ) {

    if (
      Update.end(true)
    ) {

      addLog(
        "OTA update successful"
      );

    } else {

      Update.printError(Serial);

      addLog(
        "OTA update failed"
      );
    }
  }
}

// ============================================================
// OTA result
// ============================================================

void handleUpdateResult() {

  if (!authenticate()) {
    return;
  }

  if (Update.hasError()) {

    server.send(
      500,
      "text/plain",
      "OTA update failed"
    );

  } else {

    server.send(
      200,
      "text/html",
      "<h2>Update successful</h2>"
      "<p>ESP32 is restarting...</p>"
    );

    delay(1000);

    ESP.restart();
  }
}

// ============================================================
// Setup
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  addLog(
    "================================"
  );

  addLog(
    "ESP32 Water Monitor V3"
  );

  addLog(
    "Booting..."
  );

  addLog(
    "================================"
  );

  // ----------------------------------------------------------
  // Load saved Wi-Fi
  // ----------------------------------------------------------

  loadWiFiCredentials();

  // ----------------------------------------------------------
  // Connect to Wi-Fi
  // ----------------------------------------------------------

  bool connected =
    connectToWiFi();

  // ----------------------------------------------------------
  // Start setup AP if connection failed
  // ----------------------------------------------------------

  if (!connected) {

    startSetupMode();
  }

  // ----------------------------------------------------------
  // Web routes
  // ----------------------------------------------------------

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
    "/update",
    HTTP_GET,
    handleUpdatePage
  );

  server.on(
    "/update",
    HTTP_POST,
    handleUpdateResult,
    handleUpdateUpload
  );

  server.begin();

  addLog(
    "Web server started"
  );

  // ----------------------------------------------------------
  // Startup information
  // ----------------------------------------------------------

  if (setupMode) {

    addLog(
      "Connect to Wi-Fi: " +
      String(AP_SSID)
    );

    addLog(
      "Setup IP: " +
      WiFi.softAPIP().toString()
    );

  } else {

    addLog(
      "Dashboard: http://" +
      WiFi.localIP().toString()
    );

    addLog(
      "IPv6: " +
      getIPv6Address()
    );

    addLog(
      "DuckDNS: watermanager.duckdns.org"
    );
  }
}

// ============================================================
// Main loop
// ============================================================

void loop() {

  server.handleClient();

  // ----------------------------------------------------------
  // Periodic DuckDNS update
  // ----------------------------------------------------------

  if (
    WiFi.status() == WL_CONNECTED &&
    !setupMode
  ) {

    unsigned long now =
      millis();

    if (
      lastDuckDNSUpdate == 0 ||
      now - lastDuckDNSUpdate >=
        DUCKDNS_UPDATE_INTERVAL
    ) {

      updateDuckDNS();
    }
  }

  delay(2);
}
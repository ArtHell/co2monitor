#include <DFRobot_SCD4X.h>
#include <WiFiS3.h>
#include <ArduinoMDNS.h>
#include <EEPROM.h>

struct WifiCredentials
{
  char tag[4];
  char ssid[32];
  char pass[64];
  uint8_t crc;
};

const int EEPROM_ADDR = 0;
WifiCredentials creds;


bool loadCredentials();
void saveCredentials(const char *ssid, const char *pass);
uint8_t calcCRC(const WifiCredentials &c);
bool connectToWiFi(unsigned long timeoutMs = 15000);
void startConfigPortal();
void handleClient();
void serveMainPage(WiFiClient &client);
void serveConfigPage(WiFiClient &client, const char *message = nullptr);
void processSaveRequest(WiFiClient &client, const String &req);

bool deferredConnectRequested = false;
unsigned long deferredRequestTime = 0;
char lastErrorMsg[64] = {0};

enum RunMode
{
  MODE_NORMAL,
  MODE_CONFIG
};
RunMode currentMode = MODE_CONFIG;

DFRobot_SCD4X SCD4X(&Wire, SCD4X_I2C_ADDR);
WiFiServer server(80);
WiFiUDP udp;
MDNS mdns(udp);
WiFiUDP dnsUDP;
IPAddress apIP;

unsigned long lastWiFiCheck = 0;
unsigned long lastDataTime = 0;
const unsigned long dataInterval = 5500;
const unsigned long sensorTimeout = 20000;

unsigned long co2_ppm = 0;

void sendJson(WiFiClient &client, const String &json)
{
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Connection: close");
  client.println();
  client.println(json);
  client.stop();
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("--- CO2 Monitor Boot ---");

  while (!SCD4X.begin())
  {
    Serial.println("Communication with device failed, please check connection");
    delay(3000);
  }
  Serial.println("SCD4X initialized");

  SCD4X.enablePeriodMeasure(SCD4X_STOP_PERIODIC_MEASURE);
  delay(500);
  SCD4X.setTempComp(4.0);
  SCD4X.setSensorAltitude(110);
  SCD4X.enablePeriodMeasure(SCD4X_START_PERIODIC_MEASURE);

  bool haveCreds = loadCredentials();
  if (haveCreds)
  {
    Serial.print("Loaded credentials for SSID: ");
    Serial.println(creds.ssid);
    if (connectToWiFi())
    {
      currentMode = MODE_NORMAL;
      server.begin();
      Serial.println("Server started (normal mode)");
      delay(400);
      if (mdns.begin(WiFi.localIP(), "co2")) {
        mdns.addServiceRecord("CO2._http", 80, MDNSServiceTCP);
        Serial.println("mDNS: http://co2.local");
      } else {
        Serial.println("mDNS setup failed");
      }
    }
    else
    {
      Serial.println("Connection failed, starting config portal");
      startConfigPortal();
    }
  }
  else
  {
    Serial.println("No stored credentials, starting config portal");
    startConfigPortal();
  }
}

void loop()
{
  unsigned long now = millis();

  if (currentMode == MODE_NORMAL && now - lastWiFiCheck > 5000)
  {
    lastWiFiCheck = now;
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("WiFi disconnected! Attempting reconnection...");
      WiFi.disconnect();
      delay(150);
      WiFi.begin(creds.ssid, creds.pass);
    }
  }

  if (now - lastDataTime > dataInterval)
  {
    if (SCD4X.getDataReadyStatus())
    {
      DFRobot_SCD4X::sSensorMeasurement_t data;
      SCD4X.readMeasurement(&data);
      co2_ppm = data.CO2ppm;
      Serial.print("CO2: ");
      Serial.println(co2_ppm);
      lastDataTime = now;
    }
    else if (millis() - lastDataTime > 20000)
    {
      Serial.println("Sensor not responding, restarting measurement...");
      SCD4X.enablePeriodMeasure(SCD4X_STOP_PERIODIC_MEASURE);
      delay(500);
      SCD4X.moduleReinit();
      delay(100);
      SCD4X.enablePeriodMeasure(SCD4X_START_PERIODIC_MEASURE);
      lastDataTime = millis();
    }
  }

  if (currentMode == MODE_NORMAL) {
    mdns.run();
  }

  handleClient();

  if (currentMode == MODE_CONFIG && deferredConnectRequested && (millis() - deferredRequestTime) > 400)
  {
    deferredConnectRequested = false;
    Serial.println("[Deferred] Connecting with saved credentials...");
    if (connectToWiFi())
    {
      currentMode = MODE_NORMAL;
      server.begin();
      delay(300);
      if (mdns.begin(WiFi.localIP(), "co2"))
      {
        mdns.addServiceRecord("CO2._http", 80, MDNSServiceTCP);
        Serial.println("mDNS: http://co2.local");
      }
      else
      {
        Serial.println("mDNS setup failed");
      }
      Serial.println("Switched to NORMAL mode (deferred)");
    }
    else
    {
      Serial.println("Deferred connection failed");
      strncpy(lastErrorMsg, "Failed to connect. Check SSID/password.", sizeof(lastErrorMsg)-1);
      if (currentMode != MODE_CONFIG) {
        startConfigPortal();
      } else {
        WiFi.disconnect();
        delay(150);
        startConfigPortal();
      }
    }
  }
}

uint8_t calcCRC(const WifiCredentials &c)
{
  uint8_t sum = 0;
  for (size_t i = 0; i < sizeof(c.tag); i++)
    sum += c.tag[i];
  for (size_t i = 0; i < sizeof(c.ssid); i++)
    sum += c.ssid[i];
  for (size_t i = 0; i < sizeof(c.pass); i++)
    sum += c.pass[i];
  return sum;
}

bool loadCredentials()
{
  EEPROM.get(EEPROM_ADDR, creds);
  if (strncmp(creds.tag, "CFG", 3) != 0)
    return false;
  uint8_t crc = calcCRC(creds);
  return crc == creds.crc && creds.ssid[0] != '\0';
}

void saveCredentials(const char *ssid, const char *pass)
{
  memset(&creds, 0, sizeof(creds));
  strncpy(creds.tag, "CFG", 4);
  strncpy(creds.ssid, ssid, sizeof(creds.ssid) - 1);
  strncpy(creds.pass, pass, sizeof(creds.pass) - 1);
  creds.crc = calcCRC(creds);
  EEPROM.put(EEPROM_ADDR, creds);
#ifdef ESP_PLATFORM
  EEPROM.commit();
#endif
  Serial.println("Credentials saved to EEPROM");
}

bool connectToWiFi(unsigned long timeoutMs)
{
  Serial.print("Connecting to WiFi SSID='");
  Serial.print(creds.ssid);
  Serial.print("' ...");
  WiFi.disconnect();
  delay(100);
  WiFi.begin(creds.ssid, creds.pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs)
  {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\nConnected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("\nFailed to connect");
  return false;
}

void startConfigPortal()
{
  currentMode = MODE_CONFIG;
  WiFi.disconnect();
  delay(200);
  const char *apName = "CO2Config";
  Serial.print("Starting AP: ");
  Serial.println(apName);
  if (WiFi.beginAP(apName) != WL_AP_LISTENING)
  {
    Serial.println("AP start failed");
  }
  else
  {
    Serial.print("AP IP: ");
    Serial.println(WiFi.localIP());
    apIP = WiFi.localIP();
  }

  server.begin();
  Serial.println("Server started (config mode)");
  if (dnsUDP.begin(53)) {
    Serial.println("Captive DNS started on port 53");
  } else {
    Serial.println("Failed to start captive DNS");
  }
}

void handleClient()
{
  if (currentMode == MODE_CONFIG) {
    int packetSize = dnsUDP.parsePacket();
    if (packetSize > 0 && packetSize <= 512) {
      uint8_t buffer[512];
      int len = dnsUDP.read(buffer, sizeof(buffer));
      if (len >= 12) {
        buffer[2] |= 0x80;
        buffer[2] |= 0x04;
        buffer[3] &= 0xF0;

        uint16_t qdCount = (buffer[4] << 8) | buffer[5];
        buffer[6] = buffer[4];
        buffer[7] = buffer[5];
        buffer[8] = 0; buffer[9] = 0;
        buffer[10] = 0; buffer[11] = 0;

        int idx = 12;
        for (uint16_t q = 0; q < qdCount && idx < len; q++) {

          while (idx < len && buffer[idx] != 0) idx += buffer[idx] + 1;
          if (idx < len) idx++;
          idx += 4;
        }
        if (idx + 16 < (int)sizeof(buffer)) {

          int ansStart = idx;
          buffer[idx++] = 0xC0; buffer[idx++] = 0x0C;
          buffer[idx++] = 0x00; buffer[idx++] = 0x01;
          buffer[idx++] = 0x00; buffer[idx++] = 0x01;
          buffer[idx++] = 0x00; buffer[idx++] = 0x00; buffer[idx++] = 0x00; buffer[idx++] = 0x3C;
          buffer[idx++] = 0x00; buffer[idx++] = 0x04;
          buffer[idx++] = apIP[0];
          buffer[idx++] = apIP[1];
            buffer[idx++] = apIP[2];
          buffer[idx++] = apIP[3];
          int outLen = idx;
          dnsUDP.beginPacket(dnsUDP.remoteIP(), dnsUDP.remotePort());
          dnsUDP.write(buffer, outLen);
          dnsUDP.endPacket();
        }
      }
    }
  }

  WiFiClient client = server.available();
  if (!client)
    return;

  unsigned long t0 = millis();
  while (!client.available() && millis() - t0 < 1000)
    delay(1);
  if (!client.available())
  {
    client.stop();
    return;
  }

  String req;
  while (client.available())
  {
    char c = client.read();
    req += c;
    if (req.length() > 2048)
      break;
  }

  int getIdx = req.indexOf("GET ");
  String path = "/";
  if (getIdx != -1)
  {
    int pathStart = getIdx + 4;
    int pathEnd = req.indexOf(' ', pathStart);
    if (pathEnd != -1)
      path = req.substring(pathStart, pathEnd);
  }

  if (currentMode == MODE_CONFIG)
  {
    if (path == "/status")
    {
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Cache-Control: no-store");
      client.println("Connection: close");
      client.println();
      if (currentMode == MODE_NORMAL)
      {
        client.print("{\"mode\":\"normal\",\"ip\":\""); client.print(WiFi.localIP()); client.println("\"}");
      }
      else
      {
        client.print("{\"mode\":\"config\"");
        if (lastErrorMsg[0]) { client.print(",\"error\":\""); client.print(lastErrorMsg); client.print("\""); }
        client.println("}");
      }
      client.stop();
      return;
    }

    const char *redirectHosts[] = {
      "connectivitycheck.gstatic.com", "clients3.google.com", "captive.apple.com",
      "www.msftconnecttest.com", "www.msftncsi.com"
    };
    bool forceRedirect = false;
    int hIdx = req.indexOf("Host:");
    if (hIdx != -1) {
      int lineEnd = req.indexOf('\r', hIdx);
      String hostLine = req.substring(hIdx, lineEnd > hIdx ? lineEnd : req.length());
      hostLine.toLowerCase();
      for (auto h : redirectHosts) {
        if (hostLine.indexOf(h) != -1) { forceRedirect = true; break; }
      }
    }
    if (path.indexOf("generate_204") != -1 || path.indexOf("hotspot-detect") != -1) forceRedirect = true;
    if (forceRedirect && !path.startsWith("/save")) {
      client.println("HTTP/1.1 302 Found");
      client.print("Location: http://"); client.print(apIP); client.println("/");
      client.println("Cache-Control: no-store");
      client.println("Connection: close");
      client.println();
      client.stop();
      return;
    }
    if (path.startsWith("/save"))
    {
      processSaveRequest(client, req);
      return;
    }
    serveConfigPage(client);
    return;
  }

  if (path == "/status") {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/json");
    client.println("Cache-Control: no-store");
    client.println("Connection: close");
    client.println();
    client.print("{\"mode\":\"normal\",\"ip\":\""); client.print(WiFi.localIP()); client.println("\"}");
    client.stop();
    return;
  }
  if (path == "/data")
  {
    String json = "{\"co2\":" + String(co2_ppm) + "}";
    sendJson(client, json);
  }
  else if (path == "/" || path.startsWith("/index"))
  {
    serveMainPage(client);
  }
  else
  {
    client.println("HTTP/1.1 404 Not Found");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    client.println("404 Not Found");
    client.stop();
  }
}

String urlDecode(const String &s)
{
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i)
  {
    char c = s[i];
    if (c == '+')
      out += ' ';
    else if (c == '%' && i + 2 < s.length())
    {
      char h1 = s[i + 1];
      char h2 = s[i + 2];
      auto hexVal = [](char h) -> int
      { if (h>='0'&&h<='9') return h-'0'; if (h>='A'&&h<='F') return h-'A'+10; if (h>='a'&&h<='f') return h-'a'+10; return 0; };
      out += char((hexVal(h1) << 4) | hexVal(h2));
      i += 2;
    }
    else
      out += c;
  }
  return out;
}

void processSaveRequest(WiFiClient &client, const String &req)
{
  int qIdx = req.indexOf("/save?");
  String query;
  if (qIdx != -1)
  {
    int lineEnd = req.indexOf(' ', qIdx);
    if (lineEnd != -1)
      query = req.substring(qIdx + 6, lineEnd);
  }
  int qm = query.indexOf('?');
  if (qm != -1)
    query = query.substring(qm + 1);

  String ssidVal, passVal;
  int last = 0;
  while (last < (int)query.length())
  {
    int amp = query.indexOf('&', last);
    if (amp == -1)
      amp = query.length();
    String pair = query.substring(last, amp);
    int eq = pair.indexOf('=');
    if (eq != -1)
    {
      String key = pair.substring(0, eq);
      String val = pair.substring(eq + 1);
      val = urlDecode(val);
      if (key == "ssid")
        ssidVal = val;
      else if (key == "pass")
        passVal = val;
    }
    last = amp + 1;
  }

  if (ssidVal.length() == 0)
  {
    serveConfigPage(client, "SSID required");
    return;
  }

  saveCredentials(ssidVal.c_str(), passVal.c_str());
  lastErrorMsg[0] = '\0';
  deferredConnectRequested = true;
  deferredRequestTime = millis();

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'><title>Connecting...</title><meta name='viewport' content='width=device-width,initial-scale=1'><style>body{font-family:sans-serif;max-width:480px;margin:2rem auto;padding:1rem;background:#f5f5f7;color:#222}code{background:#eee;padding:2px 4px;border-radius:4px}#err{color:#b91c1c;margin-top:1rem}</style></head><body>");
  client.println("<h2>Attempting Connection</h2><p>Saved credentials for SSID: <code>" + ssidVal + "</code></p><p>The device will temporarily leave the setup AP while testing these credentials. If they are wrong, the AP will reappear in ~10s and you'll return to the setup page with an error.</p><div id='status'>Connecting...</div><div id='err'></div><script>function poll(){fetch('/status').then(r=>r.json()).then(j=>{if(j.mode==='normal'){document.getElementById('status').innerHTML='Connected. <a href=\\'http://co2.local/\\'>Open dashboard</a>'; }else if(j.error){document.getElementById('status').innerText='Not connected';document.getElementById('err').innerText=j.error;}}).catch(()=>{}).finally(()=>{setTimeout(poll,2000);});}poll();</script><p><a href='/'>Back</a></p></body></html>");
  client.stop();
}

void serveConfigPage(WiFiClient &client, const char *message)
{
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'><title>CO2 WiFi Setup</title><meta name='viewport' content='width=device-width,initial-scale=1'>");
  client.println("<style>body{font-family:sans-serif;max-width:420px;margin:2rem auto;padding:1rem;background:#f5f5f7;color:#222}h1{font-size:1.4rem}form{display:flex;flex-direction:column;gap:.75rem}input{padding:.6rem;border:1px solid #bbb;border-radius:6px;font-size:1rem}button{padding:.75rem;background:#2563EB;color:#fff;border:0;border-radius:6px;font-size:1rem;cursor:pointer}button:hover{background:#1d4ed8}.msg{padding:.6rem;border-radius:6px;margin-bottom:.75rem}.err{background:#fee2e2;color:#b91c1c}.ok{background:#dcfce7;color:#166534}footer{margin-top:2rem;font-size:.75rem;color:#666}</style></head><body>");
  client.println("<h1>CO₂ Monitor WiFi Setup</h1>");
  client.print("<div style='background:#fff;border:1px solid #ddd;padding:.65rem .75rem;border-radius:6px;font-size:.8rem;margin:.5rem 0'>Connect to WiFi AP <b>CO2Config</b> then open <b>http://");
  client.print(WiFi.localIP());
  client.println("/</b><br>If mDNS is supported after connecting you'll be able to use <code>http://co2.local/</code>. Enter your WiFi credentials below.</div>");
  if ((message && *message) || lastErrorMsg[0])
  {
    client.print("<div class='msg err'>");
    if (message && *message) client.print(message); else client.print(lastErrorMsg);
    client.println("</div>");
  }
  client.println("<form method='GET' action='/save'>");
  client.print("<input name='ssid' placeholder='WiFi SSID' maxlength='31' required value='");
  if (strncmp(creds.tag, "CFG", 3) == 0)
    client.print(creds.ssid);
  client.println("'>");
  client.println("<input name='pass' placeholder='WiFi Password' maxlength='63' type='password'>");
  client.println("<button type='submit'>Save & Connect</button>");
  client.println("</form>");
  client.println("<footer>Device in config mode. Provide network credentials to continue.</footer>");
  client.println("</body></html>");
  client.stop();
}

void serveMainPage(WiFiClient &client)
{
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html>");
  client.println("<html lang=\"en\">");
  client.println("<head>");
  client.println("  <meta charset=\"UTF-8\">");
  client.println("  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">");
  client.println("  <title>CO₂ Monitor Dashboard</title>");
  client.println("  <script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script>");
  client.println("  <script src=\"https://cdn.tailwindcss.com\"></script>");
  client.println("  <style>");
  client.println("    body {");
  client.println("      background-color: #f4f6f8;");
  client.println("      font-family: 'Inter', sans-serif;");
  client.println("    }");
  client.println("    .card {");
  client.println("      border-radius: 1rem;");
  client.println("      box-shadow: 0 4px 20px rgba(0,0,0,0.1);");
  client.println("      background-color: #fff;");
  client.println("      padding: 1.5rem;");
  client.println("    }");
  client.println("@media (max-width: 640px) {");
  client.println("      .card {");
  client.println("        padding: 1rem;");
  client.println("      }");
  client.println("      .max-w-3xl {");
  client.println("        max-width: 100vw;");
  client.println("      }");
  client.println("      #co2Value {");
  client.println("        font-size: 2.5rem;");
  client.println("      }");
  client.println("    }");
  client.println("  </style>");
  client.println("</head>");
  client.println("<body class=\"flex items-center justify-center min-h-screen\">");
  client.println("  <div class=\"w-full max-w-3xl space-y-6 px-2 sm:px-0\">");
  client.println("    <div class=\"card text-center\">");
  client.println("      <h1 class=\"text-2xl font-bold text-gray-800 mb-2\">CO₂ Monitor</h1>");
  client.println("      <p class=\"text-gray-500\">Real-Time Indoor CO₂ Levels</p>");
  client.println("      <div class=\"mt-4\">");
  client.println("        <span class=\"text-5xl font-extrabold text-blue-600\" id=\"co2Value\">--</span>");
  client.println("        <span class=\"text-xl text-gray-600\">ppm</span>");
  client.println("      </div>");
  client.println("        <div class=\"mt-2\">");
  client.println("          <span id=\"statusIndicator\" class=\"text-sm font-semibold text-yellow-500\">Connecting...</span>");
  client.println("        </div>");
  client.println("    </div>");
  client.println("    <div class=\"card\">");
  client.println("      <canvas id=\"co2Chart\" height=\"300\"></canvas>");
  client.println("    </div>");
  client.println("  </div>");
  client.println("  <script>");
  client.println("  let lastMinuteLabel = \"\";");
  client.println("    const ctx = document.getElementById('co2Chart').getContext('2d');");
  client.println("    const fullTimes = [];");
  client.println("    const co2Chart = new Chart(ctx, {");
  client.println("      type: 'line',");
  client.println("      data: {");
  client.println("        labels: [],");
  client.println("        datasets: [{");
  client.println("          label: 'CO₂ (ppm)',");
  client.println("          data: [],");
  client.println("          borderColor: '#2563EB',");
  client.println("          backgroundColor: 'rgba(37, 99, 235, 0.1)',");
  client.println("          fill: true,");
  client.println("          tension: 0.3,");
  client.println("          pointRadius: 3,");
  client.println("          pointBackgroundColor: '#2563EB'");
  client.println("        }]");
  client.println("      },");
  client.println("      options: {");
  client.println("        responsive: true,");
  client.println("        maintainAspectRatio: false,");
  client.println("        animation: { duration: 500 },");
  client.println("        plugins: {");
  client.println("          legend: { display: false },");
  client.println("          tooltip: { callbacks: { title: function(context){ const idx=context[0].dataIndex; return fullTimes[idx] || ''; } } }");
  client.println("        },");
  client.println("        scales: { x: { title: { display: true, text: 'Time', color: '#374151', font: { weight: 'bold' } } }, y: { beginAtZero: true, title: { display: true, text: 'CO₂ (ppm)', color: '#374151', font: { weight: 'bold' } } } }");
  client.println("      }");
  client.println("    });");
  client.println("    async function fetchData() {");
  client.println("      const statusEl = document.getElementById('statusIndicator');");
  client.println("      try {");
  client.println("        const response = await fetch('/data');");
  client.println("        if (!response.ok) throw new Error('HTTP '+response.status);");
  client.println("        const data = await response.json();");
  client.println("        if (data.co2 === 0) { statusEl.innerText='Connecting...'; statusEl.className='text-sm font-semibold text-yellow-500'; }");
  client.println("        else { statusEl.innerText='Connected'; statusEl.className='text-sm font-semibold text-green-600'; const now=new Date().toLocaleTimeString([], {hour12:false,hour:'2-digit',minute:'2-digit'}); const fullTime=new Date().toLocaleTimeString([], {hour12:false,hour:'2-digit',minute:'2-digit',second:'2-digit'}); document.getElementById('co2Value').innerText=data.co2; const labels=co2Chart.data.labels; let labelToAdd=''; const currMinute=now.split(':')[1]; if (lastMinuteLabel!==currMinute){ labelToAdd=now; lastMinuteLabel=currMinute;} labels.push(labelToAdd); co2Chart.data.datasets[0].data.push(data.co2); fullTimes.push(fullTime); if(labels.length>20){ labels.shift(); co2Chart.data.datasets[0].data.shift(); fullTimes.shift(); } co2Chart.update(); }");
  client.println("      } catch(e){ statusEl.innerText='Disconnected'; statusEl.className='text-sm font-semibold text-red-600'; }");
  client.println("    }");
  client.println("    setInterval(fetchData, 5500); fetchData();");
  client.println("  </script>");
  client.println("</body></html>");
  client.stop();
}

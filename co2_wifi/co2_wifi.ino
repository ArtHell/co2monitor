#include <DFRobot_SCD4X.h>
#include <WiFiS3.h>
#include <ArduinoMDNS.h>

// --- WiFi credentials ---
char ssid[] = "artheii";
char pass[] = "spasibonet";

// --- Sensor and server ---
DFRobot_SCD4X SCD4X(&Wire, SCD4X_I2C_ADDR);
WiFiServer server(80);
WiFiUDP udp;
MDNS mdns(udp);

// --- Timers ---
unsigned long lastWiFiCheck = 0;
unsigned long lastDataTime = 0;
const unsigned long dataInterval = 5500;   // 5.5s between measurements
const unsigned long sensorTimeout = 20000; // 20s to restart sensor if not ready

// --- Global CO2 value ---
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

  // --- Initialize sensor ---
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

  IPAddress ip(192, 168, 1, 123);
  IPAddress dns(192, 168, 1, 1);
  IPAddress gateway(192, 168, 1, 1);
  IPAddress subnet(255, 255, 255, 0);
  // WiFi.config(ip, dns, gateway, subnet);
  WiFi.begin(ssid, pass);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  server.begin();
  Serial.println("Server started");

  if (mdns.begin(WiFi.localIP(), "co2"))
  {
    mdns.addServiceRecord("CO2 Monitor._http", 80, MDNSServiceTCP);
    Serial.println("mDNS responder started: http://co2.local");
  }
  else
  {
    Serial.println("mDNS setup failed");
  }
}

void loop()
{
  unsigned long now = millis();

  // --- WiFi watchdog ---
  if (now - lastWiFiCheck > 5000)
  { // every 5s
    lastWiFiCheck = now;
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("WiFi disconnected! Attempting reconnection...");
      WiFi.disconnect();
      WiFi.begin(ssid, pass);
    }
  }

  // --- Sensor measurement ---
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

  // --- mDNS loop ---
  mdns.run();

  // --- HTTP server handling ---
  WiFiClient client = server.available();
  if (client)
  {
    // Wait for request to arrive
    unsigned long timeout = millis();
    while (!client.available() && millis() - timeout < 1000)
    {
      delay(1);
    }

    // Read request
    String req = "";
    while (client.available())
    {
      char c = client.read();
      req += c;
    }

    // Find requested path
    String path = "/";
    int getIdx = req.indexOf("GET ");
    if (getIdx != -1)
    {
      int pathStart = getIdx + 4;
      int pathEnd = req.indexOf(' ', pathStart);
      if (pathEnd != -1)
      {
        path = req.substring(pathStart, pathEnd);
      }
    }

    if (path == "/data")
    {
      String json = "{\"co2\":" + String(co2_ppm) + "}";
      sendJson(client, json);
    }
    else if (path == "/" || path.startsWith("/index.html"))
    {
      // Serve index.html in small chunks
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
      client.println("  // Track the last minute label shown");
      client.println("  let lastMinuteLabel = \"\";");
      client.println("    const ctx = document.getElementById('co2Chart').getContext('2d');");
      client.println("    // Store full time for each data point");
      client.println("    const fullTimes = [];");
      client.println("    const co2Chart = new Chart(ctx, {");
      client.println("      type: 'line',");
      client.println("      data: {");
      client.println("        labels: [],");
      client.println("        datasets: [{");
      client.println("          label: 'CO₂ (ppm)',");
      client.println("          data: [],");
      client.println("          borderColor: '#2563EB', // Tailwind blue-600");
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
      client.println("          tooltip: {");
      client.println("            callbacks: {");
      client.println("              title: function(context) {");
      client.println("                // Show full time in tooltip");
      client.println("                const idx = context[0].dataIndex;");
      client.println("                return fullTimes[idx] || \"\";");
      client.println("              }");
      client.println("            }");
      client.println("          }");
      client.println("        },");
      client.println("        scales: {");
      client.println("          x: { ");
      client.println("            title: { display: true, text: 'Time', color: '#374151', font: { weight: 'bold' } }");
      client.println("          },");
      client.println("          y: { ");
      client.println("            beginAtZero: true,");
      client.println("            title: { display: true, text: 'CO₂ (ppm)', color: '#374151', font: { weight: 'bold' } }");
      client.println("          }");
      client.println("        }");
      client.println("      }");
      client.println("    });");
      client.println("    async function fetchData() {");
      client.println("      const statusEl = document.getElementById('statusIndicator');");
      client.println("      try {");
      client.println("        const response = await fetch(\"/data\");");
      client.println("        if (!response.ok) throw new Error(`HTTP ${response.status}`);");
      client.println("        const data = await response.json();");
      client.println("        if (data.co2 === 0) {");
      client.println("          statusEl.innerText = \"Connecting...\";");
      client.println("          statusEl.className = \"text-sm font-semibold text-yellow-500\";");
      client.println("        } else {");
      client.println("          statusEl.innerText = \"Connected\";");
      client.println("          statusEl.className = \"text-sm font-semibold text-green-600\";");
      client.println("          const now = new Date().toLocaleTimeString([], { hour12: false, hour: '2-digit', minute: '2-digit'});");
      client.println("          const fullTime = new Date().toLocaleTimeString([], { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' });");
      client.println("          document.getElementById('co2Value').innerText = data.co2;");
      client.println("          // Only add non-zero values to the chart");
      client.println("          const labels = co2Chart.data.labels;");
      client.println("          let labelToAdd = \"\";");
      client.println("          const currMinute = now.split(\":\")[1];");
      client.println("          if (lastMinuteLabel !== currMinute) {");
      client.println("            labelToAdd = now;");
      client.println("            lastMinuteLabel = currMinute;");
      client.println("          }");
      client.println("          labels.push(labelToAdd);");
      client.println("          co2Chart.data.datasets[0].data.push(data.co2);");
      client.println("          fullTimes.push(fullTime);");
      client.println("          if (labels.length > 20) {");
      client.println("            labels.shift();");
      client.println("            co2Chart.data.datasets[0].data.shift();");
      client.println("            fullTimes.shift();");
      client.println("          }");
      client.println("          co2Chart.update();");
      client.println("        }");
      client.println("      } catch (err) {");
      client.println("        statusEl.innerText = \"Disconnected\";");
      client.println("        statusEl.className = \"text-sm font-semibold text-red-600\";");
      client.println("        console.error(\"Error fetching CO₂ data:\", err);");
      client.println("      }");
      client.println("    }");
      client.println("    setInterval(fetchData, 5500);");
      client.println("    fetchData();");
      client.println("  </script>");
      client.println("</body>");
      client.println("</html>");
      client.stop();
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
}

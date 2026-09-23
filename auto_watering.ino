#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

// ======= User config =======
const char* WIFI_SSID = "YOUR_SSID"; // <-- set your WiFi SSID
const char* WIFI_PASS = "YOUR_PASSWORD"; // <-- set your WiFi password

// Pins
const int SOIL_PIN = 34; // ADC pin for soil sensor (ADC1)
const int DHT_PIN = 27;  // DHT data pin
const int RELAY_PIN = 26; // Relay control pin (pump)
const int SW1_PIN = 14; // Auto mode toggle
const int SW2_PIN = 12; // Manual water ON
const int SW3_PIN = 13; // Manual water OFF

// Calibration for soil sensor (raw ADC values)
// Adjust these values for your sensor: dry -> near-air reading, wet -> submerged in water
const int SOIL_RAW_DRY = 3000; // raw value when dry (adjust)
const int SOIL_RAW_WET = 1200; // raw value when wet (adjust)

// Thresholds
const int THRESHOLD_DRY = 35; // percent or equal -> consider dry -> start pump
const int THRESHOLD_WET = 60; // percent or equal -> stop pump

// Globals
LiquidCrystal_I2C lcd(0x27, 16, 2);
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
DHT dht(DHT_PIN, DHT22);

bool autoMode = true;
bool manualOverride = false;
bool pumpState = false;

unsigned long lastSensorMillis = 0;
unsigned long sensorInterval = 2000; // ms
float lastTemp = 0.0;
int lastSoilPercent = 0;

// Debounce
unsigned long lastSw1 = 0, lastSw2 = 0, lastSw3 = 0;
const unsigned long debounce = 200;

void setPump(bool on) {
  pumpState = on;
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
}

int readSoilPercent() {
  int raw = analogRead(SOIL_PIN); // 0-4095
  // constrain and map to 0-100
  raw = constrain(raw, SOIL_RAW_WET, SOIL_RAW_DRY);
  int pct = map(raw, SOIL_RAW_WET, SOIL_RAW_DRY, 100, 0); // wet->100, dry->0
  pct = constrain(pct, 0, 100);
  return pct;
}

String jsonStatus() {
  String s = "{";
  s += "\"temp\":" + String(lastTemp, 1) + ",";
  s += "\"soil\":" + String(lastSoilPercent) + ",";
  s += "\"pump\":" + String(pumpState ? 1 : 0) + ",";
  s += "\"auto\":" + String(autoMode ? 1 : 0);
  s += "}";
  return s;
}

// Websocket event
void onWsEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_TEXT) {
    String msg = String((char*)payload);
    if (msg == "pump_on") {
      manualOverride = true;
      autoMode = false;
      setPump(true);
    } else if (msg == "pump_off") {
      manualOverride = false;
      setPump(false);
    } else if (msg == "auto_on") {
      autoMode = true;
      manualOverride = false;
    } else if (msg == "auto_off") {
      autoMode = false;
      manualOverride = false;
    }
    // Reply current status
    webSocket.broadcastTXT(jsonStatus());
  }
}

// Serve web UI
const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>Auto Watering</title>
  <style>body{font-family:Arial;max-width:420px;margin:8px;} .big{font-size:1.4em}</style>
</head>
<body>
  <h2>Auto Watering</h2>
  <div class="big">Temperature: <span id="temp">--</span> °C</div>
  <div class="big">Soil: <span id="soil">--</span> %</div>
  <div>Status: <span id="status">--</span></div>
  <hr>
  <button onclick="send('auto_on')">Auto ON</button>
  <button onclick="send('auto_off')">Auto OFF</button>
  <button onclick="send('pump_on')">Pump ON</button>
  <button onclick="send('pump_off')">Pump OFF</button>

  <script>
    let ws;
    function init(){
      const host = location.hostname;
      ws = new WebSocket('ws://'+host+':81/');
      ws.onopen = ()=>{ console.log('ws open'); };
      ws.onmessage = (evt)=>{ try{ const j=JSON.parse(evt.data); document.getElementById('temp').innerText=j.temp; document.getElementById('soil').innerText=j.soil; document.getElementById('status').innerText=(j.pump? 'PUMP ON':'PUMP OFF') + ' | auto:'+(j.auto? 'ON':'OFF'); }catch(e){} };
      ws.onclose = ()=>{ setTimeout(init,2000); };
    }
    function send(cmd){ if(ws && ws.readyState===1) ws.send(cmd); }
    window.onload = init;
  </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", index_html);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  // pins
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(SW1_PIN, INPUT_PULLUP);
  pinMode(SW2_PIN, INPUT_PULLUP);
  pinMode(SW3_PIN, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();
  lcd.clear();

  dht.begin();

  // WiFi connect
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(500);
    Serial.print('.');
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nCannot connect, start AP");
    WiFi.softAP("AutoWater_AP");
    Serial.print("AP IP:");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.print("\nConnected. IP: ");
    Serial.println(WiFi.localIP());
  }

  server.on("/", handleRoot);
  server.begin();

  webSocket.begin();
  webSocket.onEvent(onWsEvent);

  // initial sensor read
  lastSensorMillis = 0;
}

void loop() {
  server.handleClient();
  webSocket.loop();

  unsigned long now = millis();

  // read switches (debounced, active LOW)
  if (digitalRead(SW1_PIN) == LOW && now - lastSw1 > debounce) {
    lastSw1 = now;
    autoMode = !autoMode;
    if (autoMode) manualOverride = false;
  }
  if (digitalRead(SW2_PIN) == LOW && now - lastSw2 > debounce) {
    lastSw2 = now;
    manualOverride = true;
    autoMode = false;
    setPump(true);
  }
  if (digitalRead(SW3_PIN) == LOW && now - lastSw3 > debounce) {
    lastSw3 = now;
    manualOverride = false;
    setPump(false);
  }

  if (now - lastSensorMillis >= sensorInterval) {
    lastSensorMillis = now;
    // read sensors
    int soil = readSoilPercent();
    float temp = dht.readTemperature();
    if (isnan(temp)) temp = lastTemp; else lastTemp = temp;
    lastSoilPercent = soil;

    // Auto logic (only when not manual override)
    if (autoMode && !manualOverride) {
      if (soil <= THRESHOLD_DRY) {
        setPump(true);
      } else if (soil >= THRESHOLD_WET) {
        setPump(false);
      }
    }

    // Update LCD
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("T:"); lcd.print(lastTemp,1); lcd.print((char)223); lcd.print("C ");
    lcd.print(pumpState?"P:ON":"P:OFF");
    lcd.setCursor(0,1);
    lcd.print("Soil:"); lcd.print(lastSoilPercent); lcd.print("% ");
    lcd.print(autoMode?"Auto":"Man");

    // Broadcast over websocket
    webSocket.broadcastTXT(jsonStatus());
  }
}

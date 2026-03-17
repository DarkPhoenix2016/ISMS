#include <WiFi.h>
#include <WebServer.h>

const char* ssid = "SLT-4G-FE1117";
const char* password = "6TE44H8X83";

WebServer server(80);

// MQ-135
#define MQ135_AO 33
#define MQ135_DO 19

// MQ-2
#define MQ2_AO 25
#define MQ2_DO 21

String getHTML() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Gas Dashboard</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
body { font-family: Arial; text-align:center; background:#111; color:white; }
.card {
  display:inline-block;
  padding:20px;
  margin:20px;
  border-radius:10px;
  width:300px;
}
.green { background:#1e7e34; }
.red { background:#a71d2a; }
canvas { background:white; border-radius:10px; margin-top:20px; }
</style>
</head>
<body>

<h2>ESP32 Gas Monitoring Dashboard</h2>

<div id="mq135" class="card">
<h3>MQ-135</h3>
<p>Analog: <span id="mq135_analog">0</span></p>
<p>Status: <span id="mq135_status">Safe</span></p>
</div>

<div id="mq2" class="card">
<h3>MQ-2</h3>
<p>Analog: <span id="mq2_analog">0</span></p>
<p>Status: <span id="mq2_status">Safe</span></p>
</div>

<canvas id="chart135" width="400" height="200"></canvas>
<canvas id="chart2" width="400" height="200"></canvas>

<script>
let labels = [];
let data135 = [];
let data2 = [];

const ctx135 = document.getElementById('chart135').getContext('2d');
const ctx2 = document.getElementById('chart2').getContext('2d');

const chart135 = new Chart(ctx135, {
    type: 'line',
    data: { labels: labels,
        datasets: [{ label: 'MQ-135', data: data135, borderColor: 'blue', fill:false }]
    }
});

const chart2 = new Chart(ctx2, {
    type: 'line',
    data: { labels: labels,
        datasets: [{ label: 'MQ-2', data: data2, borderColor: 'orange', fill:false }]
    }
});

function updateData(){
  fetch("/data")
  .then(response => response.json())
  .then(data => {

    document.getElementById("mq135_analog").innerText = data.mq135_analog;
    document.getElementById("mq2_analog").innerText = data.mq2_analog;

    if(data.mq135_digital == 1){
      document.getElementById("mq135").className = "card red";
      document.getElementById("mq135_status").innerText = "Gas Detected!";
    } else {
      document.getElementById("mq135").className = "card green";
      document.getElementById("mq135_status").innerText = "Safe";
    }

    if(data.mq2_digital == 1){
      document.getElementById("mq2").className = "card red";
      document.getElementById("mq2_status").innerText = "Gas Detected!";
    } else {
      document.getElementById("mq2").className = "card green";
      document.getElementById("mq2_status").innerText = "Safe";
    }

    if(labels.length > 20){
      labels.shift();
      data135.shift();
      data2.shift();
    }

    labels.push(new Date().toLocaleTimeString());
    data135.push(data.mq135_analog);
    data2.push(data.mq2_analog);

    chart135.update();
    chart2.update();
  });
}

setInterval(updateData, 2000);
</script>

</body>
</html>
)rawliteral";
}

void handleRoot() {
  server.send(200, "text/html", getHTML());
}

void handleData() {
  int mq135_analog = analogRead(MQ135_AO);
  int mq135_digital = digitalRead(MQ135_DO);
  int mq2_analog = analogRead(MQ2_AO);
  int mq2_digital = digitalRead(MQ2_DO);

  String json = "{";
  json += "\"mq135_analog\":" + String(mq135_analog) + ",";
  json += "\"mq135_digital\":" + String(mq135_digital) + ",";
  json += "\"mq2_analog\":" + String(mq2_analog) + ",";
  json += "\"mq2_digital\":" + String(mq2_digital);
  json += "}";

  server.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);

  pinMode(MQ135_DO, INPUT);
  pinMode(MQ2_DO, INPUT);

  WiFi.begin(ssid, password);
  Serial.print("Connecting");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/data", handleData);

  server.begin();
}

void loop() {
  server.handleClient();
}
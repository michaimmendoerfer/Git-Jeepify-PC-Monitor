
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// --- KONFIGURATION ---

const char* ssid     = "UPC6864BEC";
const char* password = "cxKsdj6c4uhs";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

struct __attribute__((packed)) EspNowPacket {
    char jsonString[250];
};
EspNowPacket incomingPacket;

// Ringpuffer für die letzten 100 Nachrichten
const int HISTORY_SIZE = 100;
String history[HISTORY_SIZE];
int historyIndex = 0;
int historyCount = 0;

String macToString(const uint8_t *mac) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}

// Statische HTML/JS-Oberfläche mit komplett zweizeiligem Layout (Links & Rechts)
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP-NOW Realtime Monitor</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background: #f4f4f9; color: #333; display: flex; gap: 20px; flex-wrap: wrap; justify-content: center; }
        .card { background: white; padding: 20px; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); width: 100%; max-width: 550px; box-sizing: border-box; }
        h1 { color: #007bff; margin-top: 0; font-size: 1.5em; }
        pre { background: #eee; padding: 10px; border-radius: 4px; overflow-x: auto; font-family: monospace; font-size: 0.9em; max-height: 120px; }
        .meta { font-size: 0.9em; color: #666; margin-bottom: 15px; line-height: 1.5; }
        .status { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 5px; }
        .online { background: #28a745; }
        .offline { background: #dc3545; }
        .btn-pause { background: #007bff; color: white; border: none; padding: 10px 15px; border-radius: 4px; cursor: pointer; font-weight: bold; width: 100%; margin-bottom: 15px; }
        .btn-pause.paused { background: #dc3545; }
        
        /* Globales zweizeiliges Layout-System */
        .display-container { display: flex; flex-direction: column; gap: 12px; margin-top: 10px; }
        .row-primary { display: flex; gap: 15px; background: #eef7ff; padding: 10px 12px; border-radius: 6px; border-left: 5px solid #007bff; }
        .row-primary .data-block { flex: 1; }
        .row-primary .label { font-size: 0.8em; text-transform: uppercase; color: #555; font-weight: bold; display: block; }
        .row-primary .value { font-size: 1.25em; font-weight: bold; color: #004085; font-family: monospace; }
        
        .row-secondary { background: #f8f9fa; padding: 10px 12px; border-radius: 6px; border: 1px solid #e2e8f0; font-size: 0.9em; }
        .row-secondary .label { font-weight: bold; color: #495057; }
        .secondary-badge { display: inline-block; background: #e2e8f0; padding: 2px 6px; border-radius: 4px; margin: 2px 4px 2px 0; font-family: monospace; font-size: 0.85em; }

        /* Historie-spezifische Anpassungen */
        .history-list { max-height: 600px; overflow-y: auto; border: 1px solid #ddd; border-radius: 4px; display: flex; flex-direction: column; gap: 10px; padding: 10px; background: #fafafa; }
        .history-item { background: white; padding: 10px; border-radius: 6px; border: 1px solid #e2e8f0; cursor: pointer; transition: all 0.2s; box-shadow: 0 2px 4px rgba(0,0,0,0.02); }
        .history-item:hover { background: #f1f5f9; border-color: #cbd5e1; }
        .history-item.selected { background: #e3f2fd; border-color: #007bff; box-shadow: 0 0 0 1px #007bff; }
        .history-item .row-primary { border-left-width: 4px; padding: 6px 10px; }
        .history-item .row-primary .value { font-size: 1.1em; }
        .history-item .row-secondary { padding: 6px 10px; }
    </style>
</head>
<body>
    <!-- Linke Spalte: Detail- / Live-Ansicht -->
    <div class="card">
        <h1 id="view-title">ESP-NOW Live Monitor</h1>
        <button id="pause-btn" class="btn-pause" onclick="togglePause()">⏸ PAUSE</button>
        <div class="meta">
            <span id="ws-status" class="status offline"></span> <span id="ws-status-text">Verbinde...</span><br>
            <strong>Sender MAC:</strong> <span id="mac">--:--:--:--:--:--</span><br>
            <strong>Zeit:</strong> <span id="time">Noch keine Daten</span>
        </div>
        
        <h3>Strukturierte Daten:</h3>
        <div id="parsed-data-container" class="display-container">
            <p>Warte auf Pakete...</p>
        </div>

        <h3>Roher JSON-String:</h3>
        <pre id="raw-json">{}</pre>
    </div>

    <!-- Rechte Spalte: Verlauf -->
    <div class="card">
        <h1>Letzte 100 Nachrichten</h1>
        <p class="meta">Klicke auf eine Nachricht, um sie links im Detail einzufrieren.</p>
        <div id="history-container" class="history-list"></div>
    </div>

    <script>
        let gateway = `ws://${window.location.hostname}/ws`;
        let websocket;
        let isPaused = false;
        let localHistory = [];
        let selectedIndex = null;

        function initWebSocket() {
            websocket = new WebSocket(gateway);
            websocket.onopen = () => {
                document.getElementById('ws-status').className = "status online";
                document.getElementById('ws-status-text').innerText = "Live-Verbindung aktiv";
            };
            websocket.onclose = () => {
                document.getElementById('ws-status').className = "status offline";
                document.getElementById('ws-status-text').innerText = "Verbindung verloren.";
                setTimeout(initWebSocket, 2000);
            };
            websocket.onmessage = onMessage;
        }

        // Steuert den Pausenzustand der UI
        function setPauseState(state) {
            isPaused = state;
            const btn = document.getElementById('pause-btn');
            const title = document.getElementById('view-title');
            
            if (isPaused) {
                btn.innerText = "▶ FORTSETZEN";
                btn.classList.add('paused');
                title.innerText = "⏸ Monitor pausiert";
                title.style.color = "#dc3545";
            } else {
                btn.innerText = "⏸ PAUSE";
                btn.classList.remove('paused');
                title.innerText = "ESP-NOW Live Monitor";
                title.style.color = "#007bff";
                selectedIndex = null;
                renderHistory();
                if(localHistory.length > 0) updateLiveView(localHistory[0]);
            }
        }

        function togglePause() {
            setPauseState(!isPaused);
        }

        function onMessage(event) {
            let data = JSON.parse(event.data);
            
            if (Array.isArray(data)) {
                localHistory = data.map(str => JSON.parse(str)).reverse();
                renderHistory();
                if(localHistory.length > 0 && !isPaused) {
                    updateLiveView(localHistory[0]);
                }
                return;
            }

            localHistory.unshift(data);
            if (localHistory.length > 100) localHistory.pop();
            if (selectedIndex !== null) selectedIndex++;
            
            renderHistory();

            if (!isPaused) {
                updateLiveView(data);
            }
        }

        // Erzeugt die zweizeilige Darstellung für Links und Rechts
        function buildTwoRowHtml(data) {
            let sndVal = data.parsed.hasOwnProperty('SND') ? data.parsed['SND'] : '--';
            let rcvVal = data.parsed.hasOwnProperty('RCV') ? data.parsed['RCV'] : '--';
            
            let secondaryHtml = "";
            let coreKeys = ['SND', 'RCV', 'Fehler'];
            let hasSecondary = false;

            for (const [key, value] of Object.entries(data.parsed)) {
                if (!coreKeys.includes(key)) {
                    hasSecondary = true;
                    secondaryHtml += `<span class="secondary-badge"><span class="label">${key}:</span> ${value}</span>`;
                }
            }

            if(data.parsed.hasOwnProperty('Fehler')) {
                hasSecondary = true;
                secondaryHtml += `<span class="secondary-badge" style="background:#f8d7da; color:#721c24;"><span class="label">Fehler:</span> ${data.parsed['Fehler']}</span>`;
            }

            if (!hasSecondary) {
                secondaryHtml = "<i style='color:#999;'>Keine weiteren Daten</i>";
            }

            return `
                <div class="row-primary">
                    <div class="data-block">
                        <span class="label">SND</span>
                        <span class="value">${sndVal}</span>
                    </div>
                    <div class="data-block">
                        <span class="label">RCV</span>
                        <span class="value">${rcvVal}</span>
                    </div>
                </div>
                <div class="row-secondary">
                    ${secondaryHtml}
                </div>
            `;
        }

        function updateLiveView(data) {
            document.getElementById('mac').innerText = data.mac;
            document.getElementById('time').innerText = data.timestamp;
            document.getElementById('raw-json').innerText = JSON.stringify(data.raw, null, 2);
            document.getElementById('parsed-data-container').innerHTML = buildTwoRowHtml(data);
        }

        function renderHistory() {
            let html = "";
            localHistory.forEach((item, index) => {
                let isSelected = (index === selectedIndex) ? "selected" : "";
                let cardBody = buildTwoRowHtml(item);
                
                html += `<div class="history-item ${isSelected}" onclick="selectHistoryItem(${index})">
                    <div style="font-size: 0.8em; color: #666; margin-bottom: 6px; display: flex; justify-content: space-between;">
                        <span><b>[${item.timestamp}]</b> MAC: ${item.mac.substring(12)}</span>
                    </div>
                    ${cardBody}
                </div>`;
            });
            document.getElementById('history-container').innerHTML = html || "<p style='padding:10px; color:#999;'>Noch kein Verlauf vorhanden.</p>";
        }

        function selectHistoryItem(index) {
            selectedIndex = index;
            setPauseState(true);
            renderHistory();
            updateLiveView(localHistory[index]);
        }

        window.addEventListener('load', initWebSocket);
    </script>
</body>
</html>
)rawliteral";

void addToHistory(String jsonStr) {
    history[historyIndex] = jsonStr;
    historyIndex = (historyIndex + 1) % HISTORY_SIZE;
    if (historyCount < HISTORY_SIZE) historyCount++;
}

void sendHistoryToClient(AsyncWebSocketClient *client) {
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();
    
    int start = (historyCount == HISTORY_SIZE) ? historyIndex : 0;
    for (int i = 0; i < historyCount; i++) {
        int idx = (start + i) % HISTORY_SIZE;
        array.add(history[idx]);
    }
    
    String response;
    serializeJson(doc, response);
    client->text(response);
}

void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    int copyLen = (len < sizeof(incomingPacket.jsonString)) ? len : sizeof(incomingPacket.jsonString) - 1;
    memcpy(incomingPacket.jsonString, data, copyLen);
    incomingPacket.jsonString[copyLen] = '\0';
    
    String rawJsonStr = String(incomingPacket.jsonString);
    String senderMac = macToString(recv_info->src_addr);
    
    JsonDocument incomingDoc;
    DeserializationError error = deserializeJson(incomingDoc, rawJsonStr);
    
    JsonDocument wsDoc;
    wsDoc["mac"] = senderMac;
    wsDoc["timestamp"] = String(millis() / 1000) + "s";
    
    if (error) {
        wsDoc["parsed"]["Fehler"] = "Parser-Error";
        wsDoc["raw"] = rawJsonStr;
    } else {
        wsDoc["parsed"] = incomingDoc.as<JsonObject>();
        wsDoc["raw"] = incomingDoc;
    }
    
    String responseString;
    serializeJson(wsDoc, responseString);
    
    addToHistory(responseString);
    ws.textAll(responseString);
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
               void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        sendHistoryToClient(client);
    }
}

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf("\nVerbunden! http://%s\n", WiFi.localIP().toString().c_str());
    
    if (esp_now_init() != ESP_OK) return;
    esp_now_register_recv_cb(onDataRecv);
    
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });
    
    server.begin();
}

void loop() {
    ws.cleanupClients();
    delay(1000);
}

//wireless hardware
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
//google home
#include <SinricPro.h>
#include <SinricProSwitch.h>
//remote
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

#include <AC>
#include <AC REMOTE>

/* ---------------- WIFI & CREDENTIALS ---------------- */
const char* ssid = "ACT-ai";
const char* password = "Wifiatharsh1*";

#define APP_KEY    "30f711b4-5a18-4451-aac1-53165dc95ce3"
#define APP_SECRET "cb9fff13-5d59-4495-8dcb-80493fae4622-1864de18-ed5e-44f0-833a-4c3112fae155"

#define WHITE_ID   "699547d44ee3ff23bfdeeb79"
#define YELLOW_ID  "69a5ecb517b32c0941cb0f98"
#define FAN_ID     "69a5ed7a17b32c0941cb1048"


/* ---------------- PINS ---------------- */
#define IR_RECEIVE_PIN  D5
#define IR_SEND_PIN     D6
#define WHITE_RELAY     D0
#define YELLOW_RELAY    D1
#define FAN_RELAY       D2
#define BUTTON_PIN      D3  // Tactile Button

/* ---------------- OBJECTS ---------------- */
ESP8266WebServer server(80);
IRrecv irrecv(IR_RECEIVE_PIN);
IRsend irsend(IR_SEND_PIN);
decode_results results;

/* ---------------- STATES ---------------- */
bool whiteState = false;
bool yellowState = false;
bool fanState = false;
bool moodState = false;
int colorIndex = 0;
String currentMode = "none";

bool isBooting = true;
unsigned long bootTimer = 0;
const unsigned long bootDelay = 15000;

//TriTone LED
unsigned long lastCozyOffTime = 0; // Tracks when the light was last turned OFF
const unsigned long triToneResetTime = 6000; // 6 seconds reset window

// Button Debounce
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;
int buttonPushCounter = 0;
unsigned long lastButtonPressTime = 0;
const int doubleClickWindow = 400;

const uint32_t colorList[20] = {
  0xF720DF, 0xF710EF, 0xF730CF, 0xF708F7, 0xF728D7,
  0xF7A05F, 0xF7906F, 0xF7B04F, 0xF78877, 0xF7A857,
  0xF7609F, 0xF750AF, 0xF7708F, 0xF748B7, 0xF76897,
  0xF7E01F, 0xF7D02F, 0xF7F00F, 0xF7C837, 0xF7E817
};

/* ---------------- HELPER FUNCTIONS ---------------- */

void setRelay(int pin, bool state) {
  digitalWrite(pin, state ? LOW : HIGH); // Active LOW logic
}

void pushToSinric() {
  SinricProSwitch &w = SinricPro[WHITE_ID];
  w.sendPowerStateEvent(whiteState);
  SinricProSwitch &y = SinricPro[YELLOW_ID];
  y.sendPowerStateEvent(yellowState);
  SinricProSwitch &f = SinricPro[FAN_ID];
  f.sendPowerStateEvent(fanState);
}

//TriTone LED
void activateCozy() {
  unsigned long now = millis();
  
  // Decide if the bulb has reset to 'Cool' or is still in 'Memory'
  // If it's been OFF for more than 6 seconds, we need 2 clicks to reach Warm.
  if (now - lastCozyOffTime > triToneResetTime) {
    Serial.println("Bulb Reset detected: Double Toggling for Warm...");
    
    // 1st Click (Ends up at Cool)
    setRelay(WHITE_RELAY, 1); 
    delay(300); // Short pulse
    setRelay(WHITE_RELAY, 0);
    delay(300); // Short gap
    
    // 2nd Click (Ends up at Warm)
    setRelay(WHITE_RELAY, 1);
  } else {
    Serial.println("Bulb Memory active: Single Toggling for Warm...");
    // Just one click needed because it was already at 'Cool' or 'Neutral' in cycle
    setRelay(WHITE_RELAY, 1);
  }

  // Set other states
  whiteState = true; 
  yellowState = false; 
  fanState = true;
  currentMode = "cozy";
  
  setRelay(YELLOW_RELAY, 0);
  setRelay(FAN_RELAY, 1);
  irsend.sendNEC(0xF740BF, 32); // Mood OFF
  
  pushToSinric();
}

// Global Toggle Function (Shared by Remote and Button)
void toggleRoomMaster() {
  if(!whiteState && !fanState && !yellowState) {
    whiteState = true;
    yellowState = false;
    fanState = true;
    moodState = false;
    setRelay(WHITE_RELAY, whiteState);
    setRelay(YELLOW_RELAY, yellowState);
    setRelay(FAN_RELAY, fanState);
    currentMode = "focus";
  } else {
    whiteState = false;
    yellowState = false;
    fanState = false;
    moodState = false;
    setRelay(WHITE_RELAY, false);
    setRelay(YELLOW_RELAY, false);
    setRelay(FAN_RELAY, false);
    irsend.sendNEC(0xF740BF, 32); // Mood OFF
    currentMode = "away";
  }
  pushToSinric();
}

void cycleModes() {
  if (currentMode == "cozy") {
    // If already Cozy, go to Focus
    whiteState = true; 
    yellowState = false; 
    fanState = true;
    currentMode = "focus";
  } else {
    // If Away, Focus, Night, or Manual -> Go to Cozy
    whiteState = false; 
    yellowState = true; 
    fanState = true;
    currentMode = "cozy";
  }
  
  // Apply hardware changes
  setRelay(WHITE_RELAY, whiteState);
  setRelay(YELLOW_RELAY, yellowState);
  setRelay(FAN_RELAY, fanState);
  irsend.sendNEC(0xF740BF, 32); // Ensure Moodlight is OFF during these modes
  
  pushToSinric();
  Serial.println("Mode Switched to: " + currentMode);
}

/* ---------------- SINRIC CALLBACKS ---------------- */

bool onWhiteState(const String &deviceId, bool &state) {
  whiteState = state;
  setRelay(WHITE_RELAY, whiteState);
  return true;
}
bool onYellowState(const String &deviceId, bool &state) {
  yellowState = state;
  setRelay(YELLOW_RELAY, yellowState);
  return true;
}
bool onFanState(const String &deviceId, bool &state) {
  fanState = state;
  setRelay(FAN_RELAY, fanState);
  return true;
}

/* ---------------- WEB PAGE ---------------- */

String getHTML() {
  return R"rawliteral(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  :root { --bg: #0a0a0a; --card: #1a1a1e; --accent: #9b59b6; --green: #31e169; }
  body { font-family: 'Segoe UI', sans-serif; background: var(--bg); color: white; margin: 0; padding: 20px; display: flex; justify-content: center; }
  .db { width: 100%; max-width: 400px; display: flex; flex-direction: column; gap: 15px; }
  h1.title { text-align: center; font-size: 2.2rem; margin-bottom: 5px; color: transparent; -webkit-text-stroke: 1px var(--green); text-shadow: 0 0 10px rgba(49, 225, 105, 0.4); letter-spacing: 2px; text-transform: uppercase; }
  .grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 12px; }
  .card { background: var(--card); padding: 15px; border-radius: 20px; border: 1px solid #333; display: flex; flex-direction: column; align-items: center; gap: 10px; }
  .name { font-size: 0.7rem; color: #888; font-weight: bold; text-transform: uppercase; }
  .sw { position: relative; width: 50px; height: 26px; }
  .sw input { opacity: 0; width: 0; height: 0; }
  .sl { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background: #333; transition: .4s; border-radius: 34px; }
  .sl:before { position: absolute; content: ""; height: 20px; width: 20px; left: 3px; bottom: 3px; background: white; transition: .4s; border-radius: 50%; }
  input:checked + .sl { background: var(--green); }
  input:checked + .sl:before { transform: translateX(24px); }
  .m-btn { background: #262626; border: 1px solid #3d3d3d; padding: 12px; border-radius: 12px; color: white; font-weight: bold; font-size: 11px; cursor: pointer; flex: 1; }
  .m-btn.active { background: var(--accent); border-color: var(--accent); }
  .mood-title { font-size: 1.8rem; text-align: center; margin: 10px 0; font-weight: 900; background: linear-gradient(45deg, #f1c40f, #9b59b6); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
  .cap { flex: 1; display: flex; background: var(--card); border: 1px solid #333; border-radius: 40px; overflow: hidden; height: 80px; }
  .cap-btn { font-weight: bold; flex: 1; background: transparent; border: none; color: white; font-size: 24px; cursor: pointer; }
  .cap-btn:active { background: #333; }
  .cap-label { font-weight: bold; flex: 1.5; display: flex; align-items: center; justify-content: center; font-size: 0.8rem; color: #666; border-left: 1px solid #333; border-right: 1px solid #333; text-align: center; }
  .flex { display: flex; gap: 10px; }
  .c-btn { height: 40px; border-radius: 10px; flex: 1; cursor: pointer; border: 2px solid transparent; }
</style>
<script>
  function send(c){ fetch("/cmd?c="+c); }
  function update(){
    fetch("/state").then(r=>r.json()).then(d=>{
      document.getElementById('ws').checked = d.white;
      document.getElementById('ys').checked = d.yellow;
      document.getElementById('fs').checked = d.fan;
      document.querySelectorAll('.m-btn').forEach(b => b.classList.remove('active'));
      if(d.mode !== "none" && document.getElementById(d.mode+'Btn')) document.getElementById(d.mode+'Btn').classList.add('active');
    });
  }
  setInterval(update, 2500);
</script></head><body>
<div class="db">
  <h1 class="title">Atharsh's SmartRoom</h1>
  <div class="grid">
    <div class="card"><span class="name">White Light</span><label class="sw"><input type="checkbox" id="ws" onchange="send('white')"><span class="sl"></span></label></div>
    <div class="card"><span class="name">Yellow Light</span><label class="sw"><input type="checkbox" id="ys" onchange="send('yellow')"><span class="sl"></span></label></div>
    <div class="card"><span class="name">Fan</span><label class="sw"><input type="checkbox" id="fs" onchange="send('fan')"><span class="sl"></span></label></div>
    <div class="card"><span class="name">Away</span><button class="m-btn" onclick="send('away')" style="width:100%">OFF</button></div>
  </div>
  <div class="flex">
    <button class="m-btn" id="cozyBtn" onclick="send('cozy')">COZY</button>
    <button class="m-btn" id="focusBtn" onclick="send('focus')">FOCUS</button>
    <button class="m-btn" id="nightBtn" onclick="send('night')">NIGHT</button>
  </div>
  <h2 class="mood-title">Mood Control</h2>
  <div class="flex" style="margin-top:10px">
    <button class="m-btn" onclick="send('moodon')">ON</button>
    <button class="m-btn" onclick="send('moodoff')">OFF</button>
  </div>
  <div class="cap">
    <button class="cap-btn" onclick="send('brightdown')">-</button>
    <div class="cap-label">BRIGHT</div>
    <button class="cap-btn" onclick="send('brightup')">+</button>
  </div>
  <div class="flex" style="margin-top:5px">
    <div class="c-btn" style="background:#e74c3c" onclick="send('colorred')"></div>
    <div class="c-btn" style="background:#f1c40f" onclick="send('coloryellow')"></div>
    <div class="c-btn" style="background:#9b59b6" onclick="send('colorpurple')"></div>
  </div>
</div></body></html>)rawliteral";
}

/* ---------------- WEB COMMAND HANDLER ---------------- */

void handleCommand() {
  String cmd = server.arg("c");
  if(cmd == "white") { whiteState = !whiteState; setRelay(WHITE_RELAY, whiteState); currentMode = "none"; }
  else if(cmd == "yellow") { yellowState = !yellowState; setRelay(YELLOW_RELAY, yellowState); currentMode = "none"; }
  else if(cmd == "fan") { fanState = !fanState; setRelay(FAN_RELAY, fanState); currentMode = "none"; }
  else if(cmd == "moodon") { moodState = true; irsend.sendNEC(0xF7C03F, 32);}
  else if(cmd == "moodoff") { moodState = false; irsend.sendNEC(0xF740BF, 32);}
  else if(cmd == "brightup") { irsend.sendNEC(0xF700FF, 32); }
  else if(cmd == "brightdown") { irsend.sendNEC(0xF7807F, 32); }
  else if(cmd == "colorred") { irsend.sendNEC(0xF720DF, 32); }
  else if(cmd == "coloryellow") { irsend.sendNEC(0xF708F7, 32); }
  else if(cmd == "colorpurple") { irsend.sendNEC(0xF748B7, 32); }
  else if(cmd == "cozy") { whiteState=false; yellowState=true; fanState=true; currentMode="cozy"; setRelay(WHITE_RELAY,0); setRelay(YELLOW_RELAY,1); setRelay(FAN_RELAY,1); irsend.sendNEC(0xF740BF,32); }
  else if(cmd == "focus") { whiteState=true; yellowState=false; fanState=true; currentMode="focus"; setRelay(WHITE_RELAY,1); setRelay(YELLOW_RELAY,0); setRelay(FAN_RELAY,1); irsend.sendNEC(0xF740BF,32); }
  else if(cmd == "night") { whiteState=false; yellowState=false; fanState=true; moodState=true; currentMode="night"; setRelay(WHITE_RELAY,0); setRelay(YELLOW_RELAY,0); setRelay(FAN_RELAY,1); irsend.sendNEC(0xF7C03F,32); }
  else if(cmd == "away") { whiteState=false; yellowState=false; fanState=false; moodState=false; currentMode="away"; setRelay(WHITE_RELAY,0); setRelay(YELLOW_RELAY,0); setRelay(FAN_RELAY,0); irsend.sendNEC(0xF740BF,32); }

  pushToSinric();
  server.send(200, "text/plain", "OK");
}

/* ---------------- SETUP & LOOP ---------------- */

void setup() {
  Serial.begin(115200);
  whiteState = false; yellowState = false; fanState = false; moodState = false;

  pinMode(WHITE_RELAY, OUTPUT); pinMode(YELLOW_RELAY, OUTPUT); pinMode(FAN_RELAY, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP); // Button to GND
  
  setRelay(WHITE_RELAY, false); setRelay(YELLOW_RELAY, false); setRelay(FAN_RELAY, false);
  irrecv.enableIRIn(); irsend.begin();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(YELLOW_RELAY, LOW); delay(2000);
    digitalWrite(YELLOW_RELAY, HIGH); delay(2000);
  }

  SinricProSwitch &w = SinricPro[WHITE_ID]; w.onPowerState(onWhiteState);
  SinricProSwitch &y = SinricPro[YELLOW_ID]; y.onPowerState(onYellowState);
  SinricProSwitch &f = SinricPro[FAN_ID]; f.onPowerState(onFanState);
  SinricPro.begin(APP_KEY, APP_SECRET);

  delay(100);
  pushToSinric();

  server.on("/", [](){ server.send(200, "text/html", getHTML()); });
  server.on("/cmd", handleCommand);
  server.on("/state", [](){
    String j = "{\"white\":"+String(whiteState?"true":"false")+",\"yellow\":"+String(yellowState?"true":"false")+",\"fan\":"+String(fanState?"true":"false")+",\"mode\":\""+currentMode+"\"}";
    server.send(200, "application/json", j);
  });
  server.begin();

  ArduinoOTA.setHostname("Atharsh_Smartroom");
  ArduinoOTA.setPassword("1234");
  ArduinoOTA.begin();
}


void loop() {
  yield();

  if (isBooting && (millis() - bootTimer > bootDelay)) {
    isBooting = false;
  }

  ArduinoOTA.handle();
  server.handleClient();
  SinricPro.handle();

// --- TACTILE BUTTON (SINGLE & DOUBLE CLICK) ---
  static bool lastButtonState = HIGH;
  bool currentReading = digitalRead(BUTTON_PIN);

  if (currentReading == LOW && lastButtonState == HIGH) {
    unsigned long now = millis();
    delay(50); // Debounce
    
    // Check if this click is within the window of the previous one
    if (now - lastButtonPressTime > doubleClickWindow) {
      buttonPushCounter = 1; 
    } else {
      buttonPushCounter++; 
    }
    lastButtonPressTime = now;
  }
  lastButtonState = currentReading;

  // Wait for the window to expire before deciding what action to take
  if (buttonPushCounter > 0 && (millis() - lastButtonPressTime > doubleClickWindow)) {
    if (buttonPushCounter == 1) {
      toggleRoomMaster(); // Single click: ON/OFF
    } 
    else if (buttonPushCounter >= 2) {
      Serial.println("Double Click: Cycling Modes...");
      cycleModes(); // Double click: Cozy <-> Focus
    }
    buttonPushCounter = 0;
  }

  // --- IR REMOTE HANDLES ---
  if (irrecv.decode(&results)) {
    uint32_t val = results.value;
    switch(val) {
      case 0xFF906F: whiteState = !whiteState; setRelay(WHITE_RELAY, whiteState); break;
      case 0xFFB847: yellowState = !yellowState; setRelay(YELLOW_RELAY, yellowState); break;
      case 0xFFF807: fanState = !fanState; setRelay(FAN_RELAY, fanState); break;
      case 0xFF6897: 
        whiteState = false; yellowState = true; fanState = true; moodState = false;
        setRelay(WHITE_RELAY, 0); setRelay(YELLOW_RELAY, 1); setRelay(FAN_RELAY, 1);
        irsend.sendNEC(0xF740BF, 32); currentMode = "cozy"; break;
      case 0xFF48B7: 
        whiteState = true; yellowState = false; fanState = true; moodState = false;
        setRelay(WHITE_RELAY, 1); setRelay(YELLOW_RELAY, 0); setRelay(FAN_RELAY, 1);
        irsend.sendNEC(0xF740BF, 32); currentMode = "focus"; break;
      case 0xFF22DD: 
        whiteState = false; yellowState = false; fanState = true; moodState = true;
        setRelay(WHITE_RELAY, 0); setRelay(YELLOW_RELAY, 0); setRelay(FAN_RELAY, 1);
        irsend.sendNEC(0xF7C03F, 32); currentMode = "night"; break;
      case 0xFF38C7: toggleRoomMaster(); break; // Shared Master Toggle
      case 0xFFE01F: moodState = !moodState; if (moodState) irsend.sendNEC(0xF7C03F, 32); else irsend.sendNEC(0xF740BF, 32); break;
      case 0xFFC03F: irsend.sendNEC(0xF700FF, 32); break;
      case 0xFF40BF: irsend.sendNEC(0xF7807F, 32); break;
      case 0xFF58A7: colorIndex++; if (colorIndex >= 20) colorIndex = 0; irsend.sendNEC(colorList[colorIndex], 32); break;
      case 0xFF708F: colorIndex--; if (colorIndex < 0) colorIndex = 19; irsend.sendNEC(colorList[colorIndex], 32); break;
      case 0xFF8877: colorIndex = 2; irsend.sendNEC(colorList[colorIndex], 32); break;
      case 0xFFA857: colorIndex = 13; irsend.sendNEC(colorList[colorIndex], 32); break;
      case 0xFFE817: colorIndex = 0; irsend.sendNEC(colorList[colorIndex], 32); break;
    }
    pushToSinric();
    irrecv.resume();
  }
}

I am adding a section to enable AC

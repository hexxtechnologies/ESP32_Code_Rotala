/*   AquaSpec ESP32 + SPI OLED
  Menu + Tanks list with WiFi and Supabase fetch
  Two line status bar shows WiFi and Supabase health
  Buttons: UP=25, DOWN=26, SELECT=27, BACK=14
  SPI OLED: MOSI=23, CLK=18, DC=21, CS=5, RESET=22
  RC522: SDA=4 (SS), SCK=18, MOSI=23, MISO=19, RST=13, IRQ optional=32
  Serial: 115200  set your Serial Monitor to 115200 8 N 1
*/

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <MFRC522.h>

// Buttons
#define UP_BUTTON     25
#define DOWN_BUTTON   26
#define SELECT_BUTTON 27
#define BACK_BUTTON   14

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_MOSI   23
#define OLED_CLK    18
#define OLED_RESET  22
#define OLED_DC     21
#define OLED_CS     5
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RESET, OLED_CS);

// RFID (RC522)
#define RFDI_SS_PIN 4
#define RFDI_RST_PIN 13
MFRC522 mfrc522(RFDI_SS_PIN, RFDI_RST_PIN);

// Sleep control
uint32_t lastInteraction = 0;
const uint32_t INACTIVITY_TIMEOUT_MS = 60000;
const uint32_t BACK_HOLD_TIME_MS = 5000;
uint32_t backPressStart = 0;

// Screen state
enum Screen {
  SCREEN_MENU,
  SCREEN_TANKS,
  SCREEN_RFID_MENU
};
Screen screen = SCREEN_MENU;

// Menu
// Insert RFID into main menu
const char* MENU_ITEMS[] = { "Tanks", "Settings", "About", "RFID" };
const int MENU_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);
int selectedIdx = 0;

// RFID submenu
const char* RFID_MENU[] = { "Read Tag", "Write Tag" };
const int RFID_MENU_COUNT = sizeof(RFID_MENU) / sizeof(RFID_MENU[0]);
int rfidSelected = 0;

// Tanks
struct Tank {
  String id;
  String name;
};
Tank tanks[20];
int tankCount = 0;
int tankSel = 0;

// WiFi and Supabase
const char* WIFI_SSID = "iPhone";
const char* WIFI_PASS = "yeet1234";

// Project settings
String SUPABASE_URL  = "https://dbfglovgjuzqiejekflg.supabase.co";
String SUPABASE_ANON = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImRiZmdsb3ZnanV6cWllamVrZmxnIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDM4ODI2NzQsImV4cCI6MjA1OTQ1ODY3NH0.mzRht4dDiCC9GQlX_5c1K_UJKWXvKeAHPBHqBVNsHvU";
String SUPABASE_HOST = "";

// Prototype RPC secret and target user
static const char* PROTO_SECRET = "DEVPROTOTYPE_ABC123";
static const char* PROTO_USER_ID = "255d3f82-115d-4918-b670-df4128af461e";

// Timers
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
const uint32_t TLS_TIMEOUT_MS          = 15000;

// Status flags
bool wifiOK = false;
int  wifiRSSI = 0;
bool supabaseOK = false;
int  supabaseHTTP = 0;

// Layout
const int STATUS_H = 20;
const int LINE_H   = 13;

// RFID state
bool rfidWriting = false;   // prevent global reads while writing

// Forward declarations
void setupButtons();
void drawSplash();
void drawStatusBar2();
void drawMenu();
void drawSelected(const char* label);
void drawTanksList();
void showSupabaseErrorOverlay(const char* context, int httpCode, const char* hint);
void ensureDummyTanks();
void goToSleep();
void turnOffOLED();

bool connectWifi();
bool testSupabase();
bool fetchTanks();            // uses RPC now
bool httpBegin(HTTPClient& http, WiFiClientSecure& client, const String& url);
String parseHostFromUrl(const String& url);

// RFID helpers
void enterRFIDMenu();
void drawRFIDMenu();
void handleRFIDMenu();
void rfidReadBlock(uint8_t block = 4);
bool rfidWriteBlock(uint8_t block = 4);
void rfidHandlePresentCard(uint8_t block = 4);  // shared card handler
void checkRFIDTap();                            // global read from any screen
void assignTagToTank(int idx);                  // assign NFC chip to tank
void dumpFullCardData();                        // new: dump all blocks to Serial

// ------------- Setup -------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== AquaSpec Menu + WiFi + Supabase RPC + RFID ===");
  Serial.println("Set Serial Monitor to 115200 8 N 1");

  // Initialize SPI with pins (SCK, MISO, MOSI, SS)
  SPI.begin(OLED_CLK, 19, OLED_MOSI, OLED_CS); // keep MISO=19, matches wiring

  if (!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println(F("SSD1306 allocation failed"));
    while (true) delay(1000);
  }
  display.setRotation(3);
  display.setTextWrap(false);

  drawSplash();

  setupButtons();
  lastInteraction = millis();

  SUPABASE_HOST = parseHostFromUrl(SUPABASE_URL);

  wifiOK = connectWifi();
  if (wifiOK) {
    wifiRSSI = WiFi.RSSI();
    supabaseOK = testSupabase();   // plain ping of REST to confirm reachability
  }

  // RC522 powered and initialized so we can read from any screen
  pinMode(RFDI_RST_PIN, OUTPUT);
  digitalWrite(RFDI_RST_PIN, HIGH);
  mfrc522.PCD_Init();

  drawMenu();
}

// ------------- Loop -------------
void loop() {
  static uint32_t lastDebounce = 0;
  const uint32_t debounceMs = 180;

  if (millis() - lastInteraction > INACTIVITY_TIMEOUT_MS) {
    goToSleep();
  }

  if (digitalRead(BACK_BUTTON) == LOW) {
    if (backPressStart == 0) backPressStart = millis();
    else if (millis() - backPressStart > BACK_HOLD_TIME_MS) goToSleep();
  } else {
    backPressStart = 0;
  }

  // Global RFID read available from any screen (non blocking)
  checkRFIDTap();

  if (screen == SCREEN_MENU) {
    if (millis() - lastDebounce > debounceMs) {
      if (digitalRead(UP_BUTTON) == LOW) {
        selectedIdx = (selectedIdx - 1 + MENU_COUNT) % MENU_COUNT;
        drawMenu();
        lastDebounce = millis();
        lastInteraction = millis();
      } else if (digitalRead(DOWN_BUTTON) == LOW) {
        selectedIdx = (selectedIdx + 1) % MENU_COUNT;
        drawMenu();
        lastDebounce = millis();
        lastInteraction = millis();
      } else if (digitalRead(SELECT_BUTTON) == LOW) {
        lastDebounce = millis();
        lastInteraction = millis();

        const char* choice = MENU_ITEMS[selectedIdx];
        if (strcmp(choice, "Tanks") == 0) {
          tankCount = 0;
          bool got = false;
          if (wifiOK) {
            got = fetchTanks();   // RPC call fills tanks
          } else {
            Serial.println("No WiFi, cannot fetch tanks");
          }

          if (!got) {
            const char* hint = nullptr;
            if (supabaseHTTP == 200) hint = "0 rows, check user id";
            showSupabaseErrorOverlay("Fetch tanks", supabaseHTTP, hint);
          }

          tankSel = 0;
          screen = SCREEN_TANKS;
          drawTanksList();
        } else if (strcmp(choice, "RFID") == 0) {
          // Enter RFID submenu
          enterRFIDMenu();
        } else {
          Serial.print("Selected: ");
          Serial.println(choice);
          drawSelected(choice);
          delay(900);
          drawMenu();
        }
      }
    }
  } else if (screen == SCREEN_TANKS) {
    if (millis() - lastDebounce > debounceMs) {
      if (digitalRead(UP_BUTTON) == LOW && tankCount > 0) {
        tankSel = (tankSel - 1 + tankCount) % tankCount;
        drawTanksList();
        lastDebounce = millis();
        lastInteraction = millis();
      } else if (digitalRead(DOWN_BUTTON) == LOW && tankCount > 0) {
        tankSel = (tankSel + 1) % tankCount;
        drawTanksList();
        lastDebounce = millis();
        lastInteraction = millis();
      } else if (digitalRead(SELECT_BUTTON) == LOW && tankCount > 0) {
        // Assign NFC chip to this tank
        lastDebounce = millis();
        lastInteraction = millis();
        assignTagToTank(tankSel);
        // After assignment, stay in tank list
        drawTanksList();
      } else if (digitalRead(BACK_BUTTON) == LOW) {
        screen = SCREEN_MENU;
        drawMenu();
        lastDebounce = millis();
        lastInteraction = millis();
        delay(150);
      }
    }
  } else if (screen == SCREEN_RFID_MENU) {
    handleRFIDMenu();
  }
}

// ------------- UI -------------
void drawSplash() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  const int W = display.width();

  auto bestTextSizeFor = [&](const char* s, int maxWidth) -> int {
    int len = strlen(s);
    if (len <= 0) return 1;
    int sz = max(1, maxWidth / (len * 6));
    if (sz > 3) sz = 3;
    return sz;
  };

  const char* title = "AquaSpec";
  const char* sub   = "Serial 115200";

  int titleSize = bestTextSizeFor(title, W - 4);
  int subSize   = bestTextSizeFor(sub,   W - 4);

  display.setTextSize(titleSize);
  int titlePixW = strlen(title) * 6 * titleSize;
  int titleX = (W - titlePixW) / 2;
  if (titleX < 0) titleX = 0;
  display.setCursor(titleX, 12);
  display.print(title);

  display.setTextSize(subSize);
  int subPixW = strlen(sub) * 6 * subSize;
  int subX = (W - subPixW) / 2;
  if (subX < 0) subX = 0;
  display.setCursor(subX, 32);
  display.print(sub);

  display.display();
  delay(1200);
}

void drawStatusBar2() {
  display.fillRect(0, 0, display.width(), STATUS_H, SSD1306_BLACK);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  String w = wifiOK ? String("WiFi OK ") + wifiRSSI + "dBm" : "WiFi FAIL";
  display.setCursor(2, 1);
  display.print(w);

  String s;
  if (!wifiOK) {
    s = "Supabase n a";
  } else if (supabaseHTTP == 200) {
    s = "Supabase OK";
  } else if (supabaseHTTP > 0) {
    s = String("Supabase HTTP ") + supabaseHTTP;
  } else {
    s = "Supabase FAIL";
  }
  display.setCursor(2, 11);
  display.print(s);
}

void drawMenu() {
  display.clearDisplay();
  drawStatusBar2();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const int W = display.width();
  const int charW = 6;
  const int padX  = 2;
  const int topY  = STATUS_H + 2;
  const int labelWidth = W - 2 * padX;
  const int maxChars = max(1, labelWidth / charW);

  auto trunc = [&](const char* s) -> String {
    String t = s;
    if ((int)t.length() > maxChars) {
      if (maxChars >= 3) t = t.substring(0, maxChars - 3) + "...";
      else t = t.substring(0, maxChars);
    }
    return t;
  };

  int start = selectedIdx - 1;
  if (start < 0) start = 0;
  if (start > max(0, MENU_COUNT - 4)) start = max(0, MENU_COUNT - 4);

  for (int i = 0; i < 4 && (start + i) < MENU_COUNT; i++) {
    int idx = start + i;
    int y = topY + i * LINE_H;

    if (idx == selectedIdx) {
      display.fillRect(0, y - 1, W, 11, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      display.setCursor(padX, y);
      display.print(trunc(MENU_ITEMS[idx]));
      display.setTextColor(SSD1306_WHITE);
    } else {
      display.setCursor(padX, y);
      display.print(trunc(MENU_ITEMS[idx]));
    }
  }

  display.display();
}

void drawSelected(const char* label) {
  display.clearDisplay();
  drawStatusBar2();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, STATUS_H + 4);
  display.print("Selected");
  display.setCursor(2, STATUS_H + 20);
  display.print(label);
  display.display();
}

void drawTanksList() {
  display.clearDisplay();
  drawStatusBar2();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const int W = display.width();
  const int charW = 6;
  const int padX = 2;
  const int topY = STATUS_H + 2;
  const int labelWidth = W - 2 * padX;
  const int maxChars = max(1, labelWidth / charW);

  auto trunc = [&](const String& s) -> String {
    if ((int)s.length() > maxChars) {
      if (maxChars >= 3) return s.substring(0, maxChars - 3) + "...";
      return s.substring(0, maxChars);
    }
    return s;
  };

  if (tankCount == 0) {
    display.setCursor(2, topY + 2);
    display.print("No tanks");
    if (wifiOK) {
      display.setCursor(2, topY + 16);
      if (supabaseHTTP == 200) display.print("0 rows check user");
      else if (supabaseHTTP > 0) { display.print("HTTP "); display.print(supabaseHTTP); }
      else display.print("SB not reachable");
    }
    display.display();
    return;
  }

  int start = tankSel - 1;
  if (start < 0) start = 0;
  if (start > max(0, tankCount - 4)) start = max(0, tankCount - 4);

  for (int i = 0; i < 4 && (start + i) < tankCount; i++) {
    int idx = start + i;
    int y = topY + 2 + i * LINE_H;

    if (idx == tankSel) {
      display.fillRect(0, y - 1, W, 11, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      display.setCursor(padX, y);
      display.print(trunc(tanks[idx].name));
      display.setTextColor(SSD1306_WHITE);
    } else {
      display.setCursor(padX, y);
      display.print(trunc(tanks[idx].name));
    }
  }

  display.display();
}

void showSupabaseErrorOverlay(const char* context, int httpCode, const char* hint) {
  display.fillRect(0, STATUS_H, display.width(), display.height()-STATUS_H, SSD1306_BLACK);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, STATUS_H + 4);
  display.print(context);
  display.setCursor(2, STATUS_H + 18);
  if (!wifiOK) {
    display.print("WiFi not connected");
  } else if (httpCode == 0) {
    display.print("Supabase FAIL");
  } else if (httpCode == 200) {
    display.print("0 rows");
  } else {
    display.print("HTTP ");
    display.print(httpCode);
  }
  if (hint) {
    display.setCursor(2, STATUS_H + 32);
    display.print(hint);
  }
  display.display();
  delay(1500);
}

// ------------- WiFi and Supabase -------------
bool connectWifi() {
  Serial.printf("Connecting WiFi to SSID [%s]\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  int spin = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(2, 10); display.print("WiFi connect");
    display.setCursor(2, 24); display.print(WIFI_SSID);
    static const char* sp[4] = {"-", "\\", "|", "/"};
    display.setCursor(2, 38); display.print(sp[spin++ % 4]);
    display.display();
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiRSSI = WiFi.RSSI();
    Serial.printf("WiFi OK  IP: %s  RSSI: %d dBm\n",
      WiFi.localIP().toString().c_str(), wifiRSSI);
    wifiOK = true;
    return true;
  } else {
    Serial.printf("WiFi FAILED status=%d\n", WiFi.status());
    wifiOK = false;
    return false;
  }
}

bool testSupabase() {
  supabaseOK = false;
  supabaseHTTP = 0;

  if (!wifiOK) return false;
  if (SUPABASE_HOST.isEmpty()) return false;

  IPAddress ip;
  if (!WiFi.hostByName(SUPABASE_HOST.c_str(), ip)) {
    Serial.println("DNS failed for Supabase");
    return false;
  }

  // Simple ping to REST root table with limit 1 using anon
  String url = "https://" + SUPABASE_HOST + "/rest/v1/tanks?select=id&limit=1";
  WiFiClientSecure client;
  HTTPClient http;
  if (!httpBegin(http, client, url)) return false;

  int code = http.GET();
  String body = http.getString();
  http.end();
  supabaseHTTP = code;

  Serial.printf("SB test GET -> %d\n", code);
  Serial.println(body);

  supabaseOK = (code == HTTP_CODE_OK);
  return supabaseOK;
}

// Now uses the RPC get_tanks_proto
bool fetchTanks() {
  if (!wifiOK) { supabaseOK = false; supabaseHTTP = 0; return false; }
  if (SUPABASE_HOST.isEmpty()) { supabaseOK = false; supabaseHTTP = 0; return false; }

  IPAddress ip;
  if (!WiFi.hostByName(SUPABASE_HOST.c_str(), ip)) {
    Serial.println("DNS failed");
    supabaseOK = false; supabaseHTTP = 0; return false;
  }

  String url = "https://" + SUPABASE_HOST + "/rest/v1/rpc/get_tanks_proto";
  WiFiClientSecure client;
  HTTPClient http;
  if (!httpBegin(http, client, url)) { supabaseOK = false; supabaseHTTP = 0; return false; }

  // Body with secret and user id
  String bodyReq = String("{\"p_secret\":\"") + PROTO_SECRET + "\",\"p_user\":\"" + PROTO_USER_ID + "\"}";

  int code = http.POST(bodyReq);
  String body = http.getString();
  http.end();
  supabaseHTTP = code;

  Serial.printf("RPC get_tanks_proto -> %d, bytes=%d\n", code, body.length());
  Serial.println(body);

  if (code != HTTP_CODE_OK) {
    supabaseOK = false;
    return false;
  }

  supabaseOK = true;

  // Parse [{"id":"...","name":"..."}]
  tankCount = 0;
  int pos = 0;
  while (tankCount < 20) {
    int idK = body.indexOf("\"id\":\"", pos);
    if (idK < 0) break;
    int idQ = body.indexOf("\"", idK + 6);
    if (idQ < 0) break;
    int nmK = body.indexOf("\"name\":\"", idQ);  if (nmK < 0) break;
    int nmQ = body.indexOf("\"", nmK + 8);       if (nmQ < 0) break;
    tanks[tankCount].id   = body.substring(idK + 6, idQ);
    tanks[tankCount].name = body.substring(nmK + 8, nmQ);
    tankCount++;
    pos = nmQ + 1;
  }

  Serial.printf("Parsed %d tank(s)\n", tankCount);
  for (int i = 0; i < tankCount; i++) {
    Serial.printf("  [%d] %s  id=%s\n", i, tanks[i].name.c_str(), tanks[i].id.c_str());
  }

  return tankCount > 0;
}

bool httpBegin(HTTPClient& http, WiFiClientSecure& client, const String& url) {
  client.setInsecure();
  client.setHandshakeTimeout(TLS_TIMEOUT_MS);
  client.setTimeout(TLS_TIMEOUT_MS);

  if (!http.begin(client, url)) return false;
  http.setConnectTimeout(TLS_TIMEOUT_MS);
  http.setTimeout(TLS_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON);
  http.addHeader("Accept-Profile",  "public");
  http.addHeader("Content-Profile", "public");
  return true;
}

String parseHostFromUrl(const String& url) {
  String s = url;
  if (s.startsWith("https://")) s.remove(0, 8);
  int slash = s.indexOf('/');
  if (slash >= 0) s = s.substring(0, slash);
  s.trim();
  return s;
}

// ------------- Buttons and Sleep -------------
void setupButtons() {
  pinMode(UP_BUTTON,     INPUT_PULLUP);
  pinMode(DOWN_BUTTON,   INPUT_PULLUP);
  pinMode(SELECT_BUTTON, INPUT_PULLUP);
  pinMode(BACK_BUTTON,   INPUT_PULLUP);
}

void turnOffOLED() {
  digitalWrite(OLED_DC, LOW);
  digitalWrite(OLED_CS, LOW);
  SPI.transfer(0x8D);
  SPI.transfer(0x10);
  SPI.transfer(0xAE);
  digitalWrite(OLED_CS, HIGH);
}

void goToSleep() {
  Serial.println("Going to sleep");
  turnOffOLED();

  esp_sleep_enable_ext0_wakeup((gpio_num_t)SELECT_BUTTON, 0);
  while (digitalRead(SELECT_BUTTON) == LOW) delay(10);

  delay(100);
  esp_deep_sleep_start();
}

// ------------- Demo fallback -------------
void ensureDummyTanks() {
  if (tankCount > 0) return;
  const char* demo[] = {
    "Discus", "Shrimp Rack", "Reef 40B", "Betta Nano",
    "Planted 20L", "Goldfish", "Breeder A", "Breeder B"
  };
  int n = sizeof(demo) / sizeof(demo[0]);
  for (int i = 0; i < n && i < 20; i++) {
    tanks[tankCount].id = String(i + 1);
    tanks[tankCount].name = demo[i];
    tankCount++;
  }
}

// ------------- RFID integration -------------
void enterRFIDMenu() {
  // RC522 already initialized in setup, but safe to reinit here if needed
  mfrc522.PCD_Init();
  delay(50);
  rfidSelected = 0;
  screen = SCREEN_RFID_MENU;
  drawRFIDMenu();
}

void drawRFIDMenu() {
  display.clearDisplay();
  drawStatusBar2();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const int W = display.width();
  const int charW = 6;
  const int padX  = 2;
  const int topY  = STATUS_H + 2;
  const int maxItems = 4;
  const int maxChars = max(1, (W - 2 * padX) / charW);

  for (int i = 0; i < RFID_MENU_COUNT && i < maxItems; i++) {
    int y = topY + i * LINE_H;
    if (i == rfidSelected) {
      display.fillRect(0, y - 1, W, 11, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      display.setCursor(padX, y);
      String t = String(RFID_MENU[i]);
      if ((int)t.length() > maxChars) t = t.substring(0, maxChars - 3) + "...";
      display.print(t);
      display.setTextColor(SSD1306_WHITE);
    } else {
      display.setCursor(padX, y);
      display.print(RFID_MENU[i]);
    }
  }
  display.display();
}

void handleRFIDMenu() {
  static uint32_t lastDebounce = 0;
  const uint32_t debounceMs = 180;
  if (millis() - lastDebounce > debounceMs) {
    if (digitalRead(UP_BUTTON) == LOW) {
      rfidSelected = (rfidSelected - 1 + RFID_MENU_COUNT) % RFID_MENU_COUNT;
      drawRFIDMenu();
      lastDebounce = millis();
      lastInteraction = millis();
    } else if (digitalRead(DOWN_BUTTON) == LOW) {
      rfidSelected = (rfidSelected + 1) % RFID_MENU_COUNT;
      drawRFIDMenu();
      lastDebounce = millis();
      lastInteraction = millis();
    } else if (digitalRead(SELECT_BUTTON) == LOW) {
      lastDebounce = millis();
      lastInteraction = millis();
      if (rfidSelected == 0) {
        // Read Tag
        display.clearDisplay();
        drawStatusBar2();
        display.setCursor(2, STATUS_H + 4);
        display.print("RFID: Read Tag");
        display.setCursor(2, STATUS_H + 20);
        display.print("Bring tag near...");
        display.display();
        rfidReadBlock(4); // read block 4 (sector 1 block 0)
        drawRFIDMenu();
      } else if (rfidSelected == 1) {
        // Write Tag (serial input)
        display.clearDisplay();
        drawStatusBar2();
        display.setCursor(2, STATUS_H + 4);
        display.print("RFID: Write Tag");
        display.setCursor(2, STATUS_H + 20);
        display.print("Bring tag near...");
        display.display();
        // prompt user on serial
        Serial.println("RFID Write selected. Bring tag near, then type up to 16 characters and press Enter.");
        rfidWriteBlock(4); // write to block 4
        drawRFIDMenu();
      }
    } else if (digitalRead(BACK_BUTTON) == LOW) {
      // exit RFID menu
      screen = SCREEN_MENU;
      drawMenu();
      lastDebounce = millis();
      lastInteraction = millis();
      delay(120);
    }
  }
}

// Shared handler for a card that is already detected and uid loaded
void rfidHandlePresentCard(uint8_t block) {
  MFRC522::StatusCode status;

  // print UID
  String uidS;
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uidS += "0";
    uidS += String(mfrc522.uid.uidByte[i], HEX);
    if (i + 1 < mfrc522.uid.size) uidS += ":";
  }
  uidS.toUpperCase();
  Serial.print("UID: "); Serial.println(uidS);

  // Authenticate using default key A
  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  status = (MFRC522::StatusCode)mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, block, &key, &(mfrc522.uid));
  if (status != MFRC522::STATUS_OK) {
    Serial.print("Auth failed: "); Serial.println(mfrc522.GetStatusCodeName(status));
    display.clearDisplay();
    drawStatusBar2();
    display.setCursor(2, STATUS_H + 4);
    display.print("Auth failed");
    display.display();
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(1200);
    return;
  }

  byte buffer[18];
  byte size = sizeof(buffer);
  status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(block, buffer, &size);
  if (status != MFRC522::STATUS_OK) {
    Serial.print("Read failed: "); Serial.println(mfrc522.GetStatusCodeName(status));
    display.clearDisplay();
    drawStatusBar2();
    display.setCursor(2, STATUS_H + 4);
    display.print("Read failed");
    display.display();
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(1200);
    return;
  }

  // Build printable string from 16 bytes
  char txt[17];
  for (int i = 0; i < 16; i++) {
    char c = buffer[i];
    if (c >= 32 && c <= 126) txt[i] = c;
    else txt[i] = '.';
  }
  txt[16] = 0;

  Serial.print("Block "); Serial.print(block); Serial.print(" data: ");
  for (int i = 0; i < 16; i++) Serial.write(buffer[i]);
  Serial.println();
  Serial.print("As text: "); Serial.println(txt);

  // Dump entire card contents to Serial
  dumpFullCardData();

  // display
  display.clearDisplay();
  drawStatusBar2();
  display.setCursor(2, STATUS_H + 2);
  display.setTextSize(1);
  display.print("UID:");
  display.setCursor(2, STATUS_H + 12);
  display.print(uidS);
  display.setCursor(2, STATUS_H + 28);
  display.print("Text:");
  display.setCursor(2, STATUS_H + 40);
  display.print(txt);
  display.display();

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  delay(1800);
}

// Read a 16 byte block and display text (first 16 bytes) and UID
void rfidReadBlock(uint8_t block) {
  // wait for card
  unsigned long start = millis();
  while (true) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) break;
    if (millis() - start > 15000) {
      Serial.println("Timeout waiting for card");
      display.clearDisplay();
      drawStatusBar2();
      display.setCursor(2, STATUS_H + 4);
      display.print("Read timeout");
      display.display();
      delay(1000);
      return;
    }
    delay(50);
  }

  lastInteraction = millis();
  rfidHandlePresentCard(block);
}

// Dump all blocks on a 1K card (0–63) to Serial in hex and ASCII
void dumpFullCardData() {
  Serial.println("---- Dump card blocks 0-63 ----");
  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  byte buffer[18];
  byte size = sizeof(buffer);

  for (byte block = 0; block < 64; block++) {
    MFRC522::StatusCode status = (MFRC522::StatusCode)mfrc522.PCD_Authenticate(
      MFRC522::PICC_CMD_MF_AUTH_KEY_A, block, &key, &(mfrc522.uid));
    if (status != MFRC522::STATUS_OK) {
      Serial.print("Block "); Serial.print(block);
      Serial.print(" auth failed: ");
      Serial.println(mfrc522.GetStatusCodeName(status));
      continue;
    }

    size = sizeof(buffer);
    status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(block, buffer, &size);
    if (status != MFRC522::STATUS_OK) {
      Serial.print("Block "); Serial.print(block);
      Serial.print(" read failed: ");
      Serial.println(mfrc522.GetStatusCodeName(status));
      continue;
    }

    Serial.print("Block "); Serial.print(block); Serial.print(": ");

    for (byte i = 0; i < 16; i++) {
      if (buffer[i] < 0x10) Serial.print("0");
      Serial.print(buffer[i], HEX);
      Serial.print(" ");
    }

    Serial.print(" |");
    for (byte i = 0; i < 16; i++) {
      char c = buffer[i];
      if (c >= 32 && c <= 126) Serial.print(c);
      else Serial.print('.');
    }
    Serial.println("|");
  }
  Serial.println("---- End dump ----");
}

// Global NFC read checker, callable from any screen
void checkRFIDTap() {
  if (rfidWriting) return; // do not interfere while writing

  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  lastInteraction = millis();
  rfidHandlePresentCard(4);
}

// Assign NFC tag to a tank by writing the full tank id across blocks 4, 5, 6
void assignTagToTank(int idx) {
  if (idx < 0 || idx >= tankCount) return;

  rfidWriting = true;

  // Show prompt on OLED
  display.clearDisplay();
  drawStatusBar2();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, STATUS_H + 2);
  display.print("Assign tag to:");
  display.setCursor(2, STATUS_H + 14);
  display.print(tanks[idx].name);
  display.setCursor(2, STATUS_H + 28);
  display.print("Bring tag near...");
  display.display();

  Serial.print("Assigning NFC tag to tank: ");
  Serial.print(tanks[idx].name);
  Serial.print(" id=");
  Serial.println(tanks[idx].id);

  // wait for card
  unsigned long start = millis();
  while (true) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) break;
    if (millis() - start > 15000) {
      Serial.println("Timeout waiting for card");
      display.clearDisplay();
      drawStatusBar2();
      display.setCursor(2, STATUS_H + 4);
      display.print("Assign timeout");
      display.display();
      delay(1000);
      rfidWriting = false;
      return;
    }
    delay(50);
  }

  lastInteraction = millis();

  // print UID
  String uidS;
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uidS += "0";
    uidS += String(mfrc522.uid.uidByte[i], HEX);
    if (i + 1 < mfrc522.uid.size) uidS += ":";
  }
  uidS.toUpperCase();
  Serial.print("UID: "); Serial.println(uidS);

  // Prepare data to write: full tank id spread across blocks 4, 5, 6
  String id = tanks[idx].id;
  id.trim();

  const int TOTAL_BYTES = 16 * 3; // blocks 4, 5, 6
  if (id.length() > TOTAL_BYTES) {
    Serial.println("Tank id is longer than 48 bytes, truncating");
    id = id.substring(0, TOTAL_BYTES);
  }

  byte raw[TOTAL_BYTES];
  memset(raw, 0, TOTAL_BYTES);
  // getBytes writes a null terminator as well, so length is TOTAL_BYTES + 1
  id.getBytes(raw, TOTAL_BYTES + 1);

  // Blocks we use for storage
  const uint8_t blocks[3] = {4, 5, 6};

  // Authenticate and write each block
  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  for (int b = 0; b < 3; b++) {
    uint8_t block = blocks[b];
    MFRC522::StatusCode status;

    status = (MFRC522::StatusCode)mfrc522.PCD_Authenticate(
      MFRC522::PICC_CMD_MF_AUTH_KEY_A, block, &key, &(mfrc522.uid));
    if (status != MFRC522::STATUS_OK) {
      Serial.print("Assign auth failed on block ");
      Serial.print(block);
      Serial.print(": ");
      Serial.println(mfrc522.GetStatusCodeName(status));

      display.clearDisplay();
      drawStatusBar2();
      display.setCursor(2, STATUS_H + 4);
      display.print("Auth failed");
      display.display();

      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      delay(1200);
      rfidWriting = false;
      return;
    }

    status = (MFRC522::StatusCode)mfrc522.MIFARE_Write(
      block,
      raw + (b * 16),
      16
    );
    if (status != MFRC522::STATUS_OK) {
      Serial.print("Assign write failed on block ");
      Serial.print(block);
      Serial.print(": ");
      Serial.println(mfrc522.GetStatusCodeName(status));

      display.clearDisplay();
      drawStatusBar2();
      display.setCursor(2, STATUS_H + 4);
      display.print("Write failed");
      display.display();

      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      delay(1200);
      rfidWriting = false;
      return;
    }
  }

  Serial.print("Assign OK, wrote id: '");
  Serial.print(id);
  Serial.println("' to blocks 4,5,6");

  display.clearDisplay();
  drawStatusBar2();
  display.setCursor(2, STATUS_H + 4);
  display.print("Tag assigned");
  display.setCursor(2, STATUS_H + 18);
  display.print(tanks[idx].name);
  display.setCursor(2, STATUS_H + 32);
  display.print(id);
  display.display();

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  delay(1200);
  rfidWriting = false;
}


// Write up to 16 bytes to block. Reads user input from Serial.
bool rfidWriteBlock(uint8_t block) {
  rfidWriting = true;
  MFRC522::StatusCode status;
  // wait for card
  unsigned long start = millis();
  while (true) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) break;
    if (millis() - start > 15000) {
      Serial.println("Timeout waiting for card");
      display.clearDisplay();
      drawStatusBar2();
      display.setCursor(2, STATUS_H + 4);
      display.print("Write timeout");
      display.display();
      delay(1000);
      rfidWriting = false;
      return false;
    }
    delay(50);
  }

  lastInteraction = millis();

  // print UID
  String uidS;
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uidS += "0";
    uidS += String(mfrc522.uid.uidByte[i], HEX);
    if (i + 1 < mfrc522.uid.size) uidS += ":";
  }
  uidS.toUpperCase();
  Serial.print("UID: "); Serial.println(uidS);

  // request serial input
  Serial.print("Enter text (max 16 chars): ");
  unsigned long startWait = millis();
  while (!Serial.available() && millis() - startWait < 30000) {
    delay(50);
  }
  if (!Serial.available()) {
    Serial.println();
    Serial.println("No serial input. Aborting write.");
    display.clearDisplay();
    drawStatusBar2();
    display.setCursor(2, STATUS_H + 4);
    display.print("No serial input");
    display.display();
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(1000);
    rfidWriting = false;
    return false;
  }

  String input = Serial.readStringUntil('\n');
  input.trim();
  if (input.length() > 16) input = input.substring(0, 16);
  Serial.print("Writing: '"); Serial.print(input); Serial.println("'");

  // prepare 16 byte buffer
  byte data[16];
  memset(data, 0, 16);
  input.getBytes(data, 17); // copies up to 16 bytes plus null

  // Authenticate using default key A
  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  status = (MFRC522::StatusCode)mfrc522.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A, block, &key, &(mfrc522.uid));
  if (status != MFRC522::STATUS_OK) {
    Serial.print("Auth failed: "); Serial.println(mfrc522.GetStatusCodeName(status));
    display.clearDisplay();
    drawStatusBar2();
    display.setCursor(2, STATUS_H + 4);
    display.print("Auth failed");
    display.display();
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(1200);
    rfidWriting = false;
    return false;
  }

  status = (MFRC522::StatusCode)mfrc522.MIFARE_Write(block, data, 16);
  if (status != MFRC522::STATUS_OK) {
    Serial.print("Write failed: "); Serial.println(mfrc522.GetStatusCodeName(status));
    display.clearDisplay();
    drawStatusBar2();
    display.setCursor(2, STATUS_H + 4);
    display.print("Write failed");
    display.display();
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(1200);
    rfidWriting = false;
    return false;
  }

  Serial.println("Write OK");
  display.clearDisplay();
  drawStatusBar2();
  display.setCursor(2, STATUS_H + 4);
  display.print("Write OK");
  display.setCursor(2, STATUS_H + 22);
  display.print(input);
  display.display();

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  delay(1200);
  rfidWriting = false;
  return true;
}

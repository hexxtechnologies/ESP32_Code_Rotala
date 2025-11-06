#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------- Buttons ----------
#define UP_BUTTON     25
#define DOWN_BUTTON   26
#define SELECT_BUTTON 27
#define BACK_BUTTON   14

// ---------- OLED (SPI wiring) ----------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_MOSI   23
#define OLED_CLK    18
#define OLED_RESET  22
#define OLED_DC     21
#define OLED_CS     19
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RESET, OLED_CS);

// ---------- Sleep variables ----------
uint32_t lastInteraction = 0;
const uint32_t INACTIVITY_TIMEOUT_MS = 60000;  // 1 minute
const uint32_t BACK_HOLD_TIME_MS     = 5000;   // 5 seconds
uint32_t backPressStart = 0;

// ---------- OLED helpers ----------
void drawMenu(int selected) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  display.print("Simple Menu");
  const char* items[] = {"Option 1", "Option 2", "Option 3"};
  for (int i = 0; i < 3; i++) {
    int y = 16 + i * 12;
    if (i == selected) {
      display.setCursor(0, y);
      display.print(">");
    }
    display.setCursor(10, y);
    display.print(items[i]);
  }
  display.display();
}

// ---------- Turn off screen ----------
void turnOffOLED() {
  digitalWrite(OLED_DC, LOW);
  digitalWrite(OLED_CS, LOW);
  SPI.transfer(0x8D); // CHARGEPUMP
  SPI.transfer(0x10); // Disable charge pump
  SPI.transfer(0xAE); // DISPLAYOFF
  digitalWrite(OLED_CS, HIGH);
}

// ---------- Go to sleep ----------
void goToSleep() {
  Serial.println("Going to sleep...");
  turnOffOLED();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)SELECT_BUTTON, 0);
  while (digitalRead(SELECT_BUTTON) == LOW) delay(10);
  delay(100);
  esp_deep_sleep_start();
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  pinMode(UP_BUTTON, INPUT_PULLUP);
  pinMode(DOWN_BUTTON, INPUT_PULLUP);
  pinMode(SELECT_BUTTON, INPUT_PULLUP);
  pinMode(BACK_BUTTON, INPUT_PULLUP);

  SPI.begin(OLED_CLK, -1, OLED_MOSI, OLED_CS);
  if (!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println("SSD1306 allocation failed");
    while (1);
  }
  display.setRotation(3);
  drawMenu(0);
  lastInteraction = millis();
}

// ---------- Loop ----------
void loop() {
  static int selected = 0;
  static uint32_t lastDebounce = 0;
  const uint32_t debounceMs = 200;

  // Inactivity timeout
  if (millis() - lastInteraction > INACTIVITY_TIMEOUT_MS) {
    goToSleep();
  }

  // Back button hold
  if (digitalRead(BACK_BUTTON) == LOW) {
    if (backPressStart == 0) backPressStart = millis();
    else if (millis() - backPressStart > BACK_HOLD_TIME_MS) goToSleep();
  } else backPressStart = 0;

  // Menu navigation
  if (millis() - lastDebounce > debounceMs) {
    if (digitalRead(UP_BUTTON) == LOW) {
      selected = (selected - 1 + 3) % 3;
      drawMenu(selected);
      lastInteraction = millis();
      lastDebounce = millis();
    } else if (digitalRead(DOWN_BUTTON) == LOW) {
      selected = (selected + 1) % 3;
      drawMenu(selected);
      lastInteraction = millis();
      lastDebounce = millis();
    }
  }
}

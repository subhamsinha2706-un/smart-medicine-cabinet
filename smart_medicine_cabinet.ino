// ===========================================================
// SMART MEDICINE CABINET — ESP32 SINGLE-BOARD FINAL
// Dabble voice (Classic BT Terminal) name: "ESP32_MEDCAB"
// LCD SDA=18 SCL=19 (0x27) | Servos:13,12,14 | Keypad rows:27,26,25,33 cols:32,23,22,21
// Uses: LiquidCrystal_PCF8574, DabbleESP32, Preferences, ESP32Servo, Keypad
// ===========================================================

#define CUSTOM_SETTINGS
#define INCLUDE_TERMINAL_MODULE
#include <DabbleESP32.h>

#include <Wire.h>
#include <LiquidCrystal_PCF8574.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include <Keypad.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ======= CONFIGURE THESE =======
const char* WIFI_SSID = "wow";
const char* WIFI_PASS = "";

const char* SMTP_SERVER = "smtp.gmail.com";
const int   SMTP_PORT   = 465;
const char* SMTP_USER   = "princebetuverma05@gmail.com";
const char* SMTP_PASS   = "kopvsygqwjsdjdji";   // Gmail App Password
const char* EMAIL_TO    = "1si23ec083@sit.ac.in";

const int ALERT_DAYS = 30;
// =================================

Preferences prefs;

// ---------------------- LCD SETUP ----------------------
// I2C address = 0x27
LiquidCrystal_PCF8574 lcd(0x27);

// scrolling
unsigned long lastScroll = 0;
unsigned long scrollEnd = 0;
String scrollText = "";
String scrollBottom = "";
int scrollPos = 0;
bool scrolling = false;
const int SCROLL_SPEED = 250; // ms per scroll step

// ---------------------- SERVOS ----------------------
#define SERVO1_PIN 13
#define SERVO2_PIN 12
#define SERVO3_PIN 14

Servo servo1, servo2, servo3;
const int CLOSED_ANGLE = 90;
const int OPEN_ANGLE   = 0;

// ---------------------- KEYPAD ----------------------
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {27, 26, 25, 33};
byte colPins[COLS] = {32, 23, 22, 21};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ---------------------- DATA ----------------------
int medicineCount[3];
uint64_t expiryTs[3];
bool notified[3];
time_t lastCheckedDay = 0;

// ---------------------- FORWARDS ----------------------
void lcdShow(const String &top, const String &bottom);
void lcdShowLong(const String &top, const String &bottom, int duration = 4000);
void lcdTick();

void loadData();
void saveData();

void smoothServo(Servo &s);
void dispense(int id, int qty);

int getNumber();
uint64_t getExpiryDate();

void checkExpiry();
bool smtpSend(const char* subject, const char* body);

void wifiSyncTime();

// voice helpers
int extractCabinetFromVoice(String cmd);
int extractQuantityFromVoice(String cmd);

// ===========================================================
// LCD helpers
// ===========================================================
void lcdShow(const String &top, const String &bottom) {
  scrolling = false;
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(top);
  lcd.setCursor(0, 1); lcd.print(bottom);
}

void lcdShowLong(const String &top, const String &bottom, int duration) {
  lcd.clear();
  scrollBottom = bottom;

  if (top.length() <= 16) {
    lcdShow(top, bottom);
    scrollEnd = millis() + duration;
    scrolling = false;
    return;
  }

  scrollText = top + "   ";
  scrollPos = 0;
  scrolling = true;
  scrollEnd = millis() + duration;

  lcd.setCursor(0, 0);
  lcd.print(scrollText.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(bottom);
}

void lcdTick() {
  if (!scrolling) return;
  if (millis() > scrollEnd) {
    scrolling = false;
    return;
  }
  if (millis() - lastScroll >= SCROLL_SPEED) {
    lastScroll = millis();
    scrollPos++;
    if (scrollPos >= (int)scrollText.length()) scrollPos = 0;

    String fragment = "";
    for (int i = 0; i < 16; i++) {
      fragment += scrollText[(scrollPos + i) % scrollText.length()];
    }

    lcd.setCursor(0, 0);
    lcd.print(fragment);
    lcd.setCursor(0, 1);
    lcd.print(scrollBottom);
  }
}
// ===========================================================
// Preferences load/save
// ===========================================================
void loadData() {
  prefs.begin("medcab", true);
  for (int i = 0; i < 3; i++) {
    char keyCount[16], keyExp[16], keyNot[16];
    snprintf(keyCount, sizeof(keyCount), "count%d", i);
    snprintf(keyExp, sizeof(keyExp), "exp%d", i);
    snprintf(keyNot, sizeof(keyNot), "not%d", i);

    medicineCount[i] = prefs.getInt(keyCount, 0);
    expiryTs[i] = prefs.getULong64(keyExp, 0);
    notified[i] = prefs.getBool(keyNot, false);
  }
  prefs.end();
}

void saveData() {
  prefs.begin("medcab", false);
  for (int i = 0; i < 3; i++) {
    char keyCount[16], keyExp[16], keyNot[16];
    snprintf(keyCount, sizeof(keyCount), "count%d", i);
    snprintf(keyExp, sizeof(keyExp), "exp%d", i);
    snprintf(keyNot, sizeof(keyNot), "not%d", i);

    prefs.putInt(keyCount, medicineCount[i]);
    prefs.putULong64(keyExp, expiryTs[i]);
    prefs.putBool(keyNot, notified[i]);
  }
  prefs.end();
}

// ===========================================================
// Servo logic
// ===========================================================
void smoothServo(Servo &s) {
  s.write(OPEN_ANGLE);
  delay(700);
  s.write(CLOSED_ANGLE);
}

void dispense(int id, int qty) {
  if (qty <= 0) {
    lcdShow("Invalid Qty", "");
    return;
  }
  if (medicineCount[id] < qty) {
    lcdShow("Stock Low!", String(medicineCount[id]) + " only");
    return;
  }

  lcdShow("Dispensing...", "Cab " + String(id+1));
  if (id == 0) smoothServo(servo1);
  if (id == 1) smoothServo(servo2);
  if (id == 2) smoothServo(servo3);

  medicineCount[id] -= qty;
  notified[id] = false;
  saveData();

  lcdShow("Done!", "Remaining: " + String(medicineCount[id]));
  delay(1200);
}

// ===========================================================
// Keypad helpers
// ===========================================================
int getNumber() {
  lcdShow("Enter number", "# = OK");
  long num = 0;
  while (1) {
    char k = keypad.getKey();
    if (!k) continue;

    if (k >= '0' && k <= '9') {
      num = num * 10 + (k - '0');
      lcdShow("Typing:", String(num));
    }
    else if (k == '#') {
      return (int)num;
    }
    else if (k == '*') {
      lcdShow("Cancelled", "");
      return 0;
    }
  }
}

uint64_t getExpiryDate() {
  lcdShow("Enter expiry", "DDMMYYYY");
  String s = "";
  while (1) {
    char k = keypad.getKey();
    if (!k) continue;

    if (isdigit(k)) {
      s += k;
      lcdShow("Typing:", s);
    }
    else if (k == '#' && s.length() == 8) {
      int dd = s.substring(0,2).toInt();
      int mm = s.substring(2,4).toInt();
      int yy = s.substring(4,8).toInt();

      struct tm t;
      t.tm_sec = 0; t.tm_min = 0; t.tm_hour = 12;
      t.tm_mday = dd; t.tm_mon = mm - 1; t.tm_year = yy - 1900; t.tm_isdst = -1;

      time_t ts = mktime(&t);
      if (ts <= 0) {
        lcdShow("Invalid date", "");
        delay(800);
        return 0;
      }
      return (uint64_t)ts;
    }
    else if (k == '*') {
      lcdShow("Cancelled", "");
      return 0;
    }
  }
}

// ===========================================================
// WiFi + NTP
// ===========================================================
void wifiSyncTime() {
  lcdShowLong("Connecting WiFi...", "");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    lcdShow("WiFi Failed", "");
    return;
  }
  lcdShow("WiFi Connected", "");
  delay(400);
  configTime(0,0,"pool.ntp.org","time.nist.gov");
  time_t now = time(nullptr);
  start = millis();
  while (now < 1000000000 && millis() - start < 6000) {
    now = time(nullptr);
    delay(200);
  }
  lcdShow("Time Synced", "");
  delay(600);
}
// ===========================================================
// SMTP helpers
// ===========================================================
String base64enc(const String &in) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out; int val = 0, valb = -6;
  for (unsigned char c : in) {
    val = (val<<8) + c; valb += 8;
    while (valb >= 0) { out += tbl[(val>>valb)&0x3F]; valb -= 6; }
  }
  if (valb > -6) out += tbl[((val<<8)>>(valb+8))&0x3F];
  while (out.length() % 4) out += '=';
  return out;
}

bool smtpSend(const char* subject, const char* body) {
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect(SMTP_SERVER, SMTP_PORT)) { lcdShow("SMTP Fail", ""); return false; }

  auto recv = [&](int timeout = 5000)->String {
    String s; unsigned long start = millis();
    while (millis() - start < timeout) {
      while (client.available()) s += (char)client.read();
      if (s.length()) break;
      delay(5);
    }
    return s;
  };

  recv();
  client.println("EHLO esp32"); recv();
  client.println("AUTH LOGIN"); recv();
  client.println(base64enc(String(SMTP_USER))); recv();
  client.println(base64enc(String(SMTP_PASS))); recv();
  client.printf("MAIL FROM:<%s>\r\n", SMTP_USER); recv();
  client.printf("RCPT TO:<%s>\r\n", EMAIL_TO); recv();
  client.println("DATA"); recv();
  client.printf("Subject: %s\r\n\r\n%s\r\n.\r\n", subject, body); recv();
  client.println("QUIT"); recv();

  lcdShow("Email Sent", "");
  return true;
}

// ===========================================================
// Expiry check
// ===========================================================
void checkExpiry() {
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);
  if (t.tm_yday == lastCheckedDay) return;
  lastCheckedDay = t.tm_yday;

  for (int i=0;i<3;i++){
    if (expiryTs[i] == 0) continue;
    long daysLeft = (long)((expiryTs[i] - now) / 86400);
    if (daysLeft <= ALERT_DAYS && daysLeft >= 0 && !notified[i]) {
      char subject[64]; snprintf(subject, sizeof(subject), "Expiry Alert: Cabinet %d", i+1);
      char body[128]; snprintf(body, sizeof(body), "%ld days left. Stock %d", daysLeft, medicineCount[i]);
      if (smtpSend(subject, body)) { notified[i] = true; saveData(); }
    }
    if ((long)expiryTs[i] < now && !notified[i]) {
      char subject[64]; snprintf(subject, sizeof(subject), "Expired: Cabinet %d", i+1);
      char body[128]; snprintf(body, sizeof(body), "Expired. Stock %d", medicineCount[i]);
      if (smtpSend(subject, body)) { notified[i] = true; saveData(); }
    }
  }
}
// ===========================================================
// Voice parsing helpers
// ===========================================================
int extractCabinetFromVoice(String cmd) {
  cmd.toLowerCase();
  if (cmd.indexOf("para") >= 0  cmd.indexOf("paracetamol") >= 0) return 0;
  if (cmd.indexOf("calpol") >= 0) return 1;
  if (cmd.indexOf("digene") >= 0) return 2;
  if (cmd.indexOf("one") >= 0) return 0;
  if (cmd.indexOf("two") >= 0) return 1;
  if (cmd.indexOf("three") >= 0) return 2;
  return -1;
}

int extractQuantityFromVoice(String cmd) {
  cmd.toLowerCase();
  for (int i = 0; i < cmd.length(); i++) {
    if (isdigit(cmd[i])) {
      int j=i; String n="";
      while (j < cmd.length() && isdigit(cmd[j])) { n += cmd[j++]; }
      return n.toInt();
    }
  }
  if (cmd.indexOf("one") >= 0) return 1;
  if (cmd.indexOf("two") >= 0) return 2;
  if (cmd.indexOf("three") >= 0) return 3;
  if (cmd.indexOf("four") >= 0) return 4;
  if (cmd.indexOf("five") >= 0) return 5;
  return -1;
}

// ===========================================================
// SETUP
// ===========================================================
void setup() {
  Serial.begin(115200);

  // Dabble begin (Classic BT Terminal mode)
  Dabble.begin("ESP32_MEDCAB");

  // initialize Wire with your pins (SDA=18, SCL=19)
  Wire.begin(18, 19);

  // initialize LCD
  lcd.begin(16,2);
  lcd.setBacklight(255);
  lcdShow("Smart Cabinet", "Booting...");
  delay(700);

  // servos
  servo1.attach(SERVO1_PIN);
  servo2.attach(SERVO2_PIN);
  servo3.attach(SERVO3_PIN);
  servo1.write(CLOSED_ANGLE);
  servo2.write(CLOSED_ANGLE);
  servo3.write(CLOSED_ANGLE);

  // load stored data
  loadData();

  // wifi + time sync
  wifiSyncTime();

  // If empty first-time, ask via keypad
  if (medicineCount[0]==0 && medicineCount[1]==0 && medicineCount[2]==0 &&
      expiryTs[0]==0 && expiryTs[1]==0 && expiryTs[2]==0) {
    lcdShowLong("First time setup", "Use keypad", 4000);
    for (int i=0;i<3;i++){
      lcdShow("Cab " + String(i+1), "Enter stock");
      medicineCount[i] = getNumber();
      lcdShow("Cab " + String(i+1), "Enter expiry");
      expiryTs[i] = getExpiryDate();
      notified[i] = false;
      saveData();
    }
  }

  lcdShow("Menu:", "1=View 2=Disp 3=Reset 4=SetExp");
}

// ===========================================================
// LOOP
// ===========================================================
void loop() {
  // process dabble input regularly
  Dabble.processInput();

  lcdTick();
  checkExpiry();

  char key = keypad.getKey();
  if (!key) {
    delay(10);
    return;
  }

  // ---------- MENU OPTIONS ----------
  if (key == '1') {   // VIEW STATUS
    for (int i=0; i<3; i++) {
      String top = "Cab" + String(i+1) + ": " + String(medicineCount[i]);
      String bot = expiryTs[i] ? String(ctime((time_t*)&expiryTs[i])) : "Expiry: N/A";
      lcdShowLong(top, bot.substring(0, min((int)bot.length(), 16)), 2500);
      delay(3000);
    }
    lcdShow("Menu:", "1=View 2=Disp 3=Reset 4=SetExp");
  }

  else if (key == '2') {  // DISPENSE (speak/manual)
    lcdShow("Dispense:", "1=Speak 2=Manual");
    char modeKey = 0;
    unsigned long tstart = millis();
    while (!modeKey && millis() - tstart < 10000) { modeKey = keypad.getKey(); Dabble.processInput(); }

    if (modeKey == '1') {
      // Voice flow using Dabble Terminal
      lcdShow("Listening...", "");
      unsigned long vtstart = millis();
      String cmd = "";
      while (millis() - vtstart < 8000) { // 8s timeout
        Dabble.processInput();
        if (Terminal.available()) {
          cmd = Terminal.readString();
          cmd.trim();
          break;
        }
        delay(50);
      }

      if (cmd.length() == 0) {
        lcdShow("No voice input", "Try manual");
        delay(1200);
      } else {
        lcdShowLong("Heard: " + cmd, "", 2500);
        int cab = extractCabinetFromVoice(cmd);
        int qty = extractQuantityFromVoice(cmd);

        if (cab == -1) {
          lcdShow("No such med", "Try manual");
          delay(1200);
        } else {
          if (qty == -1) {lcdShow("How many?", "");
            delay(600);
            qty = getNumber();
          }
          if (qty > 0) dispense(cab, qty);
        }
      }
    } else { // Manual
      lcdShow("Enter medicine", "code then #");
      int code = getNumber();
      if (code <= 0) {
        lcdShow("Cancelled", "");
        delay(600);
      } else {
        int cab = code - 1;
        if (cab < 0  cab > 2) {
          lcdShow("Invalid code", "");
          delay(800);
        } else {
          lcdShow("Enter qty", "");
          int qty = getNumber();
          dispense(cab, qty);
        }
      }
    }

    lcdShow("Menu:", "1=View 2=Disp 3=Reset 4=SetExp");
  }

  else if (key == '3') { // RESET STOCK & EXPIRY
    for (int i=0;i<3;i++){
      lcdShow("Stock Cab " + String(i+1), "");
      medicineCount[i] = getNumber();

      lcdShow("Expiry Cab " + String(i+1), "");
      expiryTs[i] = getExpiryDate();

      notified[i] = false;
      saveData();
    }
    lcdShow("Reset Done", "");
    delay(1200);
    lcdShow("Menu:", "1=View 2=Disp 3=Reset 4=SetExp");
  }

  else if (key == '4') { // SET EXPIRY ONLY
    lcdShow("Set Expiry", "Cab 1/2/3");
    char k2 = 0;
    while (!k2) k2 = keypad.getKey();
    if (k2=='1'  k2=='2'  k2=='3') {
      int id = k2 - '1';
      expiryTs[id] = getExpiryDate();
      saveData();
      lcdShow("Saved", "");
      delay(800);
    }
    lcdShow("Menu:", "1=View 2=Disp 3=Reset 4=SetExp");
  }
}
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <LiquidCrystal_I2C.h>
#include <TinyGPSPlus.h>
#include <Wire.h>
#include <MPU6050.h>
#include <SPI.h>
#include <SD.h>
#include <driver/i2s.h>

// ---------------- WIFI ----------------
#define WIFI_SSID "Smart IoT Connect"
#define WIFI_PASSWORD "STJ35LCR"
#define API_KEY "AIzaSyBAVT6lfD7aMj-NXgtrsCZjrgicYasp2nM"
#define DATABASE_URL "https://smart-iot-connect-default-rtdb.asia-southeast1.firebasedatabase.app/"

// ---------------- LED ----------------
#define LED_WIFI 2
#define LED_GPS 15
#define LED_SYS 0

// ---------------- BUTTONS ----------------
#define BTN_DOWN 12
#define BTN_SEL 13
#define BTN_UP 14

// ---------------- ULTRASONIC ----------------
#define TRIG 32
#define ECHO 33

// ---------------- AUDIO ----------------
#define SD_CS 5
#define I2S_DOUT 27
#define I2S_BCLK 26
#define I2S_LRC 25
#define VOLUME_GAIN 5

// ---------------- GPS ----------------
TinyGPSPlus gps;
HardwareSerial gpsSerial(2);

// ---------------- MPU ----------------
MPU6050 mpu;

// ---------------- LCD ----------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- FIREBASE ----------------
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ---------------- DATA ----------------
String names[20];
int totalDest = 0;
int currentIndex = 0;

// ---------------- NAV ----------------
String instruction = "";
int distance = 0;
int lastDistance = 99999;

// ---------------- STATE ----------------
enum Mode { MENU,
            WAIT_GPS,
            NAVIGATION };
Mode mode = MENU;

// ---------------- TIMERS ----------------
unsigned long lastVoice = 0;
unsigned long lastGPSsend = 0;

// =======================================================
// 🔊 I2S SETUP
// =======================================================
void setupI2S() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .dma_buf_count = 8,
    .dma_buf_len = 512
  };

  i2s_pin_config_t pins = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
}

// =======================================================
// 🔊 AUDIO
// =======================================================
void playWav(const char *file) {
  File f = SD.open(file);
  if (!f) {
    Serial.println("File missing");
    return;
  }

  f.seek(44);
  uint8_t buf[512];

  while (f.available()) {
    size_t r = f.read(buf, 512);

    int16_t *samples = (int16_t *)buf;

    for (int i = 0; i < r / 2; i++) {
      int32_t temp = samples[i] * VOLUME_GAIN;

      if (temp > 32767) temp = 32767;
      if (temp < -32768) temp = -32768;

      samples[i] = temp;
    }

    size_t w;
    i2s_write(I2S_NUM_0, buf, r, &w, portMAX_DELAY);
  }

  f.close();
}

// =======================================================
// 🔊 DESTINATION VOICE (YOUR FUNCTION)
// =======================================================
void speakDestination(String name) {
  name.toLowerCase();

  if (name.indexOf("site") >= 0) playWav("/site_college.wav");
  else if (name.indexOf("hostel") >= 0) playWav("/vhr_boys_hostel.wav");
  else if (name.indexOf("science") >= 0) playWav("/subharti_science_college.wav");
  else if (name.indexOf("placement") >= 0) playWav("/placement_cell_department.wav");
  else if (name.indexOf("pharmacy") >= 0) playWav("/subharti_pharmacy_college.wav");
  else if (name.indexOf("polytechnic") >= 0) playWav("/subharti_polytechnic_college.wav");
  else if (name.indexOf("library") >= 0) playWav("/central_library.wav");
  else if (name.indexOf("medical") >= 0) playWav("/subharti_medical_college.wav");
  else if (name.indexOf("dental") >= 0) playWav("/subharti_dental_college.wav");
  else if (name.indexOf("vc") >= 0) playWav("/vc_office.wav");
  else if (name.indexOf("convention") >= 0) playWav("/mangalya.wav");
  else if (name.indexOf("law") >= 0) playWav("/law.wav");
  else playWav("/unknown.wav");
}

// =======================================================
// 📡 FETCH DEST
// =======================================================
void fetchDestinations() {
  lcd.clear();
  lcd.print("Loading...");
  Serial.println("Fetching...");

  if (Firebase.RTDB.getJSON(&fbdo, "/predefined_destinations")) {
    FirebaseJson json;
    json.setJsonData(fbdo.payload());

    FirebaseJsonData data;
    totalDest = 0;

    for (int i = 1; i <= 20; i++) {
      String base = "[" + String(i) + "]";

      if (json.get(data, base + "/name")) {
        names[totalDest] = data.stringValue;

        Serial.println(names[totalDest]);  // debug

        totalDest++;
      }
    }

    Serial.print("Loaded: ");
    Serial.println(totalDest);

    // 🔥 DISPLAY + SPEAK FIRST DESTINATION
    lcd.clear();

    if (totalDest > 0) {
      lcd.print(names[0].substring(0, 16));
      speakDestination(names[0]);  // 🔊 SPEAKER OUTPUT
    } else {
      lcd.print("No Data");
      playWav("/error.wav");
    }
  } else {
    lcd.clear();
    lcd.print("Firebase Error");

    Serial.println("Firebase FAILED");
    Serial.println(fbdo.errorReason());

    playWav("/error.wav");
  }
}

// =======================================================
// 🔘 BUTTONS

void resetNavigation() {
  // 🔄 Reset Firebase destination
  Firebase.RTDB.setString(&fbdo, "/navigation_device/destination/name", "");
  Firebase.RTDB.setString(&fbdo, "/navigation_device/instruction", "");
  Firebase.RTDB.setInt(&fbdo, "/navigation_device/distance", 0);

  // 🔁 Reset local state
  mode = MENU;
  currentIndex = 0;

  lcd.clear();
  lcd.print("Navigation Reset");

  playWav("/navigation_reset.wav");  // 🔊 add this file

  delay(1500);

  // 🔙 Show menu again
  lcd.clear();
  if (totalDest > 0) {
    lcd.print(names[0].substring(0, 16));
    speakDestination(names[0]);
  }
}

// =======================================================
void handleButtons() {
  if (digitalRead(BTN_DOWN) == LOW) {
    currentIndex = (currentIndex + 1) % totalDest;
    lcd.clear();
    lcd.print(names[currentIndex].substring(0, 16));
    speakDestination(names[currentIndex]);
    delay(300);
  }

  if (digitalRead(BTN_UP) == LOW) {
    unsigned long pressTime = millis();

    while (digitalRead(BTN_UP) == LOW) {
      delay(10);
    }

    // 🔥 LONG PRESS → RESET NAVIGATION
    if (millis() - pressTime > 1500) {
      resetNavigation();
      return;
    }

    // 🔁 SHORT PRESS → NORMAL SCROLL
    currentIndex--;
    if (currentIndex < 0) currentIndex = totalDest - 1;

    lcd.clear();
    lcd.print(names[currentIndex].substring(0, 16));
    speakDestination(names[currentIndex]);

    delay(300);
  }

  if (digitalRead(BTN_SEL) == LOW) {
    unsigned long t = millis();
    while (digitalRead(BTN_SEL) == LOW)
      ;

    if (millis() - t > 1500) {
      lcd.clear();
      lcd.print("Voice Server");
      lcd.setCursor(0, 1);
      lcd.print("Down");
      playWav("/voice_server_failed.wav");
      delay(2000);
    } else {
      Firebase.RTDB.setString(&fbdo, "/navigation_device/destination/name", names[currentIndex]);

      lcd.clear();
      lcd.print("Selected:");
      lcd.setCursor(0, 1);
      lcd.print(names[currentIndex].substring(0, 16));

      speakDestination(names[currentIndex]);

      mode = WAIT_GPS;
    }
  }
}

// =======================================================
// 📡 GPS SEND
// =======================================================
void sendGPS() {
  if (gps.location.isValid() && millis() - lastGPSsend > 2000) {
    Firebase.RTDB.setDouble(&fbdo, "/navigation_device/location/lat", gps.location.lat());
    Firebase.RTDB.setDouble(&fbdo, "/navigation_device/location/lon", gps.location.lng());
    lastGPSsend = millis();
  }
}

// =======================================================
// 🧭 NAVIGATION
// =======================================================
void navigationLoop() {
  Firebase.RTDB.getString(&fbdo, "/navigation_device/instruction");
  instruction = fbdo.stringData();

  Firebase.RTDB.getInt(&fbdo, "/navigation_device/distance");
  distance = fbdo.intData();

  static String lastText = "";
  String currentText = instruction + String(distance);

  if (currentText != lastText) {
    lcd.clear();
    lcd.print(instruction.substring(0, 16));
    lcd.setCursor(0, 1);
    lcd.print(String(distance) + " m");
    lastText = currentText;
  }

  if (distance < 10) {
    lcd.clear();
    lcd.print("Arrived");
    playWav("/dest_reach.wav");
    mode = MENU;
    return;
  }

  if (distance > lastDistance + 20) {
    lcd.clear();
    lcd.print("Wrong Direction");
    playWav("/wrong_direction.wav");
    Firebase.RTDB.setBool(&fbdo, "/navigation_device/reroute", true);
  }

  lastDistance = distance;

  if (millis() - lastVoice > 4000) {
    if (instruction == "LEFT") playWav("/left.wav");
    else if (instruction == "RIGHT") playWav("/right.wav");
    else playWav("/forward.wav");

    lastVoice = millis();
  }
}

// =======================================================
// 🟢 SETUP
// =======================================================
void setup() {
  Serial.begin(115200);

  pinMode(LED_WIFI, OUTPUT);
  pinMode(LED_GPS, OUTPUT);
  pinMode(LED_SYS, OUTPUT);

  Wire.begin();
  mpu.initialize();

  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_SEL, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lcd.print("Connecting WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_WIFI, !digitalRead(LED_WIFI));
    delay(500);
  }

  digitalWrite(LED_WIFI, HIGH);

  lcd.clear();
  lcd.print("WiFi Connected");
  playWav("/wifi.wav");
  delay(1500);

  setupI2S();
  SD.begin(SD_CS);

  playWav("/welcome_to.wav");
  playWav("/btm1_dest.wav");
  playWav("/btm2_previos.wav");
  playWav("/bt3_sel_nest.wav");
  playWav("/btm3_input.wav");


  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  // 🔥 REQUIRED FOR ESP32 AUTH
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase SignUp OK");
  } else {
    Serial.printf("SignUp failed: %s\n", config.signer.signupError.message.c_str());
  }

  // Start Firebase
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  fetchDestinations();

  lcd.clear();
  if (totalDest > 0) {
    lcd.print(names[0].substring(0, 16));
    speakDestination(names[0]);
  } else lcd.print("No Data");

  digitalWrite(LED_SYS, HIGH);

  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);

  while (!Firebase.ready()) {
    Serial.print(".");
    delay(200);
  }
  Serial.println("\nFirebase Ready");
  playWav("/firebase_ready.wav");
}

// =======================================================
// 🔁 LOOP
// =======================================================
void loop() {
  while (gpsSerial.available())
    gps.encode(gpsSerial.read());

  sendGPS();

  // 🔥 ALWAYS CHECK BUTTONS (GLOBAL CONTROL)
  handleButtons();

  switch (mode) {
    case MENU:
      break;

    case WAIT_GPS:
      if (!gps.location.isValid()) {
        lcd.clear();
        lcd.print("Waiting GPS...");
      } else {
        digitalWrite(LED_GPS, HIGH);
        mode = NAVIGATION;
      }
      break;

    case NAVIGATION:
      navigationLoop();
      break;
  }
}
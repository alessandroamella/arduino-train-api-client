#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <time.h>

// Include your custom DMD library and a font
#include "fonts/Arial14.h"
#include "fonts/SystemFont5x7.h"
#include <DMD32.h>
#include <secrets.h>

// =================================================================
// WIFI & API CONFIGURATION
// =================================================================
const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;

// Default to Castelfranco Emilia if TRAIN_STATION_CODE is not defined
#ifndef TRAIN_STATION_CODE
#define TRAIN_STATION_CODE "S05037"
#endif

const char *apiUrl =
    "https://arduino-train-api.bitrey.it/departures/" TRAIN_STATION_CODE
    "?limit=5&key=" API_KEY;

// =================================================================
// DISPLAY CONFIGURATION
// =================================================================
#define DISPLAYS_ACROSS 2
#define DISPLAYS_DOWN 1
DMD dmd(DISPLAYS_ACROSS,
        DISPLAYS_DOWN); // Using default pins as per your README

#define TEXT_Y_POS 2     // Y position for Arial14 font
#define TEXT_Y_SYS_POS 4 // Y position for System5x7 font

#define TRAIN_DEP_TIME_X_OFFSET 8

// =================================================================
// CURRENT FONT TRACKING
// =================================================================
enum FontType { FONT_ARIAL_14, FONT_SYSTEM_5X7 };
FontType currentFont = FONT_ARIAL_14;
int currentYOffset = TEXT_Y_POS;

// =================================================================
// TIMER FOR DMD REFRESH ISR
// =================================================================
hw_timer_t *dmd_timer = NULL;

// =================================================================
// TIME & DATA MANAGEMENT
// =================================================================
unsigned long lastDataFetch = 0;
const long fetchInterval = 5 * 60 * 1000; // 5 minutes in milliseconds

// Timezone configuration for Italy (CET/CEST with automatic DST)
const char *TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3"; // Europe/Rome timezone

// =================================================================
// Local clock variables, synced from NTP with timezone
// =================================================================
int currentHour = 0;
int currentMinute = 0;
int currentSecond = 0;

// =================================================================
// Data Storage
// =================================================================
String weatherString = "Loading...";
String stationName = ""; // Station name from API

// =================================================================
// Train data structure
// =================================================================
struct TrainInfo {
  String type;
  String destination;
  String departureTime;
  String delay;
};
std::vector<TrainInfo> departures;

// =================================================================
// Display State Machine
// =================================================================
enum DisplayState {
  STATE_SHOW_TIME,
  STATE_SHOW_WEATHER,
  STATE_SHOW_DEPARTURES_HEADER,
  STATE_SHOW_DEPARTURES
};
DisplayState currentState = STATE_SHOW_TIME;
// DisplayState currentState = STATE_SHOW_DEPARTURES_HEADER; // debug
unsigned long stateChangeTimestamp = 0;
int currentTrainIndex = 0;

// =================================================================
// Train icon bitmap (16x16 pixels)
// =================================================================
const unsigned char trainIconBitmap[] PROGMEM = {
    0xf0, 0x07, 0xc0, 0x03, 0x80, 0x01, 0x9e, 0x79, 0x9e, 0x79, 0x9e,
    0x79, 0x9e, 0x79, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x98, 0x19,
    0x98, 0x19, 0x88, 0x11, 0xc0, 0x03, 0xf3, 0xcf, 0xe7, 0xe7};

// =================================================================
// DMD REFRESH ISR
// This function is called by a hardware timer to refresh the display
// =================================================================
void IRAM_ATTR triggerScan() { dmd.scanDisplayBySPI(); }

// =================================================================
// Constanti
// =================================================================
const unsigned long TIME_DISPLAY_DURATION = 10000; // 10 secondi
const unsigned long INFO_HOLD_DURATION = 2500;     // 2.5 secondi

// =================================================================
// WIFI CONNECTION - ROBUST VERSION
// =================================================================
bool connectToWiFiRobust(int maxRetries = 3) {
  for (int retry = 0; retry < maxRetries; retry++) {
    Serial.printf("\n=== WiFi Connection Attempt %d/%d ===\n", retry + 1,
                  maxRetries);

    // FULL reset del WiFi stack
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(1000);

    // Riaccendi WiFi
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("ESP32-Train-Board"); // Imposta hostname

    // Power management aggressivo per stabilità
    esp_wifi_set_ps(WIFI_PS_NONE); // Disabilita power saving

    WiFi.begin(ssid, password);

    // Attendi connessione
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected!");
      Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());

      // Forza DNS multipli
      IPAddress dns1(8, 8, 8, 8);
      IPAddress dns2(1, 1, 1, 1);
      WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, dns1, dns2);

      delay(2000); // Dai tempo al DNS di inizializzare

      Serial.printf("DNS1: %s\n", WiFi.dnsIP(0).toString().c_str());
      Serial.printf("DNS2: %s\n", WiFi.dnsIP(1).toString().c_str());

      return true;
    }

    Serial.println("\n✗ Failed, retrying...");
    delay(2000);
  }

  return false;
}

// =================================================================
// FORWARD DECLARATIONS
// =================================================================
void setFont(FontType font);
void fetchData();
void displayScrollingText(const String &text, int left = (32 * DISPLAYS_ACROSS),
                          int top = -1);
void animateSlideUp(const String &outgoingText, const String &incomingText);
void animateTrainSlideUp(const TrainInfo *outgoingTrain,
                         const TrainInfo *incomingTrain);

// =================================================================
// SETUP
// =================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n=== Train Board Starting ===");

  // Connessione robusta
  if (!connectToWiFiRobust(3)) {
    Serial.println("\n!!! FATAL: Cannot connect to WiFi !!!");
    Serial.println("Restarting in 10 seconds...");
    delay(10000);
    ESP.restart();
  }

  // Configure the timer (but don't start it yet)
  dmd_timer = timerBegin(40000); // 40kHz timer frequency

  if (dmd_timer) {
    // Attach the ISR function to the timer
    timerAttachInterrupt(dmd_timer, &triggerScan);
    Serial.println("DMD refresh timer configured");
  }

  // Initialize the display BEFORE starting the timer
  dmd.clearScreen(true);
  delay(100);

  // =================================================================
  // TIMEZONE & NTP SETUP
  // =================================================================
  // Configure the ESP32's internal NTP client.
  configTime(0, 0, "pool.ntp.org");

  // Set the timezone environment variable.
  setenv("TZ", TZ_INFO, 1);
  tzset();
  Serial.println(
      "Timezone configured for Europe/Rome (CET/CEST with automatic DST)");

  // Wait for the time to be synchronized.
  struct tm timeinfo;
  Serial.print("Waiting for NTP time sync");
  while (!getLocalTime(&timeinfo)) {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("\nTime synchronized!");

  // Now that we have the time, get the initial values.
  getLocalTime(&timeinfo); // Call it again to populate the struct
  currentHour = timeinfo.tm_hour;
  currentMinute = timeinfo.tm_min;
  currentSecond = timeinfo.tm_sec;
  Serial.printf("Initial time: %02d:%02d:%02d\n", currentHour, currentMinute,
                currentSecond);

  // Fetch initial data BEFORE starting the timer
  fetchData();

  // Start the timer at the END of setup (as per demo)
  if (dmd_timer) {
    timerAlarm(dmd_timer, 12, true, 0);
    Serial.println("DMD refresh timer started");
  }

  // Set the starting timestamp for the first state
  stateChangeTimestamp = millis();
}

// =================================================================
// MAIN LOOP
// =================================================================
void loop() {
  // Check WiFi health ogni tanto
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 30000) { // Ogni 30 secondi
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("!!! WiFi disconnected in loop !!!");
      if (!connectToWiFiRobust(2)) {
        Serial.println("Cannot recover, restarting...");
        delay(5000);
        ESP.restart();
      }
    }
    lastWiFiCheck = millis(); // Aggiorna DOPO il check
  }

  // Check if it's time to fetch new data
  if (millis() - lastDataFetch >= fetchInterval) {
    fetchData();
  }

  // Get local time with timezone applied
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    currentHour = timeinfo.tm_hour;
    currentMinute = timeinfo.tm_min;
    currentSecond = timeinfo.tm_sec;
  }

  // Run the display state machine

  switch (currentState) {
  case STATE_SHOW_TIME: {
    // Variabili statiche che mantengono lo stato tra le chiamate al loop
    static unsigned long enterTime = 0;
    static bool firstEntry = true;
    static int lastDisplayedSecond = -1;

    // Entrata nello stato (una volta sola)
    if (stateChangeTimestamp != 0) {
      enterTime = millis();     // Salva quando siamo entrati
      stateChangeTimestamp = 0; // Reset del flag
      firstEntry = true;        // Marca come prima entry
      lastDisplayedSecond = -1; // Forza ridisegno immediato
      dmd.clearScreen(true);    // Pulisci schermo
      setFont(FONT_ARIAL_14);   // Imposta font grande
      Serial.println("Entered STATE_SHOW_TIME");
    }

    // Aggiornamento dell'ora (ogni secondo)
    // Ridisegna solo se il secondo è cambiato
    if (currentSecond != lastDisplayedSecond) {
      dmd.clearScreen(true);

      // Crea la stringa dell'ora formato HH:MM:SS
      char timeBuffer[9];
      sprintf(timeBuffer, "%02d:%02d:%02d", currentHour, currentMinute,
              currentSecond);

      // Disegna l'ora al centro (circa)
      dmd.drawString(10, currentYOffset, timeBuffer, strlen(timeBuffer),
                     GRAPHICS_NORMAL);

      lastDisplayedSecond =
          currentSecond; // Ricorda quale secondo abbiamo mostrato

      if (firstEntry) {
        Serial.printf("Displaying time: %s\n", timeBuffer);
        firstEntry = false;
      }
    }

    // Uscita dallo stato (dopo 10 secondi)
    if (millis() - enterTime > TIME_DISPLAY_DURATION) {
      Serial.println("Time display duration elapsed, moving to weather");
      currentState = STATE_SHOW_WEATHER;
      stateChangeTimestamp = millis(); // Segnala cambio stato
      lastDisplayedSecond = -1;        // Reset per la prossima volta
    }
    break;
  }

  case STATE_SHOW_WEATHER: {
    // Per il meteo, lo scroll va ancora bene perché può essere lungo
    setFont(FONT_ARIAL_14); // Ensure normal font for weather
    dmd.clearScreen(true);
    displayScrollingText(weatherString);
    currentState = STATE_SHOW_DEPARTURES_HEADER;
    stateChangeTimestamp = millis();
    break;
  }

  case STATE_SHOW_DEPARTURES_HEADER: {
    dmd.clearScreen(true);
    delay(50); // Brief pause to ensure clear completes
    setFont(FONT_SYSTEM_5X7);

    // Disegna l'icona del treno
    dmd.drawBitmap(0, 0, trainIconBitmap, 16, 16, GRAPHICS_NORMAL);

    // Scroll the station name on the second line
    String text = "Treni da " + (stationName.length() > 0 ? stationName : "CF");
    displayScrollingText(text);

    currentState = STATE_SHOW_DEPARTURES;
    stateChangeTimestamp = millis();
    break;
  }

  case STATE_SHOW_DEPARTURES: {
    if (departures.empty()) {
      dmd.clearScreen(true);
      dmd.drawString(2, 0, "Nessun", 6, GRAPHICS_NORMAL);
      dmd.drawString(2, 8, "treno :(", 8, GRAPHICS_NORMAL);
      delay(INFO_HOLD_DURATION);
      currentState = STATE_SHOW_TIME;
      // Usa un valore non-zero per triggerare il redraw dell'ora
      stateChangeTimestamp = 1;
      break;
    }

    static int lastShownTrainIndex = -1;

    if (currentTrainIndex < departures.size()) {
      setFont(FONT_SYSTEM_5X7); // Usa il font più piccolo

      TrainInfo &train = departures[currentTrainIndex];

      // Se è il primo treno, mostralo direttamente senza animazione
      if (lastShownTrainIndex == -1) {
        dmd.clearScreen(true);

        // Prima riga: destinazione
        dmd.drawString(2, 0, train.destination.c_str(),
                       train.destination.length(), GRAPHICS_NORMAL);

        // Seconda riga: orario e ritardo
        String timeAndDelay = train.departureTime + " " + train.delay;
        dmd.drawString(TRAIN_DEP_TIME_X_OFFSET, 8, timeAndDelay.c_str(),
                       timeAndDelay.length(), GRAPHICS_NORMAL);

        delay(INFO_HOLD_DURATION * 1.5);
        lastShownTrainIndex = currentTrainIndex;
        currentTrainIndex++;
      } else {
        // Anima dalla entry precedente a quella corrente
        TrainInfo *prevTrain = &departures[lastShownTrainIndex];
        animateTrainSlideUp(prevTrain, &train);

        // Tieni ferma la nuova entry per un po'
        delay(INFO_HOLD_DURATION * 1.5);
        lastShownTrainIndex = currentTrainIndex;
        currentTrainIndex++;
      }

      // Se era l'ultimo treno, torna al font normale
      if (currentTrainIndex >= departures.size()) {
        setFont(FONT_ARIAL_14);
      }

    } else {
      currentState = STATE_SHOW_TIME;
      // Usa un valore non-zero per triggerare il redraw dell'ora
      stateChangeTimestamp = 1;
      currentTrainIndex = 0;    // Reset per il prossimo ciclo
      lastShownTrainIndex = -1; // Reset anche questo
    }
    break;
  }
  }
}

// =================================================================
// HELPER FUNCTIONS
// =================================================================

/**
 * @brief Changes the display font and updates the current Y offset.
 * @param font The font to switch to (FONT_ARIAL_14, or FONT_SYSTEM_5X7).
 */
void setFont(FontType font) {
  currentFont = font;

  switch (font) {
  case FONT_ARIAL_14:
    dmd.selectFont(Arial_14);
    currentYOffset = TEXT_Y_POS;
    break;
  case FONT_SYSTEM_5X7:
    dmd.selectFont(System5x7);
    currentYOffset = TEXT_Y_SYS_POS;
    break;
  }
}

/**
 * @brief Fetches data from the API and parses the JSON response.
 */
void fetchData() {
  Serial.println("Fetching new data...");

  // Check WiFi PRIMA di tentare HTTP
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping fetch");
    weatherString = "WiFi Down";
    return;
  }

  HTTPClient http;
  http.setTimeout(15000); // Timeout esplicito

  if (!http.begin(apiUrl)) { // Check se begin() fallisce
    Serial.println("http.begin() failed (DNS?)");
    weatherString = "DNS Error";
    http.end();
    return;
  }

  Serial.print("Requesting URL: ");
  Serial.println(apiUrl);
  int httpCode = http.GET();

  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.println("Payload received:");
      Serial.println(payload);

      // Clear old train data
      departures.clear();
      departures.shrink_to_fit(); // Libera davvero la memoria

      // Parse JSON
      JsonDocument doc; // Allocate memory for the JSON object
      DeserializationError error = deserializeJson(doc, payload);

      if (error) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
        weatherString = "JSON Error";
        return;
      }

      // Parse weather
      String temp = doc["weather"]["temperature"];
      String desc = doc["weather"]["description"];
      temp.replace("^", "\xf8"); // Replace ^ with degree symbol if your font
                                 // supports it (SystemFont5x7 does)
      weatherString = temp + " - " + desc;

      // Parse station name
      const char *stationNameStr = doc["stationName"];
      if (stationNameStr) {
        stationName = String(stationNameStr);
        Serial.print("Station name: ");
        Serial.println(stationName);
      }

      // Parse departures
      JsonArray departuresArray = doc["departures"];
      for (JsonObject train : departuresArray) {
        TrainInfo newTrain;
        newTrain.type = train["type"].as<String>();
        newTrain.destination = "-> " + train["destination"].as<String>();
        newTrain.departureTime = train["departureTime"].as<String>();
        newTrain.delay = train["delay"].as<String>();
        departures.push_back(newTrain);
      }

      Serial.println("Data parsed successfully");

    } else {
      Serial.printf("[HTTP] GET... failed, error: %s\n",
                    http.errorToString(httpCode).c_str());
      weatherString = "HTTP Error " + String(httpCode);
    }
  } else {
    Serial.printf("[HTTP] GET... failed, error: %s\n",
                  http.errorToString(httpCode).c_str());
    weatherString = "Connection Failed";
  }

  http.end();
  lastDataFetch = millis();
}

/**
 * @brief Displays a string of text scrolling from right to left.
 * This is a blocking function and will run until the text has scrolled off
 * screen.
 * @param text The text to display.
 * @param left The left position where marquee starts (default: screen width).
 * @param top The top position where marquee is displayed (default:
 * currentYOffset).
 */
void displayScrollingText(const String &text, int left, int top) {
  // Use currentYOffset if top is not specified
  int yPos = (top == -1) ? currentYOffset : top;

  // Clear before starting marquee
  dmd.clearScreen(true);
  delay(10); // Brief pause

  dmd.drawMarquee(text.c_str(), text.length(), left, yPos);

  long start = millis();
  long timer = start;
  bool ret = false;
  while (!ret) {
    // Check for Wi-Fi connection or other background tasks if needed
    if ((millis() - timer) > 35) { // Control scroll speed
      ret = dmd.stepMarquee(-1, 0);
      timer = millis();
    }
  }

  // Clear after marquee completes
  dmd.clearScreen(true);
}

/**
 * @brief Anima una transizione "slide up" tra due stringhe di testo.
 * @param outgoingText Il testo che sta uscendo dallo schermo (verso l'alto).
 * @param incomingText Il testo che sta entrando nello schermo (dal basso).
 * @param animSpeed Velocità dell'animazione in millisecondi (valore più basso =
 * più veloce).
 */
void animateSlideUp(const String &outgoingText, const String &incomingText) {
  const int animSpeed = 25;
  const int screenHeight = 16; // Altezza standard di un pannello DMD

  for (int y = 0; y <= screenHeight; y++) {
    dmd.clearScreen(true);

    // Disegna il testo in uscita che scorre verso l'alto
    if (outgoingText.length() > 0) {
      dmd.drawString(2, currentYOffset - y, outgoingText.c_str(),
                     outgoingText.length(), GRAPHICS_NORMAL);
    }

    // Disegna il testo in entrata che scorre dal basso
    if (incomingText.length() > 0) {
      dmd.drawString(2, currentYOffset + screenHeight - y, incomingText.c_str(),
                     incomingText.length(), GRAPHICS_NORMAL);
    }

    delay(animSpeed);
  }
}

/**
 * @brief Anima una transizione "slide up" tra due entry di treni.
 * @param outgoingTrain Il treno che sta uscendo dallo schermo (verso l'alto).
 * @param incomingTrain Il treno che sta entrando nello schermo (dal basso).
 */
void animateTrainSlideUp(const TrainInfo *outgoingTrain,
                         const TrainInfo *incomingTrain) {
  const int animSpeed = 20;    // Velocità dell'animazione in ms
  const int screenHeight = 16; // Altezza standard di un pannello DMD

  // Prepara le stringhe per entrambi i treni
  String outDest = outgoingTrain ? outgoingTrain->destination : "";
  String outTime =
      outgoingTrain
          ? (outgoingTrain->departureTime + " " + outgoingTrain->delay)
          : "";

  String inDest = incomingTrain ? incomingTrain->destination : "";
  String inTime =
      incomingTrain
          ? (incomingTrain->departureTime + " " + incomingTrain->delay)
          : "";

  // Anima pixel per pixel
  for (int y = 0; y <= screenHeight; y++) {
    dmd.clearScreen(true);

    // Disegna il treno in uscita che scorre verso l'alto
    if (outgoingTrain && outDest.length() > 0) {
      // Destinazione (riga 1 -> sale)
      int outDestY = 0 - y;
      if (outDestY > -8) { // Solo se ancora visibile
        dmd.drawString(2, outDestY, outDest.c_str(), outDest.length(),
                       GRAPHICS_NORMAL);
      }

      // Orario e ritardo (riga 2 -> sale)
      int outTimeY = 8 - y;
      if (outTimeY > -8 && outTimeY < screenHeight) { // Solo se ancora visibile
        dmd.drawString(TRAIN_DEP_TIME_X_OFFSET, outTimeY, outTime.c_str(),
                       outTime.length(), GRAPHICS_NORMAL);
      }
    }

    // Disegna il treno in entrata che scorre dal basso
    if (incomingTrain && inDest.length() > 0) {
      // Destinazione (entra da sotto)
      int inDestY = screenHeight - y;
      if (inDestY < screenHeight && inDestY > -8) { // Solo se visibile
        dmd.drawString(2, inDestY, inDest.c_str(), inDest.length(),
                       GRAPHICS_NORMAL);
      }

      // Orario e ritardo (entra da sotto, 8px più in basso)
      int inTimeY = (screenHeight + 8) - y;
      if (inTimeY < screenHeight && inTimeY > -8) { // Solo se visibile
        dmd.drawString(TRAIN_DEP_TIME_X_OFFSET, inTimeY, inTime.c_str(),
                       inTime.length(), GRAPHICS_NORMAL);
      }
    }

    delay(animSpeed);
  }
}

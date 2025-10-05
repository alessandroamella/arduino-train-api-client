#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

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
const char *apiUrl = "https://arduino-train-api.bitrey.it/departures/"
                     "S05037?limit=5&key=" API_KEY;

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

// =================================================================
// Local clock variables, synced from the server
// =================================================================
int currentHour = 0;
int currentMinute = 0;
int currentSecond = 0;
unsigned long lastSecondTick = 0;
bool colonVisible = true;

// =================================================================
// Data Storage
// =================================================================
String weatherString = "Loading...";

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
// FORWARD DECLARATIONS
// =================================================================
void setFont(FontType font);
void fetchData();
void displayScrollingText(const String &text);
void animateSlideUp(const String &outgoingText, const String &incomingText);
void animateTrainSlideUp(const TrainInfo *outgoingTrain,
                         const TrainInfo *incomingTrain);

// =================================================================
// SETUP
// =================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\nStarting Train Departure Board...");

  // Configure the hardware timer for DMD refresh
  // The ESP32 has 4 hardware timers. timerBegin takes the timer frequency in
  // Hz. Let's aim for a high refresh rate. A frequency of 40000 Hz gives a 25us
  // period.
  dmd_timer =
      timerBegin(40000); // Returns a pointer to the timer object on success

  if (dmd_timer) {
    // Attach the ISR function to the timer
    timerAttachInterrupt(dmd_timer, &triggerScan);
  }

  // Initialize DMD and select font
  dmd.clearScreen(true);
  setFont(FONT_ARIAL_14);

  Serial.println("DMD Initialized.");

  // Connect to Wi-Fi
  Serial.println("Wi-Fiying...");
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected!");
  dmd.clearScreen(true);
  delay(1000);

  // Fetch initial data from the API
  fetchData();

  // Set the starting timestamp for the first state
  stateChangeTimestamp = millis();

  // Start the DMD refresh timer
  if (dmd_timer) {
    // Set the alarm period. The value is in timer ticks.
    // With a 40000 Hz frequency, 12 ticks gives an alarm every 12/40000 s = 300
    // us. The third boolean parameter 'true' makes it autoreload, and 0 means
    // unlimited reloads (periodic alarm).
    timerAlarm(dmd_timer, 12, true, 0);
    Serial.println("DMD refresh timer started.");
  }
}

// =================================================================
// MAIN LOOP
// =================================================================
void loop() {
  // Check if it's time to fetch new data
  if (millis() - lastDataFetch >= fetchInterval) {
    fetchData();
  }

  // Update local clock every second
  if (millis() - lastSecondTick >= 1000) {
    lastSecondTick = millis();
    currentSecond++;
    if (currentSecond >= 60) {
      currentSecond = 0;
      currentMinute++;
      if (currentMinute >= 60) {
        currentMinute = 0;
        currentHour++;
        if (currentHour >= 24) {
          currentHour = 0;
        }
      }
    }
  }

  // Run the display state machine
  // Definiamo costanti per la leggibilità
  const unsigned long TIME_DISPLAY_DURATION = 10000; // 10 secondi
  const unsigned long INFO_HOLD_DURATION = 2500;     // 2.5 secondi

  switch (currentState) {
  case STATE_SHOW_TIME: {
    static unsigned long enterTime = 0;
    if (stateChangeTimestamp != 0) { // Entra solo la prima volta
      enterTime = millis();
      stateChangeTimestamp = 0; // Resetta il flag
    }

    // Aggiorna l'ora ogni secondo mentre siamo in questo stato
    static int lastDisplayedSecond = -1;
    if (currentSecond != lastDisplayedSecond) {
      dmd.clearScreen(true);
      char timeBuffer[9];
      sprintf(timeBuffer, "%02d:%02d:%02d", currentHour, currentMinute,
              currentSecond);
      dmd.drawString(11, currentYOffset, timeBuffer, strlen(timeBuffer),
                     GRAPHICS_NORMAL);
      lastDisplayedSecond = currentSecond;
    }

    if (millis() - enterTime > TIME_DISPLAY_DURATION) {
      currentState = STATE_SHOW_WEATHER;
      stateChangeTimestamp = millis();
    }
    break;
  }

  case STATE_SHOW_WEATHER: {
    // Per il meteo, lo scroll va ancora bene perché può essere lungo
    setFont(FONT_ARIAL_14); // Ensure normal font for weather
    displayScrollingText(weatherString);
    currentState = STATE_SHOW_DEPARTURES_HEADER;
    stateChangeTimestamp = millis();
    break;
  }

  case STATE_SHOW_DEPARTURES_HEADER: {
    dmd.clearScreen(true);
    setFont(FONT_SYSTEM_5X7);

    // Disegna l'icona del treno
    dmd.drawBitmap(8, 0, trainIconBitmap, 16, 16, GRAPHICS_NORMAL);

    String header = "Treni";
    String footer = "da CF";
    dmd.drawString(32, 0, header.c_str(), header.length(), GRAPHICS_NORMAL);
    dmd.drawString(32, 8, footer.c_str(), footer.length(), GRAPHICS_NORMAL);

    delay(INFO_HOLD_DURATION);
    currentState = STATE_SHOW_DEPARTURES;
    stateChangeTimestamp = millis();
    break;
  }

  case STATE_SHOW_DEPARTURES: {
    if (departures.empty()) {
      dmd.clearScreen(true);
      dmd.drawString(2, 0, "Nessun treno", 12, GRAPHICS_NORMAL);
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
  dmd.clearScreen(true);
  dmd.drawString(2, TEXT_Y_POS, "Updating", 8, GRAPHICS_NORMAL);

  HTTPClient http;
  http.begin(apiUrl);
  int httpCode = http.GET();

  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.println("Payload received:");
      Serial.println(payload);

      // Clear old train data
      departures.clear();

      // Parse JSON
      JsonDocument doc; // Allocate memory for the JSON object
      DeserializationError error = deserializeJson(doc, payload);

      if (error) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
        weatherString = "JSON Error";
        return;
      }

      // Sync local time
      const char *timeStr = doc["time"]; // "19:10" or "19:10:30"
      if (timeStr) {
        // Try to parse with seconds first, if it fails parse without seconds
        int parsed = sscanf(timeStr, "%d:%d:%d", &currentHour, &currentMinute,
                            &currentSecond);
        if (parsed < 3) {
          // No seconds in the time string, set to 0
          sscanf(timeStr, "%d:%d", &currentHour, &currentMinute);
          currentSecond = 0;
        }
        Serial.printf("Time synced: %02d:%02d:%02d\n", currentHour,
                      currentMinute, currentSecond);
      }

      // Parse weather
      String temp = doc["weather"]["temperature"];
      String desc = doc["weather"]["description"];
      temp.replace("^", "\xf8"); // Replace ^ with degree symbol if your font
                                 // supports it (SystemFont5x7 does)
      weatherString = temp + " - " + desc;

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

      Serial.println("Data parsed successfully.");

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
 */
void displayScrollingText(const String &text) {
  dmd.clearScreen(true);
  // Use currentYOffset based on the selected font
  dmd.drawMarquee(text.c_str(), text.length(), (32 * DISPLAYS_ACROSS),
                  currentYOffset);

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

# Train departure display for ESP32

This sketch displays live train departures and weather information on a P10 LED matrix using an ESP32 and the [DMD32-v3](https://github.com/alessandroamella/DMD32-v3) library.

It connects to Wi-Fi, fetches data from a simple train live departures API, and cycles through different display states: current time, weather, and a list of upcoming train departures. The display is refreshed by a hardware timer interrupt for smooth animation.

## Requirements

- ESP32 board
- One or more P10 (HUB75) LED panels
- 5V power supply
- Working Wi-Fi connection

## Libraries

- ArduinoJson
- HTTPClient
- WiFi
- DMD32
- Fonts: `SystemFont5x7`, `Arial14`

## Wiring

Explained in the [DMD32-v3](https://github.com/alessandroamella/DMD32-v3) library README.

I followed the same wiring as the one from a GitHub issue by user [RARadchenko](https://github.com/RARadchenko), you can take a look [here](https://github.com/Qudor-Engineer/DMD32/issues/25).

## Configuration

Create a `include/secrets.h` file with your Wi-Fi and API information:

```cpp
#define WIFI_SSID "your_wifi_name"
#define WIFI_PASSWORD "your_wifi_password"
#define API_KEY "your_api_key"

// Optional: Uncomment to change the train station
// #define TRAIN_STATION_CODE "S05037"  // Defaults to Castelfranco Emilia
```

## Notes

- Data is fetched every 5 minutes.
- The local clock is synced from the API response.
- You can adjust the number of connected panels by changing `DISPLAYS_ACROSS` and `DISPLAYS_DOWN`.
- Display durations and animation speeds can be tuned in the `loop()` section.

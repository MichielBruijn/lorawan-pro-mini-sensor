# LoRaWAN Pro Mini Sensor

Arduino Pro Mini 3.3V 8MHz + SX1276 (RFM95W) temperature and humidity sensor node. Sends DHT11 data over LoRaWAN (OTAA, EU868) every 30 minutes. Sleep current ~5µA.

## Hardware

| Component | Notes |
|-----------|-------|
| Arduino Pro Mini 3.3V 8MHz | LED and onboard regulator removed |
| SX1276 / RFM95W | Powered via 7 GPIO pins in parallel |
| DHT11 | Temperature and humidity sensor |
| 18650 Li-ion cell | Direct to VCC pin (no regulator) |

## Wiring

| Pro Mini | SX1276 |
|----------|--------|
| 13 (SCK) | SCK |
| 12 (MISO) | MISO |
| 11 (MOSI) | MOSI |
| 10 (SS) | NSS |
| 2 (INT0) | DIO0 |
| 5 | DIO1 |
| 6 | RESET |
| 3, 7, 8, A0, A1, A2, A3 | VCC (all 7 pins to SX1276 Vcc) |
| GND | GND |

| Pro Mini | Other |
|----------|-------|
| 4 | DHT11 data |
| 9 | LED (debug, optional) |

Feed the board via the **VCC pin**, not RAW. The onboard regulator and power LED must be removed for ~5µA sleep.

## Power

The SX1276 has no onboard LDO to remove, so it is powered directly from 7 GPIO pins wired in parallel. Each GPIO sources up to 40mA, giving ~280mA combined — enough for the SX1276 TX peak of ~120mA at +14dBm. The pins are driven LOW after transmission to cut power completely during sleep.

Sleep current breakdown:

| Component | Current |
|-----------|---------|
| ATmega328P power-down | ~0.4µA |
| SX1276 (GPIO power off) | 0µA |
| DHT11 idle | ~0.5µA |
| **Total** | **~5µA** |

At 5µA sleep and 30-minute intervals an 18650 (3000mAh) lasts several years.

## LMIC Configuration

In `libraries/MCCI_LoRaWAN_LMIC_library/project_config/lmic_project_config.h`:

```cpp
#define CFG_eu868 1
#define CFG_sx1276_radio 1
#define DISABLE_PING 1
#define DISABLE_BEACONS 1
#define DISABLE_JOIN_LEAVE_CHANNEL 1
#define LMIC_ENABLE_arbitrary_clock_error 1
```

## Credentials

Copy `secrets.h.example` to `secrets.h` and fill in your TTN device keys:

```bash
cp secrets.h.example secrets.h
```

- **APPEUI / DEVEUI**: LSB first (reverse byte order from TTN console)
- **APPKEY**: MSB first (as shown in TTN console)

`secrets.h` is excluded from version control via `.gitignore`.

## Libraries

- [MCCI LoRaWAN LMIC Library](https://github.com/mcci-catena/arduino-lmic)
- [LowPower](https://github.com/rocketscream/Low-Power)
- [DHT sensor library](https://github.com/adafruit/DHT-sensor-library)

## Payload Format

4 bytes, big endian:

| Bytes | Type | Value |
|-------|------|-------|
| 0–1 | int16 | temperature × 10 (e.g. 23.4°C → 0x00EA) |
| 2–3 | uint16 | humidity × 10 (e.g. 42.0% → 0x01A4) |

## How It Works

Each cycle the sketch runs entirely in `setup()`:

1. Flash LED once (sign of life)
2. Power on SX1276 via GPIO pins
3. LMIC OTAA join + send DHT11 payload
4. `LMIC_shutdown()`, GPIO power off, SPI pins to INPUT
5. Watchdog sleep 225 × 8s = 30 minutes
6. `jmp 0` soft reset — LMIC reinitialises cleanly on next boot

`loop()` is never reached.

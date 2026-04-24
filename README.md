# slim-lora-868

LoRaWAN temperature and humidity node. Arduino Pro Mini 3.3V 8MHz + RFM95W (SX1276). Sends DHT11 data every 30 minutes. Sleep current ~5µA. Total cost ~€10.

Uses [SlimLoRa](https://github.com/novatechweb/SlimLoRa) — a minimal OTAA LoRaWAN library for ATmega that stores the session in EEPROM. The device joins TTN once and resumes without rejoining after a power cycle or reset.

## Hardware

| Component | Price |
|-----------|-------|
| Arduino Pro Mini 3.3V 8MHz | ~€3 |
| RFM95W (SX1276) LoRa module | ~€5 |
| DHT11 sensor breakout (3-pin) | ~€1.50 |
| AMS1117-3.3 or similar LDO | ~€0.20 |
| 18650 Li-ion cell | ~€2 |

Remove the onboard power LED and voltage regulator from the Pro Mini. Power the board via the **VCC pin** at 3.3V, not RAW.

## Wiring

| Pro Mini | RFM95W |
|----------|--------|
| 13 (SCK) | SCK |
| 12 (MISO) | MISO |
| 11 (MOSI) | MOSI |
| 10 (SS) | NSS/CS |
| 2 (INT0) | DIO0 |
| 3 | DIO1 |
| 9 | RESET (or tie RESET to VCC via 10kΩ) |
| 3.3V | VCC |
| GND | GND |

| Pro Mini | DHT11 |
|----------|-------|
| 4 | DATA |
| 3.3V | VCC |
| GND | GND |

## Power

The RFM95W is always connected to 3.3V. SlimLoRa puts it into hardware sleep mode after each transmission. During sleep:

| Component | Current |
|-----------|---------|
| ATmega328P power-down | ~0.4µA |
| RFM95W sleep mode | ~0.2µA |
| DHT11 idle | ~0.5µA |
| LDO quiescent | ~1–3µA |
| **Total** | **~5µA** |

At ~5µA average sleep and 30-minute intervals, a 3000mAh 18650 lasts several years.

## Session Persistence

SlimLoRa stores the LoRaWAN session (DevAddr, NwkSKey, AppSKey, frame counters) in EEPROM after a successful join. On the next boot the session is restored and the device goes straight to transmitting — no rejoin needed, even after a battery swap.

To force a fresh join, clear EEPROM byte 0 (e.g. with the Arduino `eeprom_write_test` sketch or `EEPROM.write(0, 0)` at startup).

## Credentials

```bash
cp secrets.h.example secrets.h
```

Fill in your TTN device keys — all **MSB first**, exactly as shown in the TTN console:

- **DevEUI**: 8 bytes MSB first
- **JoinEUI** (AppEUI): 8 bytes MSB first
- **AppKey**: 16 bytes MSB first

`secrets.h` is excluded from version control via `.gitignore`.

## Library

Install **SlimLoRa** via the Arduino Library Manager. No other LoRaWAN or DHT library needed — DHT11 is bit-banged directly in the sketch.

## Payload Format

2 bytes:

| Byte | Value |
|------|-------|
| 0 | Temperature (°C, integer) or `0xFF` on read error |
| 1 | Humidity (%, integer) or `0xFF` on read error |

TTN payload decoder (JavaScript):

```js
function decodeUplink(input) {
  return {
    data: {
      temperature: input.bytes[0] === 0xFF ? null : input.bytes[0],
      humidity:    input.bytes[1] === 0xFF ? null : input.bytes[1]
    }
  };
}
```

## How It Works

1. Boot: SlimLoRa checks EEPROM for a saved session
2. No valid session → OTAA join (retries with random backoff until a gateway responds)
3. Session saved to EEPROM on successful join
4. `loop()`: read DHT11 → send unconfirmed uplink on port 1 → sleep 30 min → repeat
5. Sleep: ATmega in power-down mode, RFM95W in hardware sleep, SPI/ADC/timers disabled

`setup()` only runs the join logic once. `loop()` handles measurements forever.

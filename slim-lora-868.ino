/*
 * SlimLoRa DHT11 sensor node — Arduino Pro Mini 3.3V/8MHz
 *
 * Hardware:
 *   - Arduino Pro Mini 3.3V 8MHz (ATmega328P)
 *   - RFM95W (SX1276) LoRa module
 *   - DHT11 breakout board (3-pin: VCC, DATA, GND)
 *
 * Bedrading:
 *   RFM95W        Pro Mini
 *   ------        --------
 *   VCC           3.3V
 *   GND           GND
 *   MISO          D12
 *   MOSI          D11
 *   SCK           D13
 *   NSS/CS        D10
 *   DIO0          D2
 *   DIO1          D3
 *   RESET         D9  (optioneel, kan ook aan VCC via 10k pullup)
 *
 *   DHT11         Pro Mini
 *   -----         --------
 *   VCC           3.3V (of VCC pin)
 *   DATA          D4
 *   GND           GND
 *
 * Powersaving tips:
 *   - Desoldeer de power LED op de Pro Mini
 *   - Desoldeer/bypass de voltage regulator als je direct 3.3V voert
 *   - Dat haalt je sleep-verbruik van ~4mA naar ~4µA
 *
 * TTN payload decoder (JavaScript):
 *   function decodeUplink(input) {
 *     return {
 *       data: {
 *         temperature: input.bytes[0] === 0xFF ? null : input.bytes[0],
 *         humidity:    input.bytes[1] === 0xFF ? null : input.bytes[1]
 *       }
 *     };
 *   }
 */

#include <SlimLoRa.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <avr/power.h>

// ─── DHT11 config ───────────────────────────────────────────────
// Geen library nodig, we bit-bangen het zelf. Scheelt flash/RAM.
#define DHT_PIN 4

// ─── SlimLoRa config ────────────────────────────────────────────
// NSS pin = D10
SlimLoRa lora = SlimLoRa(10);

// ─── TTN/ChirpStack keys ────────────────────────────────────────
#include "secrets.h"

// ─── Sleep config ───────────────────────────────────────────────
// WDT @ 8s interval. 225 × 8s = 1800s = 30 minuten
#define SLEEP_CYCLES 225

volatile uint16_t wdt_count = 0;

ISR(WDT_vect) {
    wdt_count++;
}

// ─── Minimale DHT11 reader (geen library nodig) ────────────────
// Retourneert true bij succes, vult temp en hum in.
bool read_dht11(uint8_t &temp, uint8_t &hum) {
    uint8_t data[5] = {0};

    // Start signaal: pull low 18ms, dan high 40µs
    pinMode(DHT_PIN, OUTPUT);
    digitalWrite(DHT_PIN, LOW);
    delay(20);
    digitalWrite(DHT_PIN, HIGH);
    delayMicroseconds(40);
    pinMode(DHT_PIN, INPUT_PULLUP);

    // Wacht op DHT response (low 80µs, high 80µs)
    uint8_t timeout = 100;
    while (digitalRead(DHT_PIN) == HIGH) {
        if (--timeout == 0) return false;
        delayMicroseconds(1);
    }
    timeout = 100;
    while (digitalRead(DHT_PIN) == LOW) {
        if (--timeout == 0) return false;
        delayMicroseconds(1);
    }
    timeout = 100;
    while (digitalRead(DHT_PIN) == HIGH) {
        if (--timeout == 0) return false;
        delayMicroseconds(1);
    }

    // Lees 40 bits (5 bytes)
    for (uint8_t i = 0; i < 40; i++) {
        // Wacht op rising edge
        timeout = 100;
        while (digitalRead(DHT_PIN) == LOW) {
            if (--timeout == 0) return false;
            delayMicroseconds(1);
        }
        // Meet hoe lang high duurt: >40µs = 1, <40µs = 0
        delayMicroseconds(30);
        if (digitalRead(DHT_PIN) == HIGH) {
            data[i / 8] |= (1 << (7 - (i % 8)));
        }
        // Wacht tot pin weer low is
        timeout = 100;
        while (digitalRead(DHT_PIN) == HIGH) {
            if (--timeout == 0) return false;
            delayMicroseconds(1);
        }
    }

    // Checksum
    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        return false;
    }

    hum  = data[0];  // DHT11: alleen integer deel
    temp = data[2];
    return true;
}

// ─── Power management ───────────────────────────────────────────
void setup_watchdog_8s() {
    cli();
    MCUSR &= ~(1 << WDRF);
    WDTCSR |= (1 << WDCE) | (1 << WDE);
    WDTCSR = (1 << WDIE) | (1 << WDP3) | (1 << WDP0);  // 8s, interrupt mode
    sei();
}

void disable_watchdog() {
    cli();
    MCUSR &= ~(1 << WDRF);
    WDTCSR |= (1 << WDCE) | (1 << WDE);
    WDTCSR = 0x00;
    sei();
}

void power_down_sleep() {
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    cli();
    sleep_enable();
    sleep_bod_disable();
    sei();
    sleep_cpu();
    sleep_disable();
}

void sleep_30_minutes() {
    power_adc_disable();
    power_spi_disable();
    power_twi_disable();
    power_timer1_disable();
    power_timer2_disable();

    setup_watchdog_8s();
    wdt_count = 0;

    while (wdt_count < SLEEP_CYCLES) {
        power_down_sleep();
    }

    disable_watchdog();

    // Peripherals weer aan voor volgende meting/TX
    power_adc_enable();
    power_spi_enable();
    power_timer1_enable();
}

// ─── Setup ──────────────────────────────────────────────────────
void setup() {
    // Alle ongebruikte pins als input_pullup (minder lek-stroom)
    for (uint8_t i = 0; i <= A7; i++) {
        if (i == DHT_PIN) continue;
        if (i == 10 || i == 11 || i == 12 || i == 13) continue;
        if (i == 2 || i == 3 || i == 9) continue;
        pinMode(i, INPUT_PULLUP);
    }

    // ADC uit tot we hem nodig hebben
    ADCSRA &= ~(1 << ADEN);

    // SlimLoRa init
    lora.Begin();


    // OTAA Join — retry tot het lukt
    while (lora.Join() != 0) {
        delay(15000 + random(5000));
    }
}

// ─── Loop ───────────────────────────────────────────────────────
void loop() {
    uint8_t temp = 0;
    uint8_t hum  = 0;
    uint8_t payload[2];

    // DHT11 heeft even nodig om wakker te worden
    delay(1500);

    // Probeer max 3x uit te lezen
    bool ok = false;
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        if (read_dht11(temp, hum)) {
            ok = true;
            break;
        }
        delay(2100);  // DHT11 minimaal 2s tussen reads
    }

    if (ok) {
        payload[0] = temp;
        payload[1] = hum;
    } else {
        payload[0] = 0xFF;  // Error marker
        payload[1] = 0xFF;
    }

    // Unconfirmed uplink op port 1
    lora.SendData(1, payload, sizeof(payload));

    // Slaap 30 minuten
    sleep_30_minutes();
}
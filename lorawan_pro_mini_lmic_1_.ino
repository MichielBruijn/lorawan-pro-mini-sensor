/*
 * LoRaWAN DHT11 Sensor Node — Arduino Pro Mini 3.3V 8MHz
 * MCCI LMIC library, OTAA, SF12, ultra-low power (~5µA sleep)
 *
 * Radio (SX1276/RFM95W) wordt gevoed via 7 GPIO pinnen parallel.
 * Na LMIC_shutdown() gaat de MCU in watchdog power-down sleep.
 * Na de sleep-cyclus doet jmp 0 een soft reset met handmatige
 * SPI/timer register reset zodat LMIC schoon opstart.
 *
 * Hardware:
 *   Pro Mini  →  SX1276
 *   13 (SCK)  →  SCK
 *   12 (MISO) →  MISO
 *   11 (MOSI) →  MOSI
 *   10 (SS)   →  NSS
 *    2 (INT0) →  DIO0
 *    5        →  DIO1
 *    6        →  RESET
 *    4        →  DHT11 data
 *    9        →  LED (debug)
 *    3,7,8,A0,A1,A2,A3 → SX1276 VCC (parallel)
 *
 * Regulator en power LED moeten verwijderd zijn voor laag slaapverbruik.
 * Voed via VCC pin, niet RAW.
 *
 * LMIC config (project_config/lmic_project_config.h):
 *   #define CFG_eu868 1
 *   #define CFG_sx1276_radio 1
 *   #define DISABLE_PING 1
 *   #define DISABLE_BEACONS 1
 *   #define DISABLE_JOIN_LEAVE_CHANNEL 1
 *   #define LMIC_ENABLE_arbitrary_clock_error 1
 */

#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <LowPower.h>
#include <DHT.h>

// ── Pin definities ──────────────────────────────────────────
#define DHTPIN    4
#define LED_PIN   9

// ── Radio voeding via GPIO ──────────────────────────────────
uint8_t pwrPins[] = {3, 7, 8, A0, A1, A2, A3};
#define NUM_PWR_PINS 7

void radioOn() {
  for (uint8_t i = 0; i < NUM_PWR_PINS; i++) {
    pinMode(pwrPins[i], OUTPUT);
    digitalWrite(pwrPins[i], HIGH);
  }
  delay(50);
}

// ── Sleep configuratie ──────────────────────────────────────
#define SLEEP_CYCLES 225  // 225 x 8s = 30 minuten

// ── LoRaWAN credentials (OTAA) ──────────────────────────────
// Defined in secrets.h (not committed to git — see secrets.h.example)
// APPEUI & DEVEUI: LSB! APPKEY: MSB!
#include "secrets.h"

void os_getArtEui(u1_t* buf) { memcpy_P(buf, APPEUI, 8); }
void os_getDevEui(u1_t* buf) { memcpy_P(buf, DEVEUI, 8); }
void os_getDevKey(u1_t* buf) { memcpy_P(buf, APPKEY, 16); }

// ── LMIC pin mapping ────────────────────────────────────────
const lmic_pinmap lmic_pins = {
  .nss  = 10,
  .rxtx = LMIC_UNUSED_PIN,
  .rst  = 6,
  .dio  = {2, 5, LMIC_UNUSED_PIN},
};

// ── State ───────────────────────────────────────────────────
DHT dht(DHTPIN, DHT11);
static osjob_t sendjob;
volatile bool txComplete = false;

// ── Sleep & reset ───────────────────────────────────────────
void doSleep() {
  LMIC_shutdown();
  
  // 1. Zet de radio fysiek uit
  for (uint8_t i = 0; i < NUM_PWR_PINS; i++) {
    digitalWrite(pwrPins[i], LOW);
  }

  // 2. Zet SPI pinnen op INPUT om lekstroom via de datalijnen te voorkomen
  pinMode(10, INPUT); // NSS
  pinMode(11, INPUT); // MOSI
  pinMode(12, INPUT); // MISO
  pinMode(13, INPUT); // SCK

  // 3. Slaap cyclus
  for (uint16_t i = 0; i < SLEEP_CYCLES; i++) {
    LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);
  }

  // 4. Reset peripherals voor de jmp 0
  SPCR = 0; SPSR = 0;
  TCCR0A = 0; TCCR0B = 0; TCNT0 = 0;
  TCCR1A = 0; TCCR1B = 0; TCNT1 = 0;
  TCCR2A = 0; TCCR2B = 0; TCNT2 = 0;

  asm volatile ("jmp 0");
}

// ── LMIC events ─────────────────────────────────────────────
void onEvent(ev_t ev) {
  switch (ev) {
    case EV_JOINED:     LMIC_setLinkCheckMode(0); break;
    case EV_JOIN_FAILED: doSleep(); break;
    case EV_TXCOMPLETE: txComplete = true; break;
    default: break;
  }
}

// ── Zend data ───────────────────────────────────────────────
void doSend(osjob_t* j) {
  if (LMIC.opmode & OP_TXRXPEND) return;

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  uint8_t payload[4];
  int16_t  temp = (int16_t)(t * 10.0);
  uint16_t humi = (uint16_t)(h * 10.0);
  payload[0] = temp >> 8;
  payload[1] = temp & 0xFF;
  payload[2] = humi >> 8;
  payload[3] = humi & 0xFF;

  LMIC_setTxData2(1, payload, sizeof(payload), 0);
}

// ── Setup ───────────────────────────────────────────────────
void setup() {
  SPCR = 0;
  SPSR = 0;

  // LED flash als teken van leven
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  delay(100);
  digitalWrite(LED_PIN, LOW);

  radioOn();
  dht.begin();
  delay(2000);

  os_init();
  LMIC_reset();
  LMIC_setClockError(MAX_CLOCK_ERROR * 10 / 100);
  LMIC_setDrTxpow(DR_SF12, 14);
  LMIC_setAdrMode(0);
  LMIC.dn2Dr = DR_SF9;

  txComplete = false;
  doSend(&sendjob);

  while (!txComplete) {
    os_runloop_once();
  }

  doSleep();
}

void loop() {}
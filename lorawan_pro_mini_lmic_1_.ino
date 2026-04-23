/*
 * LoRaWAN DHT11 Sensor Node — Arduino Pro Mini 3.3V 8MHz
 *
 * Reads temperature and humidity from a DHT11 sensor and transmits
 * the data via LoRaWAN (OTAA, EU868, SF12) to TTN every 30 minutes.
 * Sleep current is ~5µA.
 *
 * Power strategy:
 *   The SX1276 module has no onboard LDO to remove, so it is powered
 *   via 7 GPIO pins wired in parallel to its Vcc. Each GPIO sources
 *   up to 40mA; 7 pins give ~280mA — enough for the TX peak at +14dBm.
 *   After TX the pins are driven LOW to cut all current to the radio.
 *   The ATmega328P then enters watchdog power-down sleep (~0.4µA).
 *   After the sleep period a soft reset via jmp 0 restarts the sketch
 *   cleanly — LMIC cannot resume from sleep, so a full reinit is needed.
 *
 *   The onboard regulator and power LED must be physically removed.
 *   Power the board via the VCC pin directly from the 18650, not RAW.
 *
 * Wiring:
 *   Pro Mini      SX1276
 *   13 (SCK)   →  SCK
 *   12 (MISO)  →  MISO
 *   11 (MOSI)  →  MOSI
 *   10 (SS)    →  NSS
 *    2 (INT0)  →  DIO0
 *    5         →  DIO1
 *    6         →  RESET
 *    3,7,8,A0,A1,A2,A3 → VCC (all 7 pins to SX1276 Vcc)
 *    GND       →  GND
 *
 *    4         →  DHT11 data
 *    9         →  LED anode (debug, optional)
 *
 * Required LMIC config in
 * libraries/MCCI_LoRaWAN_LMIC_library/project_config/lmic_project_config.h:
 *   #define CFG_eu868 1
 *   #define CFG_sx1276_radio 1
 *   #define DISABLE_PING 1
 *   #define DISABLE_BEACONS 1
 *   #define DISABLE_JOIN_LEAVE_CHANNEL 1
 *   #define LMIC_ENABLE_arbitrary_clock_error 1
 *
 * Credentials: copy secrets.h.example to secrets.h and fill in your TTN keys.
 */

#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <LowPower.h>
#include <DHT.h>

// ── Pin definitions ─────────────────────────────────────────────────────────
#define DHTPIN   4   // DHT11 data pin
#define LED_PIN  9   // Debug LED (optional, blinks once on boot)

// ── Radio power supply via GPIO ─────────────────────────────────────────────
// Seven pins wired in parallel to the SX1276 Vcc.
// Driven HIGH to power on, LOW to cut power completely during sleep.
uint8_t pwrPins[] = {3, 7, 8, A0, A1, A2, A3};
#define NUM_PWR_PINS 7

void radioOn() {
  for (uint8_t i = 0; i < NUM_PWR_PINS; i++) {
    pinMode(pwrPins[i], OUTPUT);
    digitalWrite(pwrPins[i], HIGH);
  }
  delay(50);  // allow SX1276 Vcc to stabilize
}

// ── Sleep duration ───────────────────────────────────────────────────────────
// The watchdog timer maxes out at 8s per cycle. 225 cycles = 30 minutes.
#define SLEEP_CYCLES 225

// ── LoRaWAN credentials (OTAA) ──────────────────────────────────────────────
// Defined in secrets.h (not committed to git — see secrets.h.example).
// APPEUI & DEVEUI must be in LSB-first byte order.
// APPKEY must be in MSB-first byte order (as shown in TTN console).
#include "secrets.h"

// LMIC credential callbacks — called by the LMIC stack during join.
void os_getArtEui(u1_t* buf) { memcpy_P(buf, APPEUI, 8); }
void os_getDevEui(u1_t* buf) { memcpy_P(buf, DEVEUI, 8); }
void os_getDevKey(u1_t* buf) { memcpy_P(buf, APPKEY, 16); }

// ── LMIC pin mapping ─────────────────────────────────────────────────────────
const lmic_pinmap lmic_pins = {
  .nss  = 10,               // SPI chip select
  .rxtx = LMIC_UNUSED_PIN,  // no TX/RX switch
  .rst  = 6,                // radio reset
  .dio  = {2, 5, LMIC_UNUSED_PIN},  // DIO0, DIO1, DIO2
};

// ── Global state ─────────────────────────────────────────────────────────────
DHT dht(DHTPIN, DHT11);
static osjob_t sendjob;
volatile bool txComplete = false;  // set by onEvent(EV_TXCOMPLETE)

// ── Sleep and soft reset ─────────────────────────────────────────────────────
void doSleep() {
  // Shut down the LMIC stack cleanly before cutting power.
  LMIC_shutdown();

  // Cut power to the SX1276 by driving all supply pins LOW.
  for (uint8_t i = 0; i < NUM_PWR_PINS; i++) {
    digitalWrite(pwrPins[i], LOW);
  }

  // Set SPI lines to INPUT so they cannot leak current into the
  // powered-down SX1276 through its internal ESD diodes.
  pinMode(10, INPUT);  // NSS
  pinMode(11, INPUT);  // MOSI
  pinMode(12, INPUT);  // MISO
  pinMode(13, INPUT);  // SCK

  // Enter watchdog power-down sleep. ADC and BOD are disabled for
  // lowest current. Each call sleeps 8 seconds; 225 cycles = 30 min.
  for (uint16_t i = 0; i < SLEEP_CYCLES; i++) {
    LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);
  }

  // Reset all hardware peripherals before the soft reset so that LMIC
  // finds them in a known state on the next boot. Without this, leftover
  // SPI or timer state from the previous run causes LMIC to hang.
  SPCR = 0; SPSR = 0;                          // SPI
  TCCR0A = 0; TCCR0B = 0; TCNT0 = 0;           // Timer 0
  TCCR1A = 0; TCCR1B = 0; TCNT1 = 0;           // Timer 1
  TCCR2A = 0; TCCR2B = 0; TCNT2 = 0;           // Timer 2

  // Jump to address 0: equivalent to a soft reset. Restarts setup()
  // without a full power cycle. LMIC does not support waking from sleep,
  // so a full reinitialisation is the correct approach.
  asm volatile ("jmp 0");
}

// ── LMIC event handler ───────────────────────────────────────────────────────
void onEvent(ev_t ev) {
  switch (ev) {
    case EV_JOINED:
      // Disable link check mode — we never request downlinks.
      LMIC_setLinkCheckMode(0);
      break;
    case EV_JOIN_FAILED:
      // No gateway response; go back to sleep and retry next cycle.
      doSleep();
      break;
    case EV_TXCOMPLETE:
      // Uplink (and optional downlink window) finished.
      txComplete = true;
      break;
    default:
      break;
  }
}

// ── Transmit payload ─────────────────────────────────────────────────────────
void doSend(osjob_t* j) {
  // Do not queue a new TX while one is still pending.
  if (LMIC.opmode & OP_TXRXPEND) return;

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  // Encode as 4 bytes, big endian:
  //   bytes 0-1: temperature * 10, signed int16  (e.g. 23.4°C → 0x00EA)
  //   bytes 2-3: humidity    * 10, unsigned uint16 (e.g. 42.0% → 0x01A4)
  uint8_t payload[4];
  int16_t  temp = (int16_t)(t * 10.0);
  uint16_t humi = (uint16_t)(h * 10.0);
  payload[0] = temp >> 8;
  payload[1] = temp & 0xFF;
  payload[2] = humi >> 8;
  payload[3] = humi & 0xFF;

  // Send confirmed=false uplink on FPort 1.
  LMIC_setTxData2(1, payload, sizeof(payload), 0);
}

// ── Main ─────────────────────────────────────────────────────────────────────
// Everything runs in setup(). loop() is never reached because doSleep()
// ends with jmp 0, restarting setup() after the sleep period.
void setup() {
  // Clear SPI registers that may have been left in a dirty state by the
  // previous soft reset. Must happen before LMIC initialises the SPI bus.
  SPCR = 0;
  SPSR = 0;

  // Flash LED once as a visual sign of life on each boot.
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  delay(100);
  digitalWrite(LED_PIN, LOW);

  // Power on the SX1276 and wait for the DHT11 to stabilize.
  radioOn();
  dht.begin();
  delay(2000);  // DHT11 needs up to 2s after power-on before first reading

  // Initialise LMIC and configure radio parameters.
  os_init();
  LMIC_reset();

  // Allow 10% clock error to compensate for the 8MHz crystal tolerance
  // on the Pro Mini, which widens the RX windows and prevents missed downlinks.
  LMIC_setClockError(MAX_CLOCK_ERROR * 10 / 100);

  LMIC_setDrTxpow(DR_SF12, 14);  // SF12, +14dBm (max range)
  LMIC_setAdrMode(0);             // fixed data rate, no ADR
  LMIC.dn2Dr = DR_SF9;           // RX2 window data rate (TTN default)

  // Queue the first (and only) uplink, then run the LMIC event loop
  // until TX and the RX windows are complete.
  txComplete = false;
  doSend(&sendjob);
  while (!txComplete) {
    os_runloop_once();
  }

  // Transmission done — power down until next cycle.
  doSleep();
}

void loop() {}

#include <DS2438.h>
#include <Adafruit_SHT31.h>

// To turn on DEBUG, define it in the common header:
#include <onewire-common.h>

// 1-Wire pin
#define PIN_ONE_WIRE 11

// How often to take readings (in ms)
#define READ_INTERVAL 3000

// How long do we calculate delta over?
#define DELTA_INTERVAL 60000

// So how many readings do we need to store?
// Should be DELTA_INTERVAL / DELTA_INTERVAL + 1
#define DELTA_COUNT 21

// And rolling buffer
struct Reading {
  unsigned long timestamp;
  float temperature;
  float humidity; 
};
struct Reading deltas[DELTA_COUNT];

// Init SHT31
Adafruit_SHT31 sht31 = Adafruit_SHT31();

// Init Hub
OneWireHub hub = OneWireHub(PIN_ONE_WIRE);
DS2438 *ds2438;

// 1W address
uint8_t addr_1w[7];

// Starting conditions
boolean first = true;
int8_t current_slot = 0;

void zero1w(DS2438 *device) {
  // Zero out, but according to what we think 'zero' is ;)
  device->setTemperature((int8_t) 0);
  device->setVDDVoltage((uint16_t) 512);
  device->setVADVoltage((uint16_t) 512);
  device->setCurrent((int16_t) -1023);
}

void setup() {
  #ifdef DEBUG    
    Serial.begin(115200);
    Serial.println("OneWire-Hub SHT31 sensor");
  #endif

  error_flash(1000);

  if (!sht31.begin(0x44)) {   // Set to 0x45 for alternate i2c addr
    #ifdef DEBUG    
      Serial.println("Couldn't find SHT31");
    #endif
    digitalWrite(LED_BUILTIN, 1);
    while (1) delay(1);
  }

  get_address(addr_1w);

  ds2438 = new DS2438(DS2438::family_code, addr_1w[1], addr_1w[2], addr_1w[3], addr_1w[4], addr_1w[5], addr_1w[6]);
  zero1w(ds2438);
  hub.attach(*ds2438);

  #ifdef DEBUG
    dumpAddress("1-Wire DS2438 address: ", ds2438, "");
  #endif
}

// Function to return number of next/prev slots (just inc/dec but account for wrap).
int8_t next_slot(int8_t slot) {
  return (++slot) % DELTA_COUNT;
}
int8_t prev_slot(int8_t slot) {
  return (slot > 0) ? (slot - 1) : (DELTA_COUNT - 1);
}

// You'd think that one could just use the oldest slot but... timing of
// hub.poll isn't guaranteed to be 'quick' so it's possible that it could delay
// things and that we should use one of the previous slots.
//
// Yes, this is a little convoluted/complicated but really - what else is this
// CPU gonna be doing? ;)

int8_t find_interval_reading(unsigned long old_stamp) {
  #ifdef DEBUG
    Serial.print("Looking for reading before "); Serial.println(old_stamp);
  #endif
  int8_t found_slot = -1;
  int8_t check_slot = prev_slot(current_slot);
  do {
    // To match, must be older than interval, but not too old to stop wrapping timestamps
    if (deltas[check_slot].timestamp < old_stamp && old_stamp - deltas[check_slot].timestamp < DELTA_INTERVAL * 2) {
      // Found it
      #ifdef DEBUG
        Serial.print("Found it at "); Serial.println(deltas[check_slot].timestamp );
      #endif
      found_slot = check_slot;
    } else {
      check_slot = prev_slot(check_slot);
    }
  } while (check_slot != current_slot && found_slot < 0);
  return found_slot;
}

// Function to place current reading in slot and work out delta

uint16_t delta_calc(float current_value, float old_value) {
  float absolute_delta = current_value - old_value;
  float pct_delta = absolute_delta / (old_value / 100);

  #ifdef DEBUG
    Serial.print("Old: "); Serial.print(old_value);
    Serial.print(" New: "); Serial.print(current_value);
    Serial.print(" Abs: "); Serial.print(absolute_delta);
    Serial.print(" Pct: "); Serial.print(pct_delta);
  #endif

  // Delta percent needs to be 0 -> 1023 so scale 10.23x
  // 0 -> 1023 becomes -50% -> +50%
  pct_delta *= 10.23;
  pct_delta += 512;

  // Cast to final return value (within limits)
  uint16_t delta = pct_delta < 0 ? 0 : pct_delta > 1023 ? 1023 : pct_delta;
  #ifdef DEBUG
    Serial.print(" Int: "); Serial.println(delta);
  #endif

  return delta;
}

// Main loop

unsigned long next_reading = 0;
void loop() {
  // Handle 1-Wire traffic
  hub.poll();

  // Return if it's not time for the next reading yet
  if (millis() < next_reading) return;

  // Time to get readings

  // Work out next time
  unsigned long this_stamp = millis();
  next_reading = this_stamp + READ_INTERVAL;

  #ifdef DEBUG
    Serial.print("Slot: "); Serial.println(current_slot);
  #endif
  
  float temperature = sht31.readTemperature();
  float humidity = sht31.readHumidity();
  if (isnan(temperature) || isnan(humidity)) {
    // Something bad - flash LED and return
    error_flash(2000);
    #ifdef DEBUG
      Serial.println("Failed to read sensor");
    #endif
    return;
  }

  if (first) {
    // Fill up the delta buffer with initial values
    for (int8_t lp = 0; lp < DELTA_COUNT; lp++) {
      deltas[lp].timestamp = this_stamp;
      deltas[lp].temperature = temperature;
      deltas[lp].humidity = humidity;
    }
    // We're not on the first reading any more
    first = false;
  } else {
    // Find a value old enough to create delta from
    int8_t period_slot = find_interval_reading(this_stamp - DELTA_INTERVAL);

    if (period_slot < 0) {
      // No slot old enough found
      #ifdef DEBUG
        Serial.println("No reading old enough for interval");
      #endif
    } else {
      #ifdef DEBUG
        Serial.print("Using old values from slot "); Serial.println(period_slot);
      #endif
      ds2438->setVDDVoltage(delta_calc(temperature, deltas[period_slot].temperature));
      ds2438->setVADVoltage(delta_calc(humidity, deltas[period_slot].humidity));
    }
  }

  // Always set current values
  deltas[current_slot].timestamp = this_stamp;
  deltas[current_slot].temperature = temperature;
  deltas[current_slot].humidity = humidity;
  // Move to the next slot for deltas
  current_slot = next_slot(current_slot);

  ds2438->setTemperature(temperature);
  
  // Have to scale humidity to fit in current (between -1023 & +1023)
  // So divide multiply by 20.46 and subtract 1023
  // This ends up in Loxone between -0.25 & +0.25
  humidity *= 20.46;
  humidity -= 1023;
  int16_t humidity_raw = humidity;
  #ifdef DEBUG
    Serial.print("Raw humidity is "); Serial.println(humidity_raw);
  #endif
  ds2438->setCurrent(humidity_raw);
}
/*
 * Arduino Wire I2C master example
 *
 * Talks to a register-map slave at address 0x50:
 *   1. Write bytes to register 0x00
 *   2. Read them back
 *   3. Print results via Serial
 *
 * Equivalent to the Arduino Wire master_writer/master_reader examples.
 */
#include <Wire.h>

#define SLAVE_ADDR 0x50

void setup()
{
    Serial.begin(9600);
    Wire.begin();  /* Join I2C bus as master */

    delay(100);
    Serial.println("Wire I2C test");

    /* --- Write test data to register 0x00 --- */
    Serial.println("Writing...");
    Wire.beginTransmission(SLAVE_ADDR);
    Wire.write(0x00);   /* Register address */
    Wire.write(0xDE);
    Wire.write(0xAD);
    Wire.write(0xBE);
    Wire.write(0xEF);
    byte err = Wire.endTransmission();
    Serial.print("  endTransmission: ");
    Serial.println(err);

    /* --- Read back 4 bytes from register 0x00 --- */
    Serial.println("Reading...");

    /* Set register pointer */
    Wire.beginTransmission(SLAVE_ADDR);
    Wire.write(0x00);
    Wire.endTransmission(false);  /* Repeated START */

    Wire.requestFrom(SLAVE_ADDR, 4);
    byte buf[4];
    int i = 0;
    while (Wire.available() && i < 4) {
        buf[i] = Wire.read();
        Serial.print("  [");
        Serial.print(i);
        Serial.print("] = 0x");
        if (buf[i] < 0x10) Serial.print("0");
        Serial.println(buf[i], HEX);
        i++;
    }

    /* --- Verify --- */
    if (buf[0] == 0xDE && buf[1] == 0xAD &&
        buf[2] == 0xBE && buf[3] == 0xEF) {
        Serial.println("PASS");
    } else {
        Serial.println("FAIL");
    }
}

void loop()
{
    /* Nothing — test runs once in setup */
}

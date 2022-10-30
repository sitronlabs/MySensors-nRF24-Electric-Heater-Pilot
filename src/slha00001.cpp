/* Config */
#include "../cfg/config.h"

/* Arduino Libraries */
#include <Arduino.h>
#include <MySensors.h>
#include <Wire.h>
#include <aht20.h>

/* Volative working variables */
static bool m_heating_enabled = false;
static float m_heating_target = 19.0;
static aht20 m_sensor;
static float m_temperature_offset = 0;
static float m_temperature = 0;
static float m_humidity = 0;

/* Wireless messages */
static MyMessage m_message_mode(0, V_HVAC_FLOW_STATE);
static MyMessage m_message_temperature_target(0, V_HVAC_SETPOINT_HEAT);
static MyMessage m_message_temperature_measured(0, V_TEMP);
static MyMessage m_message_humidity_measured(1, V_HUM);

/**
 *
 */
void setup_failed(const char *const reason) {
    Serial.println(reason);
    Serial.flush();
    while (1) {
        sleep(0, false);
    }
}

/**
 *
 */
void setup() {

    /* Setup leds */
    pinMode(CONFIG_PERIPH_LED_RED_PIN, OUTPUT);
    pinMode(CONFIG_PERIPH_LED_YELLOW_PIN, OUTPUT);
    pinMode(CONFIG_PERIPH_LED_GREEN_PIN, OUTPUT);
    digitalWrite(CONFIG_PERIPH_LED_RED_PIN, HIGH);
    digitalWrite(CONFIG_PERIPH_LED_YELLOW_PIN, HIGH);
    digitalWrite(CONFIG_PERIPH_LED_GREEN_PIN, LOW);

    /* Setup serial */
    Serial.begin(115200);
    Serial.println(" [i] Hello world.");

    /* Setup i2c */
    Wire.begin();

    /* Setup temperature sensor */
    m_sensor.setup(Wire);
    if (!m_sensor.detect()) {
        setup_failed(" [e] Failed to detect temperature sensor!");
        return;
    }

    /* Setup triac pins */
    pinMode(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, OUTPUT);
    pinMode(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, OUTPUT);
    digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, HIGH);
    digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, LOW);

    /* Turn off red and yellow led to indicate setup done */
    digitalWrite(CONFIG_PERIPH_LED_RED_PIN, LOW);
    digitalWrite(CONFIG_PERIPH_LED_YELLOW_PIN, LOW);
}

/**
 * MySensors function called to describe this sensor and its capabilites.
 * @note Ideally, this sensor should present itself as a S_HEATER, but currently only S_HVAC is supported by Home Assistant.
 */
void presentation() {
    sendSketchInfo("SLHA00001 Electric Heater", "0.1.0");
    present(0, S_HVAC);  // V_STATUS, V_TEMP, V_HVAC_SETPOINT_HEAT, V_HVAC_SETPOINT_COOL, V_HVAC_FLOW_STATE, V_HVAC_FLOW_MODE, V_HVAC_SPEED
    present(1, S_HUM);   // V_HUM
}

/**
 * MySensors function called when a message is received.
 */
void receive(const MyMessage &message) {

    /* Handle heater setpoint messages */
    if (message.getType() == V_HVAC_SETPOINT_HEAT) {
        float target = message.getFloat();
        if (target < 0 || target > 40) {
            Serial.println(" [e] Received invalid target!");
            return;
        }
        m_heating_target = target;
        Serial.print(" [i] Receveid new target of ");
        Serial.println(target);
    }

    /* Handle heater on/off messages */
    else if (message.getType() == V_HVAC_FLOW_STATE) {
        m_heating_enabled = (strcmp(message.getString(), "HeatOn") == 0 || strcmp(message.getString(), "AutoChangeOver") == 0) ? true : false;
        Serial.print(" [i] Receveid state ");
        Serial.println(m_heating_enabled ? "on" : "off");
    }

    /* */
    else if (message.getType() == V_VAR1) {
        float offset = message.getFloat();
        if (fabs(offset) <= 5) {
            m_temperature_offset = offset;
            Serial.print(" [e] Received new offset of ");
            Serial.println(offset);
        } else {
            Serial.println(" [e] Received invalid offset!");
        }
    }

    /* Handler other messages */
    else {
        Serial.print(" [e] Unexpected message type ");
        Serial.println(message.getType(), DEC);
    }
}

/**
 * Main loop.
 */
void loop() {
    int res;

    /* State machine */
    static enum {
        STATE_0,
        STATE_1,
        STATE_2,
        STATE_3,
        STATE_ERROR,
    } s_sm;
    switch (s_sm) {

        case STATE_0: {

            /* Reset red error led and
             * turn on green activity led */
            digitalWrite(CONFIG_PERIPH_LED_RED_PIN, LOW);
            digitalWrite(CONFIG_PERIPH_LED_GREEN_PIN, HIGH);

            /* Move on */
            s_sm = STATE_1;
            break;
        }

        case STATE_1: {

            /* Read temperature */
            res = m_sensor.measurement_sync_get(&m_temperature, &m_humidity);
            if (res < 0) {
                Serial.println(" [e] Failed to read from temperature sensor!");
                s_sm = STATE_ERROR;
                return;
            }

            /* Apply temperature offset */
            m_temperature += m_temperature_offset;

            /* Move on */
            s_sm = STATE_2;
            break;
        }

        case STATE_2: {

            /* Report info */
            send(m_message_mode.set(m_heating_enabled ? "HeatOn" : "Off"));
            send(m_message_temperature_target.set(m_heating_target, 2));
            send(m_message_temperature_measured.set(m_temperature, 2));
            send(m_message_humidity_measured.set(m_humidity * 100, 2));

            /* Move on */
            s_sm = STATE_3;
            break;
        }

        case STATE_3: {

            /* Turn off green activity led */
            digitalWrite(CONFIG_PERIPH_LED_GREEN_PIN, LOW);

            /* Generate signal */
            if (m_heating_enabled && m_temperature <= m_heating_target) {
                digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, LOW);
                digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, LOW);
            } else {
                digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, LOW);
                digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, HIGH);
            }

            /* Sleep */
            wait(10000);
            s_sm = STATE_0;
            break;
        }

        case STATE_ERROR:
        default: {

            /* Turn off heating */
            m_heating_enabled = false;
            digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, LOW);
            digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, HIGH);

            /* Light up error led */
            digitalWrite(CONFIG_PERIPH_LED_RED_PIN, HIGH);

            /* Sleep before retrying */
            wait(60000);
            s_sm = STATE_0;
            break;
        }
    }
}

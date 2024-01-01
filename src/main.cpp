/* Arduino Libraries */
#include <Arduino.h>
#include <MySensors.h>
#include <Wire.h>
#include <aht20.h>

/* Peripherals */
static aht20 m_sensor;

/* Work data */
static bool m_heating_enabled = false;
static uint32_t m_heating_timestamp;
static float m_temperature_target = 19.0;
static float m_temperature_measured = 0;
static float m_temperature_reported = 0;
static uint32_t m_temperature_report_timestamp = 0;
static float m_humidity_measured = 0;
static float m_humidity_reported = 0;
static uint32_t m_humidity_report_timestamp = 0;
static bool m_hvac_report_needed = true;

/* List of virtual sensors */
enum {
    SENSOR_0_HEATER,    // S_HVAC (V_STATUS, V_TEMP, V_HVAC_SETPOINT_HEAT, V_HVAC_SETPOINT_COOL, V_HVAC_FLOW_STATE, V_HVAC_FLOW_MODE, V_HVAC_SPEED)
    SENSOR_1_HUMIDITY,  // S_HUM (V_HUM)
};

/* Wireless messages */
static MyMessage m_message_mode(0, V_HVAC_FLOW_STATE);
static MyMessage m_message_temperature_target(0, V_HVAC_SETPOINT_HEAT);
static MyMessage m_message_temperature_measured(0, V_TEMP);
static MyMessage m_message_humidity_measured(1, V_HUM);

/**
 * Setup function.
 * Called before MySensors does anything.
 */
void preHwInit(void) {

    /* Setup leds */
    pinMode(CONFIG_PERIPH_LED_RED_PIN, OUTPUT);
    pinMode(CONFIG_PERIPH_LED_YELLOW_PIN, OUTPUT);
    pinMode(CONFIG_PERIPH_LED_GREEN_PIN, OUTPUT);
    digitalWrite(CONFIG_PERIPH_LED_RED_PIN, LOW);
    digitalWrite(CONFIG_PERIPH_LED_YELLOW_PIN, HIGH);
    digitalWrite(CONFIG_PERIPH_LED_GREEN_PIN, LOW);
}

/**
 * Called when setup() encounters an error.
 */
void setup_failed(const char *const reason) {

    /* Turn on error led */
    digitalWrite(CONFIG_PERIPH_LED_RED_PIN, HIGH);

    /* Print failure message */
    Serial.println(reason);
    Serial.flush();

    /* Wait forever */
    while (1) {
        sleep(0, false);
    }
}

/**
 * Setup function.
 * Called once MySensors has successfully initialized.
 */
void setup() {

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

    /* Turn off leds to indicate setup done */
    digitalWrite(CONFIG_PERIPH_LED_RED_PIN, LOW);
    digitalWrite(CONFIG_PERIPH_LED_YELLOW_PIN, LOW);
    digitalWrite(CONFIG_PERIPH_LED_GREEN_PIN, LOW);
}

/**
 * MySensors function called to describe this sensor and its capabilites.
 * @note Ideally, this sensor should present itself as a S_HEATER, but currently only S_HVAC is supported by Home Assistant.
 */
void presentation() {

    /* Because messages might be lost,
     * we're not doing the presentation in one block, but rather step by step,
     * making sure each step is sucessful before advancing to the next */
    for (int8_t step = -1;;) {

        /* Send out presentation information corresponding to the current step,
         * and advance one step if successful */
        switch (step) {
            case -1: {
                if (sendSketchInfo(F("SLHA00001 Electric Heater"), F("0.2.0")) == true) {
                    step++;
                }
                break;
            }
            case SENSOR_0_HEATER: {
                if (present(SENSOR_0_HEATER, S_HVAC, F("Chauffage")) == true) {  // V_STATUS, V_TEMP, V_HVAC_SETPOINT_HEAT, V_HVAC_SETPOINT_COOL, V_HVAC_FLOW_STATE, V_HVAC_FLOW_MODE, V_HVAC_SPEED
                    step++;
                }
                break;
            }
            case SENSOR_1_HUMIDITY: {
                if (present(SENSOR_1_HUMIDITY, S_HUM, F("Humidité")) == true) {  // V_HUM
                    step++;
                }
                break;
            }
            default: {
                return;
            }
        }

        /* Sleep a little bit after each presentation, otherwise the next fails
         * @see https://forum.mysensors.org/topic/4450/sensor-presentation-failure */
        sleep(50);
    }
}

/**
 * MySensors function called when a message is received.
 */
void receive(const MyMessage &message) {

    /* Handle heater setpoint messages */
    if ((message.sensor == SENSOR_0_HEATER) && (message.getType() == V_HVAC_SETPOINT_HEAT)) {
        float target = message.getFloat();
        if (target < 0 || target > 40) {
            Serial.println(F(" [e] Received invalid target!"));
            return;
        }
        m_temperature_target = target;
        m_hvac_report_needed = true;
        Serial.print(F(" [i] Receveid new target of "));
        Serial.println(target);
    }

    /* Handle heater on/off messages */
    else if ((message.sensor == SENSOR_0_HEATER) && (message.getType() == V_HVAC_FLOW_STATE)) {
        m_heating_enabled = (strcmp(message.getString(), "HeatOn") == 0 || strcmp(message.getString(), "AutoChangeOver") == 0) ? true : false;
        m_hvac_report_needed = true;
        Serial.print(F(" [i] Receveid state "));
        Serial.println(m_heating_enabled ? F("on") : F("off"));
    }

    /* Handler other messages */
    else {
        Serial.print(F(" [e] Unexpected message type "));
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
        STATE_READ_0,
        STATE_REPORT_0,
        STATE_HEAT_0,
        STATE_HEAT_1,
        STATE_ERROR_0,
        STATE_ERROR_1,
    } m_sm;
    switch (m_sm) {

        case STATE_READ_0: {

            /* Read temperature and humidity */
            res = m_sensor.measurement_sync_get(m_temperature_measured, m_humidity_measured);
            if (res < 0) {
                Serial.println(F(" [e] Failed to read from temperature sensor!"));
                m_sm = STATE_ERROR_0;
                return;
            }

            /* Move on */
            m_sm = STATE_REPORT_0;
            break;
        }

        case STATE_REPORT_0: {

            /* Attempt to report temperature */
            if ((m_hvac_report_needed == true) ||  //
                ((fabs(m_temperature_measured - m_temperature_reported) >= 0.1) && (millis() - m_temperature_report_timestamp >= 30000))) {
                if (send(m_message_temperature_measured.set(m_temperature_measured, 1)) == true) {
                    m_temperature_reported = m_temperature_measured;
                    m_temperature_report_timestamp = millis();
                }
            }

            /* Attempt to report humidity */
            if ((m_hvac_report_needed == true) ||  //
                ((fabs(m_humidity_measured - m_humidity_reported) >= 0.5) && (millis() - m_humidity_report_timestamp >= 30000))) {
                if (send(m_message_humidity_measured.set(m_humidity_measured, 1)) == true) {
                    m_humidity_reported = m_humidity_measured;
                    m_humidity_report_timestamp = millis();
                }
            }

            /* Attempt to report hvac status if needed */
            if (m_hvac_report_needed == true) {
                if (send(m_message_mode.set(m_heating_enabled ? F("HeatOn") : F("Off"))) == true &&  //
                    send(m_message_temperature_target.set(m_temperature_target, 1)) == true) {
                    m_hvac_report_needed = false;
                }
            }

            /* Move on */
            m_sm = STATE_HEAT_0;
            break;
        }

        case STATE_HEAT_0: {

            /* Stop here if there is no need to heat */
            if (m_heating_enabled == false || m_temperature_measured > m_temperature_target) {
                digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, LOW);
                digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, HIGH);
                m_sm = STATE_READ_0;
                break;
            }

            /* Turn on heating */
            digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, LOW);
            digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, LOW);

            /* Move on */
            m_heating_timestamp = millis();
            m_sm = STATE_HEAT_1;
            break;
        }

        case STATE_HEAT_1: {

            /* Wait 10 seconds */
            if (millis() - m_heating_timestamp < 10000) {
                break;
            }

            /* Move on */
            m_sm = STATE_READ_0;
            break;
        }

        case STATE_ERROR_0: {

            /* Turn off heating */
            digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, LOW);
            digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, HIGH);

            /* Turn on error led */
            digitalWrite(CONFIG_PERIPH_LED_RED_PIN, HIGH);

            /* Move on */
            m_heating_timestamp = millis();
            break;
        }

        case STATE_ERROR_1: {

            /* Wait 10 seconds */
            if (millis() - m_heating_timestamp < 10000) {
                break;
            }

            /* Move on */
            m_sm = STATE_READ_0;
            break;
        }
    }
}

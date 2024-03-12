/* Arduino Libraries */
#include <Arduino.h>
#include <MySensors.h>
#include <Wire.h>
#include <aht20.h>

/* Peripherals */
static aht20 m_sensor;

/* Work data */
static enum {
    CONTROL_MODE_NONE,
    CONTROL_MODE_ONOFF,
    CONTROL_MODE_THERMOSTAT,
} m_control_mode;
static bool m_control_onoff_heating = false;
static bool m_control_thermostat_heating = false;
static float m_control_thermostat_target = 19.0;
static bool m_report_needed = true;

/* List of virtual sensors */
enum {
    SENSOR_0_HUMIDITY,            // S_HUM (V_HUM)
    SENSOR_1_CONTROL_ONOFF,       // S_BINARY (V_STATUS)
    SENSOR_2_CONTROL_THERMOSTAT,  // S_HVAC (V_STATUS, V_TEMP, V_HVAC_SETPOINT_HEAT, V_HVAC_FLOW_STATE)
};

/**
 * Setup function.
 * Called before MySensors does anything.
 */
void preHwInit(void) {

    /* Setup leds
     * The yellow one is turned on here and will be turned off in setup() once MySensors sucessfully communicates with the controller */
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
                if (sendSketchInfo(F("SLHA00001 Electric Heater"), F("0.3.0")) == true) {
                    step++;
                }
                break;
            }
            case SENSOR_0_HUMIDITY: {
                if (present(SENSOR_0_HUMIDITY, S_HUM, F("Humidité")) == true) {  // V_HUM
                    step++;
                }
                break;
            }
            case SENSOR_1_CONTROL_ONOFF: {
                if (present(SENSOR_1_CONTROL_ONOFF, S_BINARY, F("Chauffage")) == true) {  // V_STATUS
                    step++;
                }
                break;
            }
            case SENSOR_2_CONTROL_THERMOSTAT: {
                if (present(SENSOR_2_CONTROL_THERMOSTAT, S_HVAC, F("Chauffage")) == true) {  // V_STATUS, V_TEMP, V_HVAC_SETPOINT_HEAT, V_HVAC_FLOW_STATE
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

    /* Handle onoff control message */
    if ((message.sensor == SENSOR_1_CONTROL_ONOFF) && (message.getType() == V_STATUS)) {
        m_control_mode = CONTROL_MODE_ONOFF;
        m_control_onoff_heating = message.getBool();
        m_report_needed = true;
    }

    /* Handle heater on/off messages */
    else if ((message.sensor == SENSOR_2_CONTROL_THERMOSTAT) && (message.getType() == V_HVAC_FLOW_STATE)) {
        m_control_mode = CONTROL_MODE_THERMOSTAT;
        m_control_thermostat_heating = (strcmp(message.getString(), "HeatOn") == 0 || strcmp(message.getString(), "AutoChangeOver") == 0) ? true : false;
        m_report_needed = true;
        Serial.print(F(" [i] Receveid state "));
        Serial.println(m_control_thermostat_heating ? F("on") : F("off"));
    }

    /* Handle thermostat heater setpoint message */
    else if ((message.sensor == SENSOR_2_CONTROL_THERMOSTAT) && (message.getType() == V_HVAC_SETPOINT_HEAT)) {
        float target = message.getFloat();
        if (target < 0) {
            target = 0;
        } else if (target > 35) {
            target = 35;
        }
        m_control_mode = CONTROL_MODE_THERMOSTAT;
        m_control_thermostat_target = target;
        m_report_needed = true;
        Serial.print(F(" [i] Receveid new target of "));
        Serial.println(target);
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

    /* Reporting task */
    {

        /* State machine */
        static enum {
            STATE_0,
            STATE_1,
            STATE_2,
            STATE_3,
            STATE_4,
        } m_sm;
        switch (m_sm) {
            case STATE_0: {

                /* Wait for report request */
                if (m_report_needed != true) {
                    break;
                }

                /* Move on */
                m_sm = STATE_1;
                break;
            }

            case STATE_1: {

                /* Send on-off status */
                MyMessage message(SENSOR_1_CONTROL_ONOFF, V_STATUS);
                if (m_control_mode == CONTROL_MODE_ONOFF) {
                    message.set(m_control_onoff_heating);
                } else {
                    message.set(false);
                }
                if (send(message) != true) {
                    break;
                }

                /* Move on */
                m_sm = STATE_2;
                break;
            }

            case STATE_2: {

                /* Send thermostat status */
                MyMessage message(SENSOR_2_CONTROL_THERMOSTAT, V_HVAC_FLOW_STATE);
                if (m_control_mode == CONTROL_MODE_THERMOSTAT) {
                    message.set(m_control_thermostat_heating ? F("HeatOn") : F("Off"));
                } else {
                    message.set(F("Off"));
                }
                if (send(message) != true) {
                    break;
                }

                /* Move on */
                m_sm = STATE_3;
                break;
            }

            case STATE_3: {

                /* Send thermostat target */
                MyMessage message(SENSOR_2_CONTROL_THERMOSTAT, V_HVAC_SETPOINT_HEAT);
                if (send(message.set(m_control_thermostat_target, 1)) != true) {
                    break;
                }

                /* Move on */
                m_sm = STATE_4;
                break;
            }

            case STATE_4: {

                /* Clear flag and restart state machine */
                m_report_needed = false;
                m_sm = STATE_0;
                break;
            }
        }
    }

    /* Control task */
    {

        /* State machine */
        static float m_temperature_measured = 0;
        static float m_humidity_measured = 0;
        static uint32_t m_timestamp;
        static enum {
            STATE_READ,
            STATE_REPORT_TEMPERATURE,
            STATE_REPORT_HUMIDITY,
            STATE_CONTROL,
            STATE_CONTROL_THERMOSTAT_0,
            STATE_CONTROL_THERMOSTAT_1,
            STATE_CONTROL_ONOFF,
            STATE_CONTROL_NONE,
            STATE_ERROR_0,
            STATE_ERROR_1,
        } m_sm;
        switch (m_sm) {

            case STATE_READ: {

                /* Read temperature and humidity */
                res = m_sensor.measurement_sync_get(m_temperature_measured, m_humidity_measured);
                if (res < 0) {
                    Serial.println(F(" [e] Failed to read from temperature sensor!"));
                    m_sm = STATE_ERROR_0;
                    return;
                }

                /* Move on */
                m_sm = STATE_REPORT_TEMPERATURE;
                break;
            }

            case STATE_REPORT_TEMPERATURE: {

                /* Attempt to report temperature */
                static uint32_t m_temperature_report_timestamp = 0;
                static float m_temperature_reported = 0;
                if ((fabs(m_temperature_measured - m_temperature_reported) >= 0.1) && (millis() - m_temperature_report_timestamp >= 30000)) {
                    MyMessage message(SENSOR_2_CONTROL_THERMOSTAT, V_TEMP);
                    if (send(message.set(m_temperature_measured, 1)) == true) {
                        m_temperature_reported = m_temperature_measured;
                        m_temperature_report_timestamp = millis();
                    }
                }

                /* Move on */
                m_sm = STATE_REPORT_HUMIDITY;
                break;
            }

            case STATE_REPORT_HUMIDITY: {

                /* Attempt to report humidity */
                static uint32_t m_humidity_report_timestamp = 0;
                static float m_humidity_reported = 0;
                if ((fabs(m_humidity_measured - m_humidity_reported) >= 0.5) && (millis() - m_humidity_report_timestamp >= 30000)) {
                    MyMessage message(SENSOR_0_HUMIDITY, V_HUM);
                    if (send(message.set(m_humidity_measured, 1)) == true) {
                        m_humidity_reported = m_humidity_measured;
                        m_humidity_report_timestamp = millis();
                    }
                }

                /* Move on */
                m_sm = STATE_CONTROL;
                break;
            }

            case STATE_CONTROL: {

                /* This firmware has two competing operating modes.
                 * The last message received will dictate the current operating mode.
                 * In those modes, heating can be controlled:
                 * 1) as an on-off switch (this is can be usefull when you want your home automation software to have maximum control);
                 * 2) as a thermostat entity (with a temperature target). */
                if (m_control_mode == CONTROL_MODE_THERMOSTAT) {
                    m_sm = STATE_CONTROL_THERMOSTAT_0;
                } else if (m_control_mode == CONTROL_MODE_ONOFF) {
                    m_sm = STATE_CONTROL_ONOFF;
                } else {
                    m_sm = STATE_CONTROL_NONE;
                }
                break;
            }

            case STATE_CONTROL_THERMOSTAT_0: {

                /* Stop here if there is no need to heat */
                if (m_control_thermostat_heating == false || m_temperature_measured > m_control_thermostat_target) {
                    digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, LOW);
                    digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, HIGH);
                    m_sm = STATE_READ;
                    break;
                }

                /* Turn on heating */
                digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, LOW);
                digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, LOW);

                /* Move on */
                m_sm = STATE_CONTROL_THERMOSTAT_1;
                break;
            }

            case STATE_CONTROL_THERMOSTAT_1: {

                /* Wait a minute */
                if (millis() - m_timestamp < 60000) {
                    break;
                }

                /* Move on */
                m_sm = STATE_READ;
                break;
            }

            case STATE_CONTROL_ONOFF: {

                /* Adjust heating */
                if (m_control_onoff_heating == true) {
                    digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, LOW);
                    digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, LOW);
                } else {
                    digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, LOW);
                    digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, HIGH);
                }

                /* Move on */
                m_sm = STATE_READ;
                break;
            }

            case STATE_CONTROL_NONE: {

                /* Ensure heating is disabled */
                digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, LOW);
                digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, HIGH);

                /* Move on */
                m_sm = STATE_READ;
                break;
            }

            case STATE_ERROR_0: {

                /* Turn off heating */
                digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_P_PIN, LOW);
                digitalWrite(CONFIG_PERIPH_HEATER_TRIAC_N_PIN, HIGH);

                /* Turn on error led */
                digitalWrite(CONFIG_PERIPH_LED_RED_PIN, HIGH);

                /* Move on */
                m_timestamp = millis();
                break;
            }

            case STATE_ERROR_1: {

                /* Wait 10 seconds */
                if (millis() - m_timestamp < 10000) {
                    break;
                }

                /* Move on */
                m_sm = STATE_READ;
                break;
            }
        }
    }
}

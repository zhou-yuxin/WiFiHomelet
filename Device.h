#pragma once

#include "WiFiHomelet.h"

namespace Device {

class Switch {

private:
    uint8_t pin;
    bool trigger_on_low;

public:
    void begin(uint8_t pin, bool trigger_on_low = true) {
        this->pin = pin;
        this->trigger_on_low = trigger_on_low;
        pinMode(pin, OUTPUT);
        setState(false);
    }

    bool getState() const {
        return (digitalRead(pin) == LOW) == trigger_on_low;
    }

    void setState(bool trigger) {
        digitalWrite(pin, trigger == trigger_on_low ? LOW : HIGH);
    }

};

class PWM {

private:
    uint8_t pin;
    uint8_t percent;

public:
    void begin(uint8_t pin, uint32_t freq = 10000) {
        this->pin = pin;
        pinMode(pin, OUTPUT);
        analogWriteFreq(freq);
        analogWriteRange(100);
        setRatio(0);
    }

    uint8_t getRatio() const {
        return percent;
    }

    void setRatio(uint8_t percent) {
        this->percent = percent;
        analogWrite(pin, percent);
    }

};

class Radar {

private:
    uint8_t pin;
    bool trigger_on_low;
    bool last_state;
    String report_url;
    WiFiHomelet* wifi_homelet;

public:
    void begin(uint8_t pin, bool trigger_on_low = true) {
        this->pin = pin;
        this->trigger_on_low = trigger_on_low;
        pinMode(pin, INPUT);
        last_state = getState();
    }

    bool getState() const {
        return (digitalRead(pin) == LOW) == trigger_on_low;
    }

    void setReportURL(const String& url, WiFiHomelet* homelet) {
        report_url = url;
        wifi_homelet = homelet;
    }

    bool tick() {
        bool state = getState();
        if(state != last_state) {
            if(!report_url.isEmpty()) {
                String url = report_url + "&name=" + WiFi.hostname()
                    + "&radar=" + (state ? "true" : "false");
                wifi_homelet->httpGet(url);
            }
            last_state = state;
        }
        return state;
    }

};

class Mode {

public:
    static constexpr uint8_t OFF = 0;
    static constexpr uint8_t ON = 1;
    static constexpr uint8_t AUTO = 2;

private:
    uint8_t mode;

public:
    Mode(): mode(AUTO) {
    }

    const char* toString() const {
        return getMapping()[mode];
    }

    bool fromString(const String& str) {
        const char** mapping = getMapping();
        for(uint8_t i = 0; i < 3; i++) {
            if(str.equalsIgnoreCase(mapping[i])) {
                mode = i;
                return true;
            }
        }
        return false;
    }

    bool operator==(uint8_t value) const {
        return mode == value;
    }

private:
    const char** getMapping() const {
        static const char* mapping[] = {"off", "on", "auto"};
        return mapping;
    }

};

}

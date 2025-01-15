#pragma once

#include "WiFiHomelet.h"

namespace Device {

template <bool trigger_on_low = true>
class Switch {

private:
    uint8_t pin;

public:
    void begin(uint8_t pin) {
        this->pin = pin;
        pinMode(pin, OUTPUT);
        set(false);
    }

    bool get() const {
        return (digitalRead(pin) == LOW) == trigger_on_low;
    }

    void set(bool state) {
        digitalWrite(pin, state == trigger_on_low ? LOW : HIGH);
    }

    bool set(const String& arg) {
        bool state;
        if(WiFiHomelet::parse(arg, state)) {
            set(state);
            return true;
        }
        return false;
    }

};

template <bool trigger_on_high = true>
class PWM {

private:
    static constexpr uint32_t RANGE = 1000;

private:
    uint8_t pin;
    float ratio;

public:
    void begin(uint8_t pin, uint32_t freq = 10000) {
        this->pin = pin;
        pinMode(pin, OUTPUT);
        analogWriteFreq(freq);
        analogWriteRange(RANGE);
        set(0);
    }

    float get() const {
        return ratio;
    }

    String get(const char* format = "%.4f") const {
        char buffer[32];
        sprintf(buffer, format, ratio);
        return String(buffer);
    }

    bool set(float ratio) {
        if(0.0f <= ratio && ratio <= 1.0f) {
            this->ratio = ratio;
            int value = int(RANGE * ratio);
            analogWrite(pin, trigger_on_high ? value : RANGE - value);
            return true;
        }
        return false;
    }

    bool set(const String& arg) {
        float ratio;
        if(WiFiHomelet::parse(arg, ratio, 0.0f, 1.0f)) {
            return set(ratio);
        }
        return false;
    }

};

template <bool trigger_on_low = true>
class Radar {

private:
    uint8_t pin;
    bool last_state;
    String report_url;
    WiFiHomelet* wifi_homelet;

public:
    void begin(uint8_t pin) {
        this->pin = pin;
        pinMode(pin, INPUT);
        last_state = get();
    }

    bool get() const {
        return (digitalRead(pin) == LOW) == trigger_on_low;
    }

    void setReportURL(const String& url, WiFiHomelet* homelet) {
        report_url = url;
        wifi_homelet = homelet;
    }

    bool tick(std::function<void(bool)> action = nullptr) {
        bool state = get();
        if(action) {
            action(state);
        }
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

}

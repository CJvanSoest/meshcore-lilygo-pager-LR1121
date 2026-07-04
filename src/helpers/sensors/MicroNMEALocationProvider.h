#pragma once

#include "LocationProvider.h"
#include <MicroNMEA.h>
#include <RTClib.h>
#include <helpers/RefCountedDigitalPin.h>

#ifndef GPS_EN
    #ifdef PIN_GPS_EN
        #define GPS_EN PIN_GPS_EN
    #else
        #define GPS_EN (-1)
    #endif
#endif

#ifndef PIN_GPS_EN_ACTIVE
    #define PIN_GPS_EN_ACTIVE HIGH
#endif

#ifndef GPS_RESET
    #ifdef PIN_GPS_RESET
        #define GPS_RESET PIN_GPS_RESET
    #else
        #define GPS_RESET (-1)
    #endif
#endif

#ifndef GPS_RESET_FORCE
    #ifdef PIN_GPS_RESET_ACTIVE
        #define GPS_RESET_FORCE PIN_GPS_RESET_ACTIVE
    #else
        #define GPS_RESET_FORCE LOW
    #endif
#endif

class MicroNMEALocationProvider : public LocationProvider {
    char _nmeaBuffer[100];
    MicroNMEA nmea;
    mesh::RTCClock* _clock;
    Stream* _gps_serial;
    RefCountedDigitalPin* _peripher_power;
    int8_t _claims = 0;
    int _pin_reset;
    int _pin_en;
    long next_check = 0;
    long time_valid = 0;
    // GSV (satellites-in-view / best C/N0) tracking, per constellation with a
    // staleness hold so a flickering satellite doesn't drop the readout to 0.
    static const unsigned long GSV_HOLD_MS = 6000;
    struct GsvTalker { uint16_t id; int inView; int snr; unsigned long ts; };
    GsvTalker _gsv[6] = {};
    int  _gsv_n = 0;
    char _gsv_line[110];     // current sentence accumulator for GSV parsing
    int  _gsv_len = 0;
    unsigned long _last_time_sync = 0;
    static const unsigned long TIME_SYNC_INTERVAL = 1800000; // Re-sync every 30 minutes

public :
    MicroNMEALocationProvider(Stream& ser, mesh::RTCClock* clock = NULL, int pin_reset = GPS_RESET, int pin_en = GPS_EN,RefCountedDigitalPin* peripher_power=NULL) :
    _gps_serial(&ser), nmea(_nmeaBuffer, sizeof(_nmeaBuffer)), _pin_reset(pin_reset), _pin_en(pin_en), _clock(clock), _peripher_power(peripher_power) {
        if (_pin_reset != -1) {
            pinMode(_pin_reset, OUTPUT);
            digitalWrite(_pin_reset, GPS_RESET_FORCE);
        }
        if (_pin_en != -1) {
            pinMode(_pin_en, OUTPUT);
            digitalWrite(_pin_en, LOW);
        }
    }

    void claim() {
        _claims++;
        if (_claims > 0) {
            if (_peripher_power) _peripher_power->claim();
        }
    }

    void release() {
        if (_claims == 0) return; // avoid negative _claims
        _claims--;
        if (_peripher_power) _peripher_power->release();
    }

    void begin() override {
        claim();
        if (_pin_en != -1) {
            digitalWrite(_pin_en, PIN_GPS_EN_ACTIVE);
        }
        if (_pin_reset != -1) {
            digitalWrite(_pin_reset, !GPS_RESET_FORCE);
        }
    }

    void reset() override {
        if (_pin_reset != -1) {
            digitalWrite(_pin_reset, GPS_RESET_FORCE);
            delay(10);
            digitalWrite(_pin_reset, !GPS_RESET_FORCE);
        }
    }

    void stop() override {
        if (_pin_en != -1) {
            digitalWrite(_pin_en, !PIN_GPS_EN_ACTIVE);
        }
        if (_pin_reset != -1) {
            digitalWrite(_pin_reset, GPS_RESET_FORCE);
        }
        release();
    }

    bool isEnabled() override {
        // directly read the enable pin if present as gps can be
        // activated/deactivated outside of here ...
        if (_pin_en != -1) {
            return digitalRead(_pin_en) == PIN_GPS_EN_ACTIVE;
        } else {
            return true; // no enable so must be active
        }
    }

    void syncTime() override { nmea.clear(); LocationProvider::syncTime(); }
    long getLatitude() override { return nmea.getLatitude(); }
    long getLongitude() override { return nmea.getLongitude(); }
    long getAltitude() override { 
        long alt = 0;
        nmea.getAltitude(alt);
        return alt;
    }
    long satellitesCount() override { return nmea.getNumSatellites(); }
    bool isValid() override { return nmea.isValid(); }

    // Satellites in VIEW (from GSV) + best C/N0. Unlike satellitesCount()
    // (satellites used in the fix, 0 until locked) these go non-zero as soon
    // as the antenna hears anything — the "is the RF path alive / how good is
    // the sky" indicator during acquisition. Values are held per constellation
    // with a short staleness window so a satellite that flickers in and out of
    // one GSV cycle doesn't make the readout drop to 0.
    int getSatsInView() const {
        int total = 0;
        for (int i = 0; i < _gsv_n; i++)
            if ((millis() - _gsv[i].ts) < GSV_HOLD_MS) total += _gsv[i].inView;
        return total;
    }
    int getBestSNR() const {
        int best = 0;
        for (int i = 0; i < _gsv_n; i++)
            if ((millis() - _gsv[i].ts) < GSV_HOLD_MS && _gsv[i].snr > best) best = _gsv[i].snr;
        return best;
    }

    long getTimestamp() override { 
        DateTime dt(nmea.getYear(), nmea.getMonth(),nmea.getDay(),nmea.getHour(),nmea.getMinute(),nmea.getSecond());
        return dt.unixtime();
    } 

    void sendSentence(const char *sentence) override {
        nmea.sendSentence(*_gps_serial, sentence);
    }

    // Parse one complete GSV sentence: talker (chars 1-2 = constellation),
    // field 4 = satellites in view for that talker (identical across its
    // messages), SNR = every 4th field from field 8. Per-constellation state
    // is time-stamped so getSatsInView()/getBestSNR() can hold it briefly.
    // Fed one char at a time alongside nmea.process().
    void gsvAccumulate(char c) {
        if (c == '\n' || c == '\r') {
            if (_gsv_len > 6 && _gsv_line[0] == '$' &&
                _gsv_line[3] == 'G' && _gsv_line[4] == 'S' && _gsv_line[5] == 'V') {
                _gsv_line[_gsv_len] = 0;
                uint16_t talker = ((uint8_t)_gsv_line[1] << 8) | (uint8_t)_gsv_line[2];
                int field = 0, msgNum = 0, inView = 0, lineSnr = 0;
                const char* p = _gsv_line;
                for (const char* q = _gsv_line; ; q++) {
                    if (*q == ',' || *q == '*' || *q == 0) {
                        field++;
                        if (field == 3) msgNum = atoi(p);
                        else if (field == 4) inView = atoi(p);
                        else if (field >= 8 && (field - 8) % 4 == 0) {
                            int s = atoi(p); if (s > lineSnr) lineSnr = s;
                        }
                        p = q + 1;
                        if (*q == 0 || *q == '*') break;
                    }
                }
                // Locate (or add) this constellation's slot.
                int idx = -1;
                for (int i = 0; i < _gsv_n; i++) if (_gsv[i].id == talker) { idx = i; break; }
                if (idx < 0 && _gsv_n < (int)(sizeof(_gsv) / sizeof(_gsv[0]))) {
                    idx = _gsv_n++; _gsv[idx].id = talker; _gsv[idx].snr = 0;
                }
                if (idx >= 0) {
                    _gsv[idx].inView = inView;
                    // msgNum 1 starts a fresh burst for this talker; later
                    // messages contribute more satellites' SNR to the same burst.
                    if (msgNum <= 1) _gsv[idx].snr = lineSnr;
                    else if (lineSnr > _gsv[idx].snr) _gsv[idx].snr = lineSnr;
                    _gsv[idx].ts = millis();
                }
            }
            _gsv_len = 0;
        } else if (_gsv_len < (int)sizeof(_gsv_line) - 1) {
            _gsv_line[_gsv_len++] = c;
        }
    }

    void loop() override {

        while (_gps_serial->available()) {
            char c = _gps_serial->read();
            #ifdef GPS_NMEA_DEBUG
            Serial.print(c);
            #endif
            nmea.process(c);
            gsvAccumulate(c);
        }

        if (!isValid()) time_valid = 0;

        if (millis() > next_check) {
            next_check = millis() + 1000;
            // Re-enable time sync periodically when GPS has valid fix
            if (!_time_sync_needed && _clock != NULL && (millis() - _last_time_sync) > TIME_SYNC_INTERVAL) {
                _time_sync_needed = true;
            }
            if (_time_sync_needed && time_valid > 2) {
                if (_clock != NULL) {
                    _clock->setCurrentTime(getTimestamp());
                    _time_sync_needed = false;
                    _last_time_sync = millis();
                }
            }
            if (isValid()) {
                time_valid ++;
            }
        }
    }
};

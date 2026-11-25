#pragma once
// Nearby vessels from Open Waters AIS. No token. Parsed into Aircraft so the
// existing scope draws them the same way it draws flights.
#include <vector>
#include "aircraft.h"

class AisClient {
public:
    void setHome(double lat, double lon) { _lat = lat; _lon = lon; }
    void setRange(float km) { _rangeKm = km; }

    // True on a parsed reply. `out` is replaced with the nearest vessels.
    // On failure `out` is left unchanged.
    bool poll(std::vector<Aircraft>& out);

    bool lastWasRefused() const { return _refused; }
    int  lastStatus() const { return _lastStatus; }

private:
    double _lat = 0, _lon = 0;
    float  _rangeKm = 20.0f;
    bool   _refused = false;
    int    _lastStatus = 0;
};

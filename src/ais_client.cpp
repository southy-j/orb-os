// Nearby vessels over HTTPS from Open Waters (ais.openwaters.io). No API key.
// Anonymous requests must name a box. The reply is GeoJSON, streamed and filtered
// the same way the aircraft client filters readsb JSON, then cut down to the
// nearest AIS_MAX_VESSELS so the scope stays readable.
#include "ais_client.h"
#include "config.h"
#include "geo.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <StreamUtils.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <algorithm>
#include <cstring>

struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramJsonAllocator s_jsonPsram;

class ReliableJsonStream : public Stream {
public:
    explicit ReliableJsonStream(Stream& source) : _source(source) {}
    int available() override { return (int)(_len - _pos) + _source.available(); }
    int read() override {
        if (_pos >= _len && !refill()) return -1;
        ++_bytesRead;
        return _buf[_pos++];
    }
    int peek() override {
        if (_pos >= _len && !refill()) return -1;
        return _buf[_pos];
    }
    void flush() override { _source.flush(); }
    size_t write(uint8_t) override { return 0; }
    size_t bytesRead() const { return _bytesRead; }

private:
    bool refill() {
        _pos = _len = 0;
        const uint32_t started = millis();
        for (;;) {
            const int avail = _source.available();
            if (avail > 0) {
                const size_t want = avail < (int)sizeof(_buf) ? (size_t)avail : sizeof(_buf);
                const int got = _source.readBytes(_buf, want);
                if (got > 0) { _len = (size_t)got; return true; }
            }
            if (millis() - started > 8000) return false;
            delay(1);
        }
    }
    uint8_t _buf[1024];
    size_t  _pos = 0, _len = 0, _bytesRead = 0;
    Stream& _source;
};

static float knownCourse(JsonObjectConst p) {
    if (!p["heading"].isNull()) {
        const int h = p["heading"].as<int>();
        if (h >= 0 && h < 360) return (float)h;
    }
    if (!p["cog"].isNull()) {
        const float c = p["cog"].as<float>();
        if (c >= 0.0f && c < 360.0f) return c;
    }
    return NAN;
}

bool AisClient::poll(std::vector<Aircraft>& out) {
    _refused = false;
    _lastStatus = 0;
    if (WiFi.status() != WL_CONNECTED) return false;

    // Degrees of latitude are ~111 km. Longitude shrinks with latitude. The cap keeps
    // a harbour's GeoJSON in the same size range as one aircraft poll.
    const double km = std::min((double)AIS_QUERY_MAX_KM, std::max(8.0, (double)_rangeKm));
    const double dLat = km / 111.0;
    const double cosLat = cos(_lat * M_PI / 180.0);
    const double dLon = km / (111.0 * std::max(0.2, fabs(cosLat)));
    const double minLat = std::max(-90.0, _lat - dLat);
    const double maxLat = std::min(90.0, _lat + dLat);
    const double minLon = std::max(-180.0, _lon - dLon);
    const double maxLon = std::min(180.0, _lon + dLon);

    char url[320];
    snprintf(url, sizeof(url),
             "https://%s/v1/vessels?bbox=%.4f,%.4f,%.4f,%.4f&kind=vessel&max_age=6h&max_age_moving=30m",
             AIS_HOST, minLat, minLon, maxLat, maxLon);

    WiFiClientSecure client;
#if ADSB_HTTPS_INSECURE
    client.setInsecure();
#else
    extern const uint8_t rootBundleStart[] asm("_binary_x509_crt_bundle_start");
    extern const uint8_t rootBundleEnd[] asm("_binary_x509_crt_bundle_end");
    client.setCACertBundle(rootBundleStart, (size_t)(rootBundleEnd - rootBundleStart));
#endif
    client.setHandshakeTimeout(10);
    HTTPClient http;
    http.useHTTP10(false);
    http.setReuse(false);
    http.setConnectTimeout(ADSB_CONNECT_MS);
    http.setTimeout(ADSB_READ_MS);
    if (!http.begin(client, url)) { _lastStatus = -1; return false; }
    http.setUserAgent(AIS_USER_AGENT);
    http.addHeader("Accept", "application/json");
    http.setAcceptEncoding("identity");
    const char *headers[] = {"Transfer-Encoding"};
    http.collectHeaders(headers, 1);

    const int code = http.GET();
    _lastStatus = code;
    Serial.printf("[ais] HTTPS %s -> %d\n", AIS_HOST, code);
    if (code != 200) {
        _refused = code >= 400 && code < 500;
        http.end();
        client.stop();
        return false;
    }

    JsonDocument filter(&s_jsonPsram);
    filter["features"][0]["geometry"]["coordinates"] = true;
    filter["features"][0]["properties"]["mmsi"] = true;
    filter["features"][0]["properties"]["name"] = true;
    filter["features"][0]["properties"]["cog"] = true;
    filter["features"][0]["properties"]["sog"] = true;
    filter["features"][0]["properties"]["heading"] = true;
    filter["features"][0]["properties"]["kind"] = true;

    JsonDocument doc(&s_jsonPsram);
    ReliableJsonStream jsonStream(http.getStream());
    ChunkDecodingStream decoded(jsonStream);
    Stream &body = http.header("Transfer-Encoding").equalsIgnoreCase("chunked")
        ? static_cast<Stream&>(decoded) : static_cast<Stream&>(jsonStream);
    const DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    http.end();
    client.stop();
    if (err) {
        Serial.printf("[ais] parse failed: %s; read=%u\n", err.c_str(), (unsigned)jsonStream.bytesRead());
        _lastStatus = -2;
        return false;
    }

    JsonArrayConst features = doc["features"].as<JsonArrayConst>();
    if (features.isNull()) { _lastStatus = -2; return false; }

    std::vector<Aircraft> tmp;
    std::vector<float> dist;
    tmp.reserve(AIS_MAX_VESSELS);
    dist.reserve(AIS_MAX_VESSELS);
    const uint32_t now = millis();

    for (JsonObjectConst f : features) {
        JsonArrayConst coords = f["geometry"]["coordinates"].as<JsonArrayConst>();
        if (coords.size() < 2) continue;
        JsonObjectConst p = f["properties"];
        const char *kind = p["kind"] | "";
        if (kind[0] && strcmp(kind, "vessel") != 0) continue;
        const long mmsi = p["mmsi"] | 0L;
        if (mmsi <= 0) continue;

        const double lon = coords[0].as<double>();
        const double lat = coords[1].as<double>();
        const float d = (float)geo::haversineKm(_lat, _lon, lat, lon);

        int farIdx = -1;
        if ((int)tmp.size() >= AIS_MAX_VESSELS) {
            farIdx = 0;
            for (int i = 1; i < (int)dist.size(); ++i) if (dist[i] > dist[farIdx]) farIdx = i;
            if (d >= dist[farIdx]) continue;
        }

        Aircraft ac;
        char id[16];
        snprintf(id, sizeof(id), "%ld", mmsi);
        ac.hex = id;
        const char *name = p["name"] | "";
        ac.flight = (name && name[0]) ? name : id;
        ac.flight.trim();
        ac.type = "AIS";
        ac.lat = lat;
        ac.lon = lon;
        ac.onGround = false;
        ac.altBaro = 0;                 // surface: the low-altitude colour on the scope
        ac.track = knownCourse(p);
        ac.gs = p["sog"].isNull() ? NAN : p["sog"].as<float>();
        ac.baroRate = NAN;
        ac.squawk = -1;
        ac.seenPos = 0;
        ac.military = false;
        ac.lastUpdateMs = now;

        if (farIdx >= 0) { tmp[farIdx] = std::move(ac); dist[farIdx] = d; }
        else             { tmp.push_back(std::move(ac)); dist.push_back(d); }
    }

    out.swap(tmp);
    Serial.printf("[ais] source=%s vessels=%u (box %.0f km)\n", AIS_HOST, (unsigned)out.size(), km);
    return true;
}

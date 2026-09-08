#include "engine/alpha.h"
#include <nlohmann/json.hpp>

namespace qcutter {

void AlphaMode::apply(std::uint8_t* rgba, std::size_t count_px) const noexcept {
    if (!rgba || count_px == 0) return;
    switch (kind) {
    case Kind::Keep:
        return;
    case Kind::Threshold: {
        const auto below = thresholdValue;
        for (std::size_t i = 0; i < count_px; ++i) {
            auto* p = rgba + i * 4;
            if (p[3] < below) p[3] = 0;
        }
        break;
    }
    case Kind::ColorKey: {
        const auto r = ck_r, g = ck_g, b = ck_b, t = toleranceValue;
        for (std::size_t i = 0; i < count_px; ++i) {
            auto* p = rgba + i * 4;
            const std::uint8_t dr = (p[0] > r) ? p[0] - r : r - p[0];
            const std::uint8_t dg = (p[1] > g) ? p[1] - g : g - p[1];
            const std::uint8_t db = (p[2] > b) ? p[2] - b : b - p[2];
            if (dr <= t && dg <= t && db <= t) p[3] = 0;
        }
        break;
    }
    }
}

nlohmann::json AlphaMode::toJson() const {
    using nlohmann::json;
    switch (kind) {
    case Kind::Keep:
        return json{{"mode", "keep"}};
    case Kind::Threshold:
        return json{{"mode", "threshold"}, {"value", {{"below", thresholdValue}}}};
    case Kind::ColorKey:
        return json{{"mode", "colorkey"},
                    {"value", {{"r", ck_r}, {"g", ck_g}, {"b", ck_b}, {"tolerance", toleranceValue}}}};
    }
    return json{{"mode", "keep"}};
}

AlphaMode AlphaMode::fromJson(const nlohmann::json& j) {
    AlphaMode m;
    const auto mode = j.value("mode", std::string("keep"));
    if (mode == "threshold") {
        m.kind = Kind::Threshold;
        const auto& v = j.at("value");
        m.thresholdValue = v.value("below", std::uint8_t{128});
    } else if (mode == "colorkey") {
        m.kind = Kind::ColorKey;
        const auto& v = j.at("value");
        m.ck_r = v.value("r", std::uint8_t{0});
        m.ck_g = v.value("g", std::uint8_t{0});
        m.ck_b = v.value("b", std::uint8_t{0});
        m.toleranceValue = v.value("tolerance", std::uint8_t{12});
    }
    return m;
}

} // namespace qcutter
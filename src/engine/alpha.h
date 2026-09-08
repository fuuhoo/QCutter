// AlphaMode: 切片输出时的透明值处理模式
#pragma once
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

namespace qcutter {

/// 透明处理模式。
struct AlphaMode {
    enum class Kind { Keep, Threshold, ColorKey };

    Kind kind = Kind::Keep;
    /// Threshold::below: 低于该 alpha 视为透明
    std::uint8_t thresholdValue = 128;
    /// ColorKey: 接近 (r,g,b) ± tolerance 即视为透明
    std::uint8_t ck_r = 0;
    std::uint8_t ck_g = 0;
    std::uint8_t ck_b = 0;
    std::uint8_t toleranceValue = 12;

    static AlphaMode keep() { return {}; }
    static AlphaMode threshold(std::uint8_t below) {
        AlphaMode m; m.kind = Kind::Threshold; m.thresholdValue = below; return m;
    }
    static AlphaMode colorKey(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t tol) {
        AlphaMode m; m.kind = Kind::ColorKey;
        m.ck_r = r; m.ck_g = g; m.ck_b = b; m.toleranceValue = tol;
        return m;
    }

    /// 在 RGBA 缓冲上原地应用该模式。
    void apply(std::uint8_t* rgba, std::size_t count_px) const noexcept;

    /// JSON 序列化（与 Rust 端 serde 兼容）
    nlohmann::json toJson() const;
    static AlphaMode fromJson(const nlohmann::json& j);
};

} // namespace qcutter
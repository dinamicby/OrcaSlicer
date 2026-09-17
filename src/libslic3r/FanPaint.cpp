#include "FanPaint.hpp"

#include <charconv>
#include <system_error>

namespace Slic3r {

namespace {

// Speeds 0, 5, ..., 100.
constexpr int FAN_PAINT_SPEED_STEPS    = 21;
constexpr int FAN_PAINT_SPEED_STEP_PCT = 5;
constexpr int FAN_PAINT_MAX_SPEED_PCT  = 100;

constexpr std::string_view START_MARKER_PREFIX = ";_PAINT_FAN_START S";
constexpr std::string_view COMMENT_PREFIX      = "; PAINT_FAN";

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.substr(0, prefix.size()) == prefix;
}

} // namespace

std::optional<FanPaintValue> decode_fan_paint_value(uint8_t value)
{
    if (value == 0 || value > MAX_FAN_FACET_VALUE)
        return std::nullopt;
    const int index = int(value) - 1;
    return FanPaintValue{ uint8_t(index / FAN_PAINT_SPEED_STEPS + 1),
                          uint8_t((index % FAN_PAINT_SPEED_STEPS) * FAN_PAINT_SPEED_STEP_PCT) };
}

uint8_t fan_paint_line_bit(ExtrusionRole role)
{
    switch (role) {
    case erExternalPerimeter:
    case erOverhangPerimeter:
        return FAN_PAINT_LINE_OUTER_WALL;
    case erPerimeter:
        return FAN_PAINT_LINE_INNER_WALLS;
    case erSolidInfill:
    case erInternalInfill:
    case erTopSolidInfill:
    case erBottomSurface:
    case erBridgeInfill:
    case erInternalBridgeInfill:
    case erGapFill:
    case erIroning:
        return FAN_PAINT_LINE_OTHER;
    default:
        return 0;
    }
}

bool fan_paint_band_covers_layer(const FanPaintBand &band, coordf_t slice_z)
{
    return band.z_min <= slice_z + EPSILON && slice_z + EPSILON < band.z_max;
}

std::string fan_paint_start_marker(int speed_pct)
{
    return std::string(START_MARKER_PREFIX) + std::to_string(speed_pct) + "\n";
}

std::optional<int> parse_fan_paint_start_marker(std::string_view line)
{
    if (! starts_with(line, START_MARKER_PREFIX))
        return std::nullopt;
    const std::string_view digits = line.substr(START_MARKER_PREFIX.size());
    int speed = -1;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), speed);
    if (error != std::errc() || speed < 0 || speed > FAN_PAINT_MAX_SPEED_PCT)
        return std::nullopt;
    for (const char *c = end; c != digits.data() + digits.size(); ++ c)
        if (*c != '\n' && *c != '\r')
            return std::nullopt;
    return speed;
}

std::string fan_paint_entry_comment(int speed_pct)
{
    return std::string(COMMENT_PREFIX) + " S" + std::to_string(speed_pct) + "\n";
}

bool is_fan_paint_comment(std::string_view line)
{
    return starts_with(line, COMMENT_PREFIX);
}

std::string fan_paint_summary_line(size_t zones, size_t marked)
{
    return "; fan_paint: zones=" + std::to_string(zones) + " marked=" + std::to_string(marked) + "\n";
}

} // namespace Slic3r

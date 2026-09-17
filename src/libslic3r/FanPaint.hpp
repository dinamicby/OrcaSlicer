#ifndef slic3r_FanPaint_hpp_
#define slic3r_FanPaint_hpp_

// Protype fan paint: places on an object where the part-cooling fan runs at an exact, painted speed.
//
// A zone comes from mesh facets painted with an encoded value (projected onto every layer they cross and
// widened by that layer's walls) or from a Z band of the object (the whole layer outline). G-code
// generation wraps extrusions inside a zone in ";_PAINT_FAN_START S<pct>" / ";_PAINT_FAN_END"; CoolingBuffer
// turns those into the exact fan command, announced by "; PAINT_FAN S<pct>" / "; PAINT_FAN END" comments
// that the daemon's lead pass works from.

#include "libslic3r.h"
#include "ExtrusionEntity.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Slic3r {

// Encoded fan paint value of a facet or band: 0 = unpainted, otherwise 1 + (lines - 1) * 21 + speed / 5, lines
// being a mask of the FAN_PAINT_LINE_* bits (1..7) and speed a percentage in steps of 5 (0..100).
constexpr uint8_t MAX_FAN_FACET_VALUE = 147;

constexpr uint8_t FAN_PAINT_LINE_OUTER_WALL  = 1;
constexpr uint8_t FAN_PAINT_LINE_INNER_WALLS = 2;
// Infill, bridges, top and bottom surfaces, gap fill, ironing.
constexpr uint8_t FAN_PAINT_LINE_OTHER       = 4;

struct FanPaintValue
{
    uint8_t line_mask;
    uint8_t speed_pct;
};

// The decoded value, std::nullopt for 0 (unpainted) and for anything above MAX_FAN_FACET_VALUE.
std::optional<FanPaintValue> decode_fan_paint_value(uint8_t value);

// FAN_PAINT_LINE_* bit a path of `role` answers to; 0 for the roles fan paint never touches
// (supports and their interface, skirt, brim, wipe tower, custom, travel).
uint8_t fan_paint_line_bit(ExtrusionRole role);

// A Z range of an object painted whole, in object coordinates (those of ModelObject::layer_config_ranges).
struct FanPaintBand
{
    coordf_t z_min;
    coordf_t z_max;
    uint8_t  value;
};

// Whether the layer sliced at `slice_z` belongs to `band`, by the rule libslic3r hands a layer the config of a
// layer range with (layer_range_next(), PrintObjectSlice.cpp): z_min <= slice_z + EPSILON < z_max.
bool fan_paint_band_covers_layer(const FanPaintBand &band, coordf_t slice_z);

// Engine-internal markers around a painted run of a path, consumed by CoolingBuffer.
std::string fan_paint_start_marker(int speed_pct);
constexpr std::string_view FAN_PAINT_END_MARKER = ";_PAINT_FAN_END\n";
// Speed of a ";_PAINT_FAN_START S<pct>" line (its line break may follow), std::nullopt for any other line.
std::optional<int> parse_fan_paint_start_marker(std::string_view line);

// Comments CoolingBuffer leaves in the G-code at a zone entry and exit.
std::string fan_paint_entry_comment(int speed_pct);
constexpr std::string_view FAN_PAINT_EXIT_COMMENT = "; PAINT_FAN END\n";
bool is_fan_paint_comment(std::string_view line);

// The one diagnostics line of a print that carries fan paint.
std::string fan_paint_summary_line(size_t zones, size_t marked);

} // namespace Slic3r

#endif // slic3r_FanPaint_hpp_

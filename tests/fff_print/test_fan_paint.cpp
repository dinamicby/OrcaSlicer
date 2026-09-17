#include <catch2/catch.hpp>

#include <optional>
#include <string>

#include "libslic3r/FanPaint.hpp"

using namespace Slic3r;

TEST_CASE("Every fan paint value 1..147 decodes into its lines and speed", "[fan_paint]") {
    for (int lines = 1; lines <= 7; ++ lines)
        for (int speed = 0; speed <= 100; speed += 5) {
            const int value = 1 + (lines - 1) * 21 + speed / 5;
            INFO("value " << value);
            const std::optional<FanPaintValue> decoded = decode_fan_paint_value(uint8_t(value));
            REQUIRE(decoded.has_value());
            CHECK(int(decoded->line_mask) == lines);
            CHECK(int(decoded->speed_pct) == speed);
        }
    CHECK(int(MAX_FAN_FACET_VALUE) == 147);
}

TEST_CASE("Fan paint value 0 and values above 147 decode to nothing", "[fan_paint]") {
    CHECK_FALSE(decode_fan_paint_value(0).has_value());
    CHECK_FALSE(decode_fan_paint_value(148).has_value());
    CHECK_FALSE(decode_fan_paint_value(255).has_value());
}

TEST_CASE("Fan paint line bits follow the extrusion role", "[fan_paint]") {
    CHECK(fan_paint_line_bit(erExternalPerimeter) == FAN_PAINT_LINE_OUTER_WALL);
    CHECK(fan_paint_line_bit(erOverhangPerimeter) == FAN_PAINT_LINE_OUTER_WALL);
    CHECK(fan_paint_line_bit(erPerimeter) == FAN_PAINT_LINE_INNER_WALLS);
    for (ExtrusionRole role : { erSolidInfill, erInternalInfill, erTopSolidInfill, erBottomSurface,
                                erBridgeInfill, erInternalBridgeInfill, erGapFill, erIroning })
        CHECK(fan_paint_line_bit(role) == FAN_PAINT_LINE_OTHER);
    for (ExtrusionRole role : { erNone, erSkirt, erBrim, erSupportMaterial, erSupportMaterialInterface,
                                erSupportTransition, erWipeTower, erCustom, erMixed })
        CHECK(fan_paint_line_bit(role) == 0);
}

TEST_CASE("A fan paint band covers the layers libslic3r gives the same layer range", "[fan_paint]") {
    const FanPaintBand band { 5., 10., 1 };
    CHECK_FALSE(fan_paint_band_covers_layer(band, 4.9));
    CHECK(fan_paint_band_covers_layer(band, 5.1));
    CHECK(fan_paint_band_covers_layer(band, 9.9));
    CHECK_FALSE(fan_paint_band_covers_layer(band, 10.1));
    // A layer sliced exactly on a boundary belongs to the range above it (layer_range_next()).
    CHECK(fan_paint_band_covers_layer(band, 5.));
    CHECK_FALSE(fan_paint_band_covers_layer(band, 10.));
}

TEST_CASE("Fan paint markers and comments carry their speed", "[fan_paint]") {
    CHECK(fan_paint_start_marker(80) == ";_PAINT_FAN_START S80\n");
    CHECK(std::string(FAN_PAINT_END_MARKER) == ";_PAINT_FAN_END\n");
    CHECK(parse_fan_paint_start_marker(";_PAINT_FAN_START S80\n") == 80);
    CHECK(parse_fan_paint_start_marker(";_PAINT_FAN_START S0") == 0);
    CHECK(parse_fan_paint_start_marker(";_PAINT_FAN_START S100\r\n") == 100);
    CHECK_FALSE(parse_fan_paint_start_marker(";_PAINT_FAN_START S101\n").has_value());
    CHECK_FALSE(parse_fan_paint_start_marker(";_PAINT_FAN_START S\n").has_value());
    CHECK_FALSE(parse_fan_paint_start_marker(";_PAINT_FAN_START S8x\n").has_value());
    CHECK_FALSE(parse_fan_paint_start_marker(";_PAINT_FAN_END\n").has_value());
    CHECK(fan_paint_entry_comment(80) == "; PAINT_FAN S80\n");
    CHECK(std::string(FAN_PAINT_EXIT_COMMENT) == "; PAINT_FAN END\n");
    CHECK(is_fan_paint_comment("; PAINT_FAN S80"));
    CHECK(is_fan_paint_comment("; PAINT_FAN END"));
    CHECK_FALSE(is_fan_paint_comment(";_PAINT_FAN_START S80"));
    CHECK_FALSE(is_fan_paint_comment("M106 S204"));
    CHECK(fan_paint_summary_line(3, 12) == "; fan_paint: zones=3 marked=12\n");
}

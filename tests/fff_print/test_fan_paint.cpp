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

#include <algorithm>

namespace {

Point mm(double x, double y) { return Point::new_scale(x, y); }

ExPolygon square(double x0, double y0, double x1, double y1)
{
    ExPolygon out;
    out.contour.points = { mm(x0, y0), mm(x1, y0), mm(x1, y1), mm(x0, y1) };
    return out;
}

FanPaintZone zone_of(ExPolygons area, uint8_t speed_pct, uint8_t line_mask)
{
    const BoundingBox bbox = get_extents(area);
    return FanPaintZone{ std::move(area), bbox, speed_pct, line_mask };
}

bool covers(const FanPaintZone &zone, double x, double y)
{
    const Point point = mm(x, y);
    return std::any_of(zone.area.begin(), zone.area.end(), [&point](const ExPolygon &expolygon) { return expolygon.contains(point); });
}

// Lines 1 (outer wall), 80 %.
constexpr uint8_t OUTER_WALL_80 = 17;
// Lines 7 (all), 0 %.
constexpr uint8_t ALL_LINES_0 = 127;

} // namespace

TEST_CASE("A painted projection is widened by the walls", "[fan_paint]") {
    Polygon projection;
    projection.points = { mm(0, 0), mm(10, 0), mm(10, 10), mm(0, 10) };
    const FanPaintZones zones = build_fan_paint_layer_zones({ { OUTER_WALL_80, Polygons{ projection } } }, {}, coord_t(scale_(1.)));
    REQUIRE(zones.size() == 1);
    CHECK(int(zones[0].speed_pct) == 80);
    CHECK(int(zones[0].line_mask) == FAN_PAINT_LINE_OUTER_WALL);
    CHECK(covers(zones[0], 10.5, 5.));
    CHECK_FALSE(covers(zones[0], 11.5, 5.));
}

TEST_CASE("A painted vertical face projects to a line and still makes a zone", "[fan_paint]") {
    Polygon line;
    line.points = { mm(0, 0), mm(10, 0), mm(5, 0) };
    const FanPaintZones zones = build_fan_paint_layer_zones({ { OUTER_WALL_80, Polygons{ line } } }, {}, coord_t(scale_(1.)));
    REQUIRE(zones.size() == 1);
    CHECK(covers(zones[0], 5., 0.5));
    CHECK_FALSE(covers(zones[0], 5., 1.5));
}

TEST_CASE("A band keeps the layer outline as is", "[fan_paint]") {
    const FanPaintZones zones = build_fan_paint_layer_zones({}, { { ALL_LINES_0, ExPolygons{ square(0, 0, 10, 10) } } }, coord_t(scale_(1.)));
    REQUIRE(zones.size() == 1);
    CHECK(int(zones[0].speed_pct) == 0);
    CHECK(int(zones[0].line_mask) == 7);
    CHECK(covers(zones[0], 5., 5.));
    CHECK_FALSE(covers(zones[0], 10.5, 5.));
}

TEST_CASE("Values that decode to nothing make no zone", "[fan_paint]") {
    Polygon projection;
    projection.points = { mm(0, 0), mm(10, 0), mm(10, 10), mm(0, 10) };
    CHECK(build_fan_paint_layer_zones({ { uint8_t(0), Polygons{ projection } }, { uint8_t(148), Polygons{ projection } } },
                                      { { uint8_t(148), ExPolygons{ square(0, 0, 10, 10) } } },
                                      coord_t(scale_(1.))).empty());
}

TEST_CASE("A path crossing a zone is split at its boundary", "[fan_paint]") {
    const FanPaintZones zones { zone_of({ square(10, -5, 20, 5) }, 80, FAN_PAINT_LINE_OUTER_WALL) };
    const std::vector<FanPaintSpan> spans = split_by_fan_paint_zones({ mm(0, 0), mm(30, 0) }, FAN_PAINT_LINE_OUTER_WALL, zones);
    REQUIRE(spans.size() == 3);
    CHECK(spans[0].speed_pct == -1);
    CHECK((spans[0].points == Points{ mm(0, 0), mm(10, 0) }));
    CHECK(spans[1].speed_pct == 80);
    CHECK((spans[1].points == Points{ mm(10, 0), mm(20, 0) }));
    CHECK(spans[2].speed_pct == -1);
    CHECK((spans[2].points == Points{ mm(20, 0), mm(30, 0) }));
}

TEST_CASE("A path of a line type the zone does not paint is not split", "[fan_paint]") {
    const FanPaintZones zones { zone_of({ square(10, -5, 20, 5) }, 80, FAN_PAINT_LINE_OUTER_WALL) };
    CHECK(split_by_fan_paint_zones({ mm(0, 0), mm(30, 0) }, FAN_PAINT_LINE_INNER_WALLS, zones).empty());
    CHECK(split_by_fan_paint_zones({ mm(0, 0), mm(30, 0) }, 0, zones).empty());
}

TEST_CASE("A path outside every zone is not split", "[fan_paint]") {
    const FanPaintZones zones { zone_of({ square(10, -5, 20, 5) }, 80, FAN_PAINT_LINE_OUTER_WALL) };
    CHECK(split_by_fan_paint_zones({ mm(0, 20), mm(30, 20) }, FAN_PAINT_LINE_OUTER_WALL, zones).empty());
}

TEST_CASE("Where fan paint zones overlap the higher speed wins", "[fan_paint]") {
    const FanPaintZones zones { zone_of({ square(0, -5, 20, 5) }, 30, FAN_PAINT_LINE_OUTER_WALL),
                                zone_of({ square(10, -5, 30, 5) }, 90, FAN_PAINT_LINE_OUTER_WALL) };
    const std::vector<FanPaintSpan> spans = split_by_fan_paint_zones({ mm(-10, 0), mm(40, 0) }, FAN_PAINT_LINE_OUTER_WALL, zones);
    REQUIRE(spans.size() == 4);
    CHECK(spans[0].speed_pct == -1);
    CHECK((spans[0].points == Points{ mm(-10, 0), mm(0, 0) }));
    CHECK(spans[1].speed_pct == 30);
    CHECK((spans[1].points == Points{ mm(0, 0), mm(10, 0) }));
    CHECK(spans[2].speed_pct == 90);
    CHECK((spans[2].points == Points{ mm(10, 0), mm(20, 0), mm(30, 0) }));
    CHECK(spans[3].speed_pct == -1);
    CHECK((spans[3].points == Points{ mm(30, 0), mm(40, 0) }));
}

TEST_CASE("A hole in a fan paint zone is outside it", "[fan_paint]") {
    ExPolygon with_hole = square(0, -10, 30, 10);
    Polygon hole;
    hole.points = { mm(10, -5), mm(10, 5), mm(20, 5), mm(20, -5) };
    with_hole.holes.push_back(hole);
    const FanPaintZones zones { zone_of({ with_hole }, 50, FAN_PAINT_LINE_OTHER) };
    const std::vector<FanPaintSpan> spans = split_by_fan_paint_zones({ mm(5, 0), mm(25, 0) }, FAN_PAINT_LINE_OTHER, zones);
    REQUIRE(spans.size() == 3);
    CHECK(spans[0].speed_pct == 50);
    CHECK((spans[0].points == Points{ mm(5, 0), mm(10, 0) }));
    CHECK(spans[1].speed_pct == -1);
    CHECK((spans[1].points == Points{ mm(10, 0), mm(20, 0) }));
    CHECK(spans[2].speed_pct == 50);
    CHECK((spans[2].points == Points{ mm(20, 0), mm(25, 0) }));
}

TEST_CASE("A split keeps the path's own vertices", "[fan_paint]") {
    const FanPaintZones zones { zone_of({ square(10, -5, 20, 5) }, 80, FAN_PAINT_LINE_OUTER_WALL) };
    const std::vector<FanPaintSpan> spans = split_by_fan_paint_zones({ mm(0, 0), mm(12, 0), mm(12, 3) }, FAN_PAINT_LINE_OUTER_WALL, zones);
    REQUIRE(spans.size() == 2);
    CHECK(spans[0].speed_pct == -1);
    CHECK((spans[0].points == Points{ mm(0, 0), mm(10, 0) }));
    CHECK(spans[1].speed_pct == 80);
    CHECK((spans[1].points == Points{ mm(10, 0), mm(12, 0), mm(12, 3) }));
}

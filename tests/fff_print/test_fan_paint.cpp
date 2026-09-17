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

#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/GCode/FanMover.hpp"

namespace {

// `gcode` through a FanMover that moves fan speed-ups `nb_seconds_delay` s earlier, with an optional kickstart
// (0 = disabled, the default -- matches the original no-kickstart helper), flushed as one layer. The feedrate
// is 10 mm/s, so each 10 mm move below takes 1 s.
std::string move_fans(const std::string &gcode, float nb_seconds_delay = 1.0f, float kickstart = 0.0f)
{
    GCodeWriter writer;
    writer.config.gcode_flavor.value = gcfMarlinFirmware;
    FanMover mover(writer, nb_seconds_delay, true, false, false, kickstart);
    return mover.process_gcode(gcode, true);
}

// The span of `out` from the `entry_comment` zone-entry comment up to (excluding) the next "; PAINT_FAN"
// comment (the zone's exit). Nothing painted should ever contain a bare M106/M107/M127 fan command in there.
std::string zone_span_after_entry(const std::string &out, const std::string &entry_comment) {
    const size_t entry = out.find(entry_comment);
    REQUIRE(entry != std::string::npos);
    const size_t next = out.find("; PAINT_FAN", entry + 1);
    REQUIRE(next != std::string::npos);
    return out.substr(entry, next - entry);
}

// The last "M106 ..." line in `out` before `marker` (e.g. a "; PAINT_FAN ..." comment) -- i.e. the fan state
// actually in effect in the G-code right before crossing into (or out of) a painted zone.
std::string last_fan_command_before(const std::string &out, const std::string &marker) {
    const size_t marker_pos = out.find(marker);
    REQUIRE(marker_pos != std::string::npos);
    const size_t cmd_pos = out.rfind("M106", marker_pos);
    REQUIRE(cmd_pos != std::string::npos);
    const size_t line_end = out.find('\n', cmd_pos);
    REQUIRE(line_end != std::string::npos);
    return out.substr(cmd_pos, line_end - cmd_pos);
}

} // namespace

TEST_CASE("FanMover still moves an unpainted fan speed-up earlier", "[fan_paint][FanMover]") {
    const std::string out = move_fans("G1 X0 Y0 F600\nG1 X10 Y0 E1\nG1 X20 Y0 E2\nM106 S204\nG1 X30 Y0 E3\n");
    INFO(out);
    CHECK(out.find("G1 X10 Y0 E1\nM106 S204\nG1 X20 Y0 E2\n") != std::string::npos);
}

TEST_CASE("FanMover leaves the fan command of a painted zone entry in place", "[fan_paint][FanMover]") {
    const std::string out = move_fans("G1 X0 Y0 F600\nG1 X10 Y0 E1\nG1 X20 Y0 E2\n; PAINT_FAN S80\nM106 S204\nG1 X30 Y0 E3\n");
    INFO(out);
    CHECK(out.find("G1 X20 Y0 E2\n; PAINT_FAN S80\nM106 S204\nG1 X30 Y0 E3\n") != std::string::npos);
}

TEST_CASE("FanMover moves nothing across a painted zone comment", "[fan_paint][FanMover]") {
    const std::string out = move_fans("G1 X0 Y0 F600\nG1 X10 Y0 E1\n; PAINT_FAN END\nG1 X20 Y0 E2\nM106 S255\nG1 X30 Y0 E3\n");
    INFO(out);
    CHECK(out.find("; PAINT_FAN END\nG1 X20 Y0 E2\nM106 S255\nG1 X30 Y0 E3\n") != std::string::npos);
}

TEST_CASE("FanMover disarms an in-flight kickstart at a painted zone entry (nb_seconds_delay=0)", "[fan_paint][FanMover]") {
    // M106 S255 is a >10% jump from the initial 0 speed: with nb_seconds_delay<=0 and kickstart>0 this arms
    // m_current_kickstart (FanMover.cpp), which only finishes counting down a couple of G1 moves later --
    // after the gcode has already entered the painted zone below.
    const std::string out = move_fans(
        "G1 X0 Y0 F600\n"
        "G1 X5 Y0 E1\n"
        "M106 S255\n"
        "G1 X8 Y0 E2\n"
        "; PAINT_FAN S100\n"
        "G1 X20 Y0 E3\n"
        "G1 X30 Y0 E4\n"
        "; PAINT_FAN END\n"
        "G1 X40 Y0 E5\n",
        /*nb_seconds_delay=*/0.0f, /*kickstart=*/0.5f);
    INFO(out);
    CHECK(zone_span_after_entry(out, "; PAINT_FAN S100").find("M106") == std::string::npos);
}

TEST_CASE("FanMover disarms an in-flight kickstart at a painted zone entry (nb_seconds_delay=1)", "[fan_paint][FanMover]") {
    // Same shape, but nb_seconds_delay>0 so the kickstart is armed through the other branch of the M106
    // handling (the "move it earlier" path, which falls back to m_current_kickstart when it can't splice the
    // kickstart command directly into the not-yet-full buffer).
    const std::string out = move_fans(
        "G1 X0 Y0 F600\n"
        "G1 X2 Y0 E1\n"
        "M106 S255\n"
        "G1 X4 Y0 E2\n"
        "; PAINT_FAN S100\n"
        "G1 X20 Y0 E3\n"
        "G1 X30 Y0 E4\n"
        "; PAINT_FAN END\n"
        "G1 X40 Y0 E5\n",
        /*nb_seconds_delay=*/1.0f, /*kickstart=*/0.5f);
    INFO(out);
    CHECK(zone_span_after_entry(out, "; PAINT_FAN S100").find("M106") == std::string::npos);
}

TEST_CASE("FanMover still leaves the fan command right after a painted zone exit in place when kickstart is enabled", "[fan_paint][FanMover]") {
    const std::string out = move_fans(
        "G1 X0 Y0 F600\nG1 X10 Y0 E1\n; PAINT_FAN END\nM106 S51\nG1 X20 Y0 E2\nG1 X30 Y0 E3\n",
        /*nb_seconds_delay=*/1.0f, /*kickstart=*/0.5f);
    INFO(out);
    CHECK(out.find("; PAINT_FAN END\nM106 S51\nG1 X20 Y0 E2\n") != std::string::npos);
}

TEST_CASE("FanMover writes the pending kickstart target before a painted zone entry instead of dropping it (nb_seconds_delay=0)", "[fan_paint][FanMover]") {
    // M106 S51 (20%) then M106 S204 (80%, a >10% jump) arms a kickstart for the 80% target via the
    // nb_seconds_delay<=0 branch. Before its countdown reaches zero, the zone entry comment arrives with the
    // SAME target speed (80%) as already "in effect" per FanMover's bookkeeping (m_front_buffer_fan_speed),
    // so C4 writes no fan command after it -- the target command must be written just before the barrier
    // instead, or the G-code is left stuck at the M106 S255 boost pulse forever.
    const std::string out = move_fans(
        "G1 X0 Y0 F600\n"
        "G1 X5 Y0 E1\n"
        "M106 S51\n"
        "M106 S204\n"
        "G1 X7 Y0 E2\n"
        "; PAINT_FAN S80\n"
        "G1 X20 Y0 E3\n"
        "G1 X30 Y0 E4\n"
        "; PAINT_FAN END\n"
        "G1 X40 Y0 E5\n",
        /*nb_seconds_delay=*/0.0f, /*kickstart=*/0.5f);
    INFO(out);
    CHECK(last_fan_command_before(out, "; PAINT_FAN S80") == "M106 S204");
    CHECK(zone_span_after_entry(out, "; PAINT_FAN S80").find("M106") == std::string::npos);
}

TEST_CASE("FanMover writes the pending kickstart target before a painted zone entry instead of dropping it (nb_seconds_delay=1)", "[fan_paint][FanMover]") {
    // Same shape, but nb_seconds_delay>0 (the other kickstart-arming branch). The move between M106 S204 and
    // the zone entry is short (0.05 s) -- long enough to not itself exceed the remaining kickstart duration
    // (which would let the *unmodified* countdown below finish the injection on its own, one line early, and
    // mask the bug) but short enough that the kickstart is still armed exactly when the barrier is reached.
    const std::string out = move_fans(
        "G1 X0 Y0 F600\n"
        "G1 X2 Y0 E1\n"
        "M106 S51\n"
        "M106 S204\n"
        "G1 X2.5 Y0 E1.05\n"
        "; PAINT_FAN S80\n"
        "G1 X20 Y0 E3\n"
        "G1 X30 Y0 E4\n"
        "; PAINT_FAN END\n"
        "G1 X40 Y0 E5\n",
        /*nb_seconds_delay=*/1.0f, /*kickstart=*/0.5f);
    INFO(out);
    CHECK(last_fan_command_before(out, "; PAINT_FAN S80") == "M106 S204");
    CHECK(zone_span_after_entry(out, "; PAINT_FAN S80").find("M106") == std::string::npos);
}

namespace {

// Value of the part-cooling fan command `line` carries, by the rule FanMover and CoolingBuffer share; fails the test
// case when `line` is no part-cooling fan command.
std::optional<float> part_fan_value(const std::string &line, GCodeFlavor flavor)
{
    const std::optional<PartFanCommand> command = parse_part_fan_command(line, flavor);
    INFO(line);
    REQUIRE(command.has_value());
    return command->value;
}

} // namespace

TEST_CASE("M106 without P or with P1 sets the part-cooling fan from S", "[fan_paint][FanMover]") {
    CHECK(part_fan_value("M106 S204", gcfMarlinFirmware) == std::optional<float>(204.f));
    CHECK(part_fan_value("M106 S255 ; enable fan", gcfMarlinLegacy) == std::optional<float>(255.f));
    CHECK(part_fan_value("M106 P1 S128", gcfKlipper) == std::optional<float>(128.f));
    CHECK(part_fan_value("M106 S0\n", gcfRepRapFirmware) == std::optional<float>(0.f));
}

TEST_CASE("M106 for another fan sets no part-cooling fan", "[fan_paint][FanMover]") {
    CHECK_FALSE(parse_part_fan_command("M106 P2 S0", gcfMarlinFirmware).has_value());
    CHECK_FALSE(parse_part_fan_command("M106 P2 S255 ; enable additional fan", gcfKlipper).has_value());
    CHECK_FALSE(parse_part_fan_command("M106 P3 S102", gcfMarlinFirmware).has_value());
    CHECK_FALSE(parse_part_fan_command("M106 P0 S255", gcfMarlinFirmware).has_value());
}

TEST_CASE("M107 and M127 turn the part-cooling fan off", "[fan_paint][FanMover]") {
    CHECK(part_fan_value("M107", gcfMarlinFirmware) == std::optional<float>(0.f));
    CHECK(part_fan_value("M107 ; disable fan", gcfKlipper) == std::optional<float>(0.f));
    CHECK(part_fan_value("M127", gcfMakerWare) == std::optional<float>(0.f));
    CHECK(part_fan_value("M127", gcfMarlinFirmware) == std::optional<float>(0.f));
}

TEST_CASE("Mach3 and Machinekit carry the part-cooling fan value in P", "[fan_paint][FanMover]") {
    CHECK(part_fan_value("M106 P204", gcfMach3) == std::optional<float>(204.f));
    CHECK(part_fan_value("M106 P2", gcfMachinekit) == std::optional<float>(2.f));
}

TEST_CASE("M126 sets the part-cooling fan only on MakerWare and Sailfish", "[fan_paint][FanMover]") {
    CHECK(part_fan_value("M126 T0", gcfMakerWare) == std::optional<float>(0.f));
    CHECK(part_fan_value("M126", gcfSailfish) == std::nullopt);
    CHECK_FALSE(parse_part_fan_command("M126 T0", gcfMarlinFirmware).has_value());
}

TEST_CASE("A part-cooling fan command without a readable value is still one", "[fan_paint][FanMover]") {
    CHECK(part_fan_value("M106", gcfMarlinFirmware) == std::nullopt);
    CHECK(part_fan_value("M106 P1", gcfKlipper) == std::nullopt);
}

TEST_CASE("Lines other than fan commands set no part-cooling fan", "[fan_paint][FanMover]") {
    CHECK_FALSE(parse_part_fan_command("G1 X10 Y0 S5", gcfMarlinFirmware).has_value());
    CHECK_FALSE(parse_part_fan_command("; M106 S204", gcfMarlinFirmware).has_value());
    CHECK_FALSE(parse_part_fan_command("", gcfMarlinFirmware).has_value());
}

TEST_CASE("Every fan command GCodeWriter writes reads back as a part-cooling fan command", "[fan_paint][FanMover]") {
    for (const GCodeFlavor flavor : { gcfMarlinLegacy, gcfKlipper, gcfRepRapFirmware, gcfMarlinFirmware, gcfTeacup, gcfMach3, gcfMachinekit, gcfMakerWare, gcfSailfish })
        for (unsigned int speed = 0; speed <= 100; ++ speed) {
            const std::string line = GCodeWriter::set_fan(flavor, speed);
            INFO(line);
            CHECK(parse_part_fan_command(line, flavor).has_value());
        }
}

TEST_CASE("A part-cooling fan command names the speed GCodeWriter writes it for", "[fan_paint][FanMover]") {
    CHECK(part_fan_speed_pct("M106 S204", gcfMarlinFirmware) == std::optional<int>(80));
    CHECK(part_fan_speed_pct("M106 P1 S255", gcfKlipper) == std::optional<int>(100));
    CHECK(part_fan_speed_pct("M106 S12 ; enable fan", gcfMarlinFirmware) == std::optional<int>(5));
    CHECK(part_fan_speed_pct("M106 S0", gcfMarlinFirmware) == std::optional<int>(0));
    CHECK(part_fan_speed_pct("M107", gcfMarlinFirmware) == std::optional<int>(0));
    CHECK(part_fan_speed_pct("M106 P204", gcfMach3) == std::optional<int>(80));
    CHECK(part_fan_speed_pct("M107", gcfMachinekit) == std::optional<int>(0));
    CHECK(part_fan_speed_pct("M127", gcfSailfish) == std::optional<int>(0));
}

TEST_CASE("A fan command names no speed when GCodeWriter writes it for none", "[fan_paint][FanMover]") {
    // 203 lies between the values of 79 % (201) and 80 % (204).
    CHECK(part_fan_speed_pct("M106 S203", gcfMarlinFirmware) == std::nullopt);
    CHECK(part_fan_speed_pct("M106", gcfMarlinFirmware) == std::nullopt);
    CHECK(part_fan_speed_pct("M106 P2 S0", gcfMarlinFirmware) == std::nullopt);
    CHECK(part_fan_speed_pct("G1 X10", gcfMarlinFirmware) == std::nullopt);
    // Mach3 carries the value in P, so M106 S204 has none there.
    CHECK(part_fan_speed_pct("M106 S204", gcfMach3) == std::nullopt);
    // M126 switches the fan on at every speed, and its T word is no speed.
    CHECK(part_fan_speed_pct("M126", gcfMakerWare) == std::nullopt);
    CHECK(part_fan_speed_pct("M126 T0", gcfMakerWare) == std::nullopt);
}

TEST_CASE("Every speed GCodeWriter writes a fan command for reads back as that speed", "[fan_paint][FanMover]") {
    for (const GCodeFlavor flavor : { gcfMarlinLegacy, gcfKlipper, gcfRepRapFirmware, gcfMarlinFirmware, gcfTeacup, gcfMach3, gcfMachinekit })
        for (int speed = 1; speed <= 100; ++ speed) {
            const std::string line = GCodeWriter::set_fan(flavor, speed);
            INFO(line);
            CHECK(part_fan_speed_pct(line, flavor) == std::optional<int>(speed));
        }
    for (const GCodeFlavor flavor : { gcfMarlinLegacy, gcfKlipper, gcfRepRapFirmware, gcfMarlinFirmware, gcfTeacup, gcfMakerWare, gcfSailfish })
        CHECK(part_fan_speed_pct(GCodeWriter::set_fan(flavor, 0), flavor) == std::optional<int>(0));
}

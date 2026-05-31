#include <catch2/catch.hpp>

#include <sstream>
#include <string>

#include "test_data.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/GCode.hpp"  // for compute_park_point_from_polygons declaration
#include "libslic3r/GCode/CoolingBuffer.hpp"  // for parse_park_hint_line

using namespace Slic3r;
using namespace Slic3r::Test;

// === Unit tests for emit_cooling_tower_visit ===
//
// Helper to populate inputs with sensible defaults so each test below only
// has to override what it cares about. Mirrors the standalone harness used
// during development.
static CoolingTowerVisitInputs make_tower_inputs() {
    CoolingTowerVisitInputs in{};
    in.layer_id           = 5;
    in.tower_xy           = Vec2f(200.f, 50.f);
    // Pre-visit head position on the model — distinct from tower_xy so tests
    // can verify the visit returns the head here before dropping Z.
    in.return_xy          = Vec2f(150.f, 50.f);
    in.prev_tower_top_z   = 1.0f;
    in.cur_z              = 1.2f;
    in.z_hop              = 0.4f;
    in.pause_needed       = 4.71f;          // one full loop @ Ø15 / 10 mm/s
    in.retract_length     = 4.f;
    in.diameter           = 15.f;
    in.tower_layer_height = 0.2f;
    in.tower_line_width   = 0.42f;
    in.filament_diameter  = 1.75f;
    in.tower_speed        = 10.f;
    in.travel_speed       = 200.f;
    in.travel_speed_z     = 10.f;
    in.retract_speed      = 30.f;
    return in;
}

TEST_CASE("emit_cooling_tower_visit emits one full loop for a circumference-worth dwell",
          "[cooling_tower][cooling]") {
    auto in = make_tower_inputs();   // pause_needed = 4.71s ~ 1 full loop
    std::string out;
    float top = emit_cooling_tower_visit(in, out);

    CHECK(out.find("; COOLING_TOWER_VISIT") != std::string::npos);
    CHECK(out.find("; END_COOLING_TOWER_VISIT") != std::string::npos);
    CHECK(out.find("G92 E0") != std::string::npos);
    CHECK(out.find("G1 E-4") != std::string::npos);
    // New contract: z_start = prev_tower_top_z (no step-up). One full loop rises
    // by tower_layer_height (the per-visit target rise). prev (1.0) + 0.2 = 1.2.
    CHECK(top == Approx(1.2f).margin(0.001f));

    // 20 polyline segments per loop -> one full loop emits exactly 20 G1 X moves
    size_t pos = 0;
    int spiral_moves = 0;
    while ((pos = out.find("G1 X", pos)) != std::string::npos) { ++spiral_moves; pos += 4; }
    CHECK(spiral_moves == 20);

    // Absolute-E default: no M82/M83 toggling.
    CHECK(out.find("M82") == std::string::npos);
    CHECK(out.find("M83") == std::string::npos);
}

TEST_CASE("emit_cooling_tower_visit wraps in M82/M83 when use_relative_e_distances=true",
          "[cooling_tower][cooling]") {
    auto in = make_tower_inputs();
    in.use_relative_e_distances = true;

    std::string out;
    emit_cooling_tower_visit(in, out);

    const size_t p_hdr = out.find("; COOLING_TOWER_VISIT");
    const size_t p_m82 = out.find("M82");
    const size_t p_g92 = out.find("G92 E0");
    const size_t p_m83 = out.find("M83");
    const size_t p_end = out.find("; END_COOLING_TOWER_VISIT");

    REQUIRE(p_m82 != std::string::npos);
    REQUIRE(p_m83 != std::string::npos);
    CHECK(p_hdr < p_m82);
    CHECK(p_m82 < p_g92);
    CHECK(p_m83 < p_end);
}

TEST_CASE("emit_cooling_tower_visit starts spiral at prev_tower_top_z with no step-up",
          "[cooling_tower][cooling]") {
    auto in = make_tower_inputs();
    in.prev_tower_top_z = 0.f;       // empty tower: print start
    in.cur_z            = 0.3f;
    in.layer_id         = 1;

    std::string out;
    float top = emit_cooling_tower_visit(in, out);

    // New contract: z_start = prev (0) directly; one full loop rises by
    // tower_layer_height (0.2) -> top = 0.2. The step-up gap that used to
    // double the per-visit rise has been removed; the tower's vertical
    // growth is now bounded by the caller-supplied target rise.
    CHECK(top == Approx(0.2f).margin(0.001f));
    CHECK(out.find("z_start=0.00") != std::string::npos);
}

TEST_CASE("emit_cooling_tower_visit half-loop dwell -> half a layer_height rise",
          "[cooling_tower][cooling]") {
    auto in = make_tower_inputs();
    in.pause_needed = 2.356f;        // half circumference (π·15/2) / 10mm/s

    std::string out;
    float top = emit_cooling_tower_visit(in, out);

    // z_start = prev (1.0); half a target rise (0.1) -> top = 1.1
    CHECK(top == Approx(1.1f).margin(0.005f));

    size_t pos = 0;
    int spiral_moves = 0;
    while ((pos = out.find("G1 X", pos)) != std::string::npos) { ++spiral_moves; pos += 4; }
    CHECK(spiral_moves == 10);
}

TEST_CASE("emit_cooling_tower_visit caps spiral at one full loop for long dwell",
          "[cooling_tower][cooling]") {
    // Pause large enough that the old contract would have run 3 full loops.
    // Under the new contract, the spiral is bounded to one loop so the tower
    // grows by at most `tower_layer_height` per visit, matching the model's
    // layer rate when the caller passes target_rise = model layer_height.
    auto in = make_tower_inputs();
    in.pause_needed = 14.13f;        // ~3 * circumference / tower_speed

    std::string out;
    float top = emit_cooling_tower_visit(in, out);

    // Exactly one loop's worth of segments — not three.
    size_t pos = 0;
    int spiral_moves = 0;
    while ((pos = out.find("G1 X", pos)) != std::string::npos) { ++spiral_moves; pos += 4; }
    CHECK(spiral_moves == 20);

    // Vertical rise is capped at tower_layer_height (0.2) regardless of dwell.
    CHECK(top == Approx(1.2f).margin(0.001f));
}

// Regression: every cooling-tower visit MUST return the head to `return_xy`
// at `cur_z` before the closing deretract, so that any deferred wipe or
// next-print travel finds the head where it expects (on the model). The pre-
// fix code dropped Z back to cur_z while still at the tower XY, then the
// follow-on wipe move ("G1 X<last-extrusion> E-...") executed as a 30–50mm
// diagonal traverse across the build at print Z — visible in 3DBenchy_PA prints
// as cobweb stringing concentrated around small upper features (cabin, funnel)
// because the wipe path crossed straight through them.
TEST_CASE("emit_cooling_tower_visit returns head to return_xy before dropping Z",
          "[cooling_tower][cooling]") {
    auto in = make_tower_inputs();  // tower_xy=(200,50), return_xy=(150,50), cur_z=1.2
    std::string out;
    emit_cooling_tower_visit(in, out);

    // Tail of the visit, after END_COOLING_TOWER_VISIT, must contain a travel
    // back to return_xy. We locate the LAST G0 X… Y… in the block and confirm
    // its coordinates match return_xy.
    const size_t end_marker = out.find("; END_COOLING_TOWER_VISIT");
    REQUIRE(end_marker != std::string::npos);

    // The return travel must happen INSIDE the visit block, not after it,
    // so the visit hands the caller a head positioned at return_xy.
    const std::string body = out.substr(0, end_marker);

    // Find the LAST G0 in the body (the return travel).
    const size_t last_g0 = body.rfind("G0 X");
    REQUIRE(last_g0 != std::string::npos);

    // After the last G0, before END, the only Z motion must drop from safe_z
    // BACK to cur_z (1.20) — never above. And the deretract `G1 E0` must
    // happen AFTER the return travel.
    const std::string tail = body.substr(last_g0);
    CHECK(tail.find("X150.00 Y50.00") != std::string::npos);  // return travel
    // The Z drop and deretract sit after the return travel.
    const size_t pos_z_drop = tail.find("G1 Z1.20");
    const size_t pos_unret  = tail.find("G1 E0 F");
    CHECK(pos_z_drop != std::string::npos);
    CHECK(pos_unret  != std::string::npos);
    CHECK(pos_z_drop < pos_unret);
}

TEST_CASE("emit_cooling_tower_visit zero-rise spiral lays a flat ring at prev_top",
          "[cooling_tower][cooling]") {
    // Edge case for visits where the tower top has already caught up with the
    // model (cur_z == prev_top). Caller passes target_rise = 0; the spiral
    // should emit one closed ring at z = prev_top without rising.
    auto in = make_tower_inputs();
    in.prev_tower_top_z   = 5.0f;
    in.cur_z              = 5.0f;
    in.tower_layer_height = 0.f;     // target rise

    std::string out;
    float top = emit_cooling_tower_visit(in, out);

    CHECK(top == Approx(5.0f).margin(0.001f));
    CHECK(out.find("z_start=5.00") != std::string::npos);
}

TEST_CASE("compute_tower_visit_speed returns desired speed when in range",
          "[cooling_tower][cooling]") {
    // circumference 47.124 mm, pause 5.236 s -> desired = 9.0 mm/s
    const float c = 3.14159265358979323846f * 15.f;
    float v = Slic3r::compute_tower_visit_speed(/*pause=*/5.236f, /*circumference=*/c,
                                                /*min_speed=*/1.5f, /*max_speed=*/20.f);
    CHECK(v == Approx(9.f).margin(0.01f));
}

TEST_CASE("compute_tower_visit_speed clamps to max_speed when desired exceeds it",
          "[cooling_tower][cooling]") {
    // Tiny pause -> very high desired speed; should clamp to max.
    const float c = 3.14159265358979323846f * 15.f;
    float v = Slic3r::compute_tower_visit_speed(/*pause=*/1.f, c, /*min=*/1.5f, /*max=*/10.f);
    CHECK(v == Approx(10.f).margin(0.001f));
}

TEST_CASE("compute_tower_visit_speed clamps to min_speed when desired below floor",
          "[cooling_tower][cooling]") {
    // Long pause -> very low desired speed; should clamp to min (caller emits
    // a G4 to absorb the leftover dwell).
    const float c = 3.14159265358979323846f * 15.f;
    float v = Slic3r::compute_tower_visit_speed(/*pause=*/100.f, c, /*min=*/1.5f, /*max=*/10.f);
    CHECK(v == Approx(1.5f).margin(0.001f));
}

TEST_CASE("compute_tower_residual_dwell is zero when one loop already covers the pause",
          "[cooling_tower][cooling]") {
    // 1 loop @ 9 mm/s over 47.124 mm takes ~5.236 s — equal to pause -> no G4.
    const float c = 3.14159265358979323846f * 15.f;
    float r = Slic3r::compute_tower_residual_dwell(/*pause=*/5.236f, /*actual_speed=*/9.f, c);
    CHECK(r == Approx(0.f).margin(0.001f));
}

TEST_CASE("compute_tower_residual_dwell returns leftover when speed clamped to floor",
          "[cooling_tower][cooling]") {
    // pause 100s, but at clamped min_speed=1.5 one loop only takes 31.416s.
    // Residual = 100 - 31.416 = 68.584 s; caller must emit a G4 for the rest.
    const float c = 3.14159265358979323846f * 15.f;
    float r = Slic3r::compute_tower_residual_dwell(/*pause=*/100.f, /*actual_speed=*/1.5f, c);
    CHECK(r == Approx(68.584f).margin(0.01f));
}

TEST_CASE("parse_park_hint_line recognizes strategy=cooling_tower",
          "[cooling_tower][cooling]") {
    auto hint = Slic3r::parse_park_hint_line(
        "; PARK_HINT extruder=0 x=300.000 y=20.000 strategy=cooling_tower");
    REQUIRE(hint.has_value());
    CHECK(hint->extruder_id == 0u);
    CHECK_FALSE(hint->is_park_and_wait);
    CHECK(hint->is_cooling_tower);
    CHECK(hint->point.x() == Approx(300.f).margin(0.01f));
    CHECK(hint->point.y() == Approx(20.f).margin(0.01f));
}

TEST_CASE("compute_cooling_tower_xy places tower to the right of model bbox when bed has room",
          "[cooling_tower][cooling]") {
    // Model 100×100 mm in the front-left quadrant of a 400×400 bed -> right side is free.
    BoundingBoxf model(Vec2d(50, 50), Vec2d(150, 150));
    BoundingBoxf bed(Vec2d(0, 0), Vec2d(400, 400));

    Vec2f xy = compute_cooling_tower_xy(model, bed, /*diameter=*/15.f, /*margin=*/3.f);

    // Expected: x = model.max.x + half = 150 + 10.5 = 160.5, y = model center = 100.
    CHECK(xy.x() == Approx(160.5f).margin(0.01f));
    CHECK(xy.y() == Approx(100.f).margin(0.01f));
}

TEST_CASE("compute_cooling_tower_xy falls back to a bed corner when no edge has room",
          "[cooling_tower][cooling]") {
    // Model fills almost the entire bed -> no edge slot fits.
    BoundingBoxf model(Vec2d(15, 15), Vec2d(385, 385));
    BoundingBoxf bed(Vec2d(0, 0), Vec2d(400, 400));

    Vec2f xy = compute_cooling_tower_xy(model, bed, /*diameter=*/15.f, /*margin=*/3.f);

    // Result must still be inside the bed minus the half-radius margin.
    CHECK(xy.x() >= 10.5f);
    CHECK(xy.x() <= 389.5f);
    CHECK(xy.y() >= 10.5f);
    CHECK(xy.y() <= 389.5f);
}

TEST_CASE("resolve_cooling_tower_xy returns the user-pinned point verbatim",
          "[cooling_tower][cooling]") {
    BoundingBoxf bed(Vec2d(0, 0), Vec2d(300, 300));
    BoundingBoxf model(Vec2d(50, 50), Vec2d(120, 120));
    std::vector<Vec2d> pinned = { Vec2d(42.5, 17.0) };

    Vec2f xy = Slic3r::resolve_cooling_tower_xy(bed, model, /*diameter=*/15.f, pinned);
    CHECK(xy.x() == Approx(42.5f).margin(0.001f));
    CHECK(xy.y() == Approx(17.0f).margin(0.001f));
}

TEST_CASE("resolve_cooling_tower_xy auto-places adjacent to the model when no pin",
          "[cooling_tower][cooling]") {
    // Small object in the lower-left of a CD400-sized bed — exactly the
    // scenario that produced the visible bug (tower landed in the bed corner
    // instead of next to the benchy).
    BoundingBoxf bed(Vec2d(0, 0), Vec2d(300, 300));
    BoundingBoxf model(Vec2d(60, 60), Vec2d(120, 120));

    Vec2f xy = Slic3r::resolve_cooling_tower_xy(bed, model, /*diameter=*/15.f, /*pinned=*/{});

    // Right of model: x = 120 + half (10.5) = 130.5, y = model center (90).
    CHECK(xy.x() == Approx(130.5f).margin(0.01f));
    CHECK(xy.y() == Approx(90.f).margin(0.01f));
}

TEST_CASE("resolve_cooling_tower_xy uses the legacy bed-corner fallback when model bbox is empty",
          "[cooling_tower][cooling]") {
    // Default-constructed BoundingBoxf is `defined=false`; the resolver must
    // not feed it into compute_cooling_tower_xy (would produce garbage).
    BoundingBoxf bed(Vec2d(0, 0), Vec2d(300, 300));
    BoundingBoxf empty_model;   // .defined == false

    Vec2f xy = Slic3r::resolve_cooling_tower_xy(bed, empty_model, /*diameter=*/15.f, /*pinned=*/{});

    // Bed's right-front corner with the same offset the old hardcode used
    // (half = 10.5): (bed.max.x - 10.5, bed.min.y + 10.5) = (289.5, 10.5).
    CHECK(xy.x() == Approx(289.5f).margin(0.01f));
    CHECK(xy.y() == Approx(10.5f).margin(0.01f));
}

// === Cooling-tower strategy: enum value + config field defaults ===
//
// `cooling_tower` is the third option for `min_layer_time_strategy`. When the
// layer would print faster than the cooling threshold, the toolhead diverts to
// a hollow spiral-vase tower printed alongside the model and keeps extruding
// the tower wall for the duration of the dwell. The tower has its own brim and
// is auto-placed (or pinned via `cooling_tower_position`). New config fields:
//   cooling_tower_diameter   (mm, default 15.0)  outer Ø of the spiral
//   cooling_tower_speed      (mm/s, default 10)  extrusion speed at the tower
//   cooling_tower_min_dwell  (s, default 1.5)    below this — strategy degrades
//                                                to G4 dwell (no tower visit)
//   cooling_tower_position   (Points)            empty -> auto-place near model

TEST_CASE("MinLayerTimeStrategy adds CoolingTower as a third value",
          "[cooling_tower][cooling]") {
    CHECK(static_cast<int>(mltsSlowdown)     == 0);
    CHECK(static_cast<int>(mltsParkAndWait)  == 1);
    CHECK(static_cast<int>(mltsCoolingTower) == 2);
}

TEST_CASE("MinLayerTimeStrategy round-trips \"cooling_tower\" string key",
          "[cooling_tower][cooling]") {
    const auto &m = ConfigOptionEnum<MinLayerTimeStrategy>::get_enum_values();
    auto it = m.find("cooling_tower");
    REQUIRE(it != m.end());
    CHECK(it->second == static_cast<int>(mltsCoolingTower));
}

TEST_CASE("min_layer_time_strategy registers \"cooling_tower\" as an enum value",
          "[cooling_tower][cooling]") {
    auto cfg = DynamicPrintConfig::full_print_config();
    const ConfigOptionDef *def = cfg.def()->get("min_layer_time_strategy");
    REQUIRE(def != nullptr);
    bool has = false;
    for (const std::string &v : def->enum_values)
        if (v == "cooling_tower") { has = true; break; }
    CHECK(has);
}

TEST_CASE("cooling-tower config fields ship with the expected defaults",
          "[cooling_tower][cooling]") {
    auto cfg = DynamicPrintConfig::full_print_config();

    SECTION("cooling_tower_diameter defaults to 15 mm") {
        const auto *o = cfg.option<ConfigOptionFloat>("cooling_tower_diameter");
        REQUIRE(o != nullptr);
        CHECK(o->value == Approx(15.0));
    }
    SECTION("cooling_tower_speed defaults to 10 mm/s") {
        const auto *o = cfg.option<ConfigOptionFloat>("cooling_tower_speed");
        REQUIRE(o != nullptr);
        CHECK(o->value == Approx(10.0));
    }
    SECTION("cooling_tower_min_dwell defaults to 1.5 s") {
        const auto *o = cfg.option<ConfigOptionFloat>("cooling_tower_min_dwell");
        REQUIRE(o != nullptr);
        CHECK(o->value == Approx(1.5));
    }
    SECTION("cooling_tower_position defaults to empty (auto-place)") {
        const auto *o = cfg.option<ConfigOptionPoints>("cooling_tower_position");
        REQUIRE(o != nullptr);
        CHECK(o->values.empty());
    }
}


TEST_CASE("compute_park_point picks nearest point inside infill polygon", "[park_and_wait]") {
    // Square infill polygon at (0..100, 0..100), nozzle currently at (150, 50)
    Polygons infill = { Polygon::new_scale({{0,0}, {100,0}, {100,100}, {0,100}}) };
    Vec2f last_pos(150.f, 50.f);

    auto park = compute_park_point_from_polygons(
        infill, last_pos, /*nozzle_d=*/0.4f,
        /*bed=*/BoundingBoxf(Vec2d(0,0), Vec2d(300,300)));

    REQUIRE(park.has_value());
    // Nearest infill point should be on the right edge near (100, 50),
    // inset by nozzle_d (0.4 mm) to keep the nozzle off the polygon edge
    CHECK(park->x() == Approx(100.f - 0.4f).margin(0.01f));
    CHECK(park->y() == Approx(50.f).margin(0.01f));
}

TEST_CASE("compute_park_point returns nullopt for empty infill", "[park_and_wait]") {
    Polygons infill = {};
    auto park = compute_park_point_from_polygons(
        infill, Vec2f(10,10), 0.4f,
        BoundingBoxf(Vec2d(0,0), Vec2d(300,300)));
    CHECK_FALSE(park.has_value());
}

TEST_CASE("compute_park_point clamps result inside bed bbox minus margin", "[park_and_wait]") {
    // Single infill polygon at the bed corner, nozzle far away
    Polygons infill = { Polygon::new_scale({{0,0}, {2,0}, {2,2}, {0,2}}) };
    Vec2f last_pos(-100.f, -100.f);

    auto park = compute_park_point_from_polygons(
        infill, last_pos, 0.4f,
        BoundingBoxf(Vec2d(0,0), Vec2d(300,300)));

    REQUIRE(park.has_value());
    // Inside bed bbox shrunk by 5mm margin on both sides
    CHECK(park->x() >= 5.f);
    CHECK(park->x() <= 295.f);
    CHECK(park->y() >= 5.f);
    CHECK(park->y() <= 295.f);
}

TEST_CASE("compute_park_point picks nearest among multiple infill polygons", "[park_and_wait]") {
    // Two disjoint infill regions; nozzle is closer to the second
    Polygons infill = {
        Polygon::new_scale({{  0,  0}, { 10,  0}, { 10, 10}, {  0, 10}}),  // far
        Polygon::new_scale({{100,100}, {110,100}, {110,110}, {100,110}}),  // near
    };
    Vec2f last_pos(115.f, 105.f);

    auto park = compute_park_point_from_polygons(
        infill, last_pos, 0.4f,
        BoundingBoxf(Vec2d(0,0), Vec2d(300,300)));

    REQUIRE(park.has_value());
    // Result must come from the second polygon (right cluster), not the first
    CHECK(park->x() > 90.f);
    CHECK(park->y() > 90.f);
}

TEST_CASE("compute_park_point falls back when infill exists only outside the bed", "[park_and_wait]") {
    // Polygon entirely outside the printable area
    Polygons infill = { Polygon::new_scale({{400, 400}, {410, 400}, {410, 410}, {400, 410}}) };
    Vec2f last_pos(200.f, 200.f);

    auto park = compute_park_point_from_polygons(
        infill, last_pos, 0.4f,
        BoundingBoxf(Vec2d(0,0), Vec2d(300,300)));

    // Contract decision: when the chosen point would lie outside the bed,
    // it is clamped to bed - 5mm margin. The function should never return
    // a point outside the bed. We don't require nullopt here — Task 8 may
    // choose either policy. We DO require: any returned value must be
    // inside the bed minus margin.
    if (park.has_value()) {
        CHECK(park->x() >= 5.f);
        CHECK(park->x() <= 295.f);
        CHECK(park->y() >= 5.f);
        CHECK(park->y() <= 295.f);
    }
}

// === Integration tests against full slicing pipeline ===

TEST_CASE("GCode emits PARK_HINT trailer when strategy=park_and_wait", "[park_and_wait]") {
    auto config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "min_layer_time_strategy", "park_and_wait" },
        { "slow_down_layer_time",    "10" },
    });

    std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config);

    int park_hint_count = 0;
    int first_layer_change = -1;
    int line_idx = 0;
    std::istringstream iss(gcode);
    for (std::string line; std::getline(iss, line); ) {
        if (first_layer_change < 0 && line.find(";LAYER_CHANGE") != std::string::npos)
            first_layer_change = line_idx;
        if (first_layer_change >= 0
            && line_idx > first_layer_change
            && line.find("; PARK_HINT") != std::string::npos)
            ++park_hint_count;
        ++line_idx;
    }
    CHECK(park_hint_count > 0);
}

TEST_CASE("GCode emits NO PARK_HINT when strategy=slowdown (backward compat)", "[park_and_wait]") {
    auto config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "slow_down_layer_time", "10" },
    });

    std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config);

    CHECK(gcode.find("PARK_HINT") == std::string::npos);
}

// === Unit test for PARK_HINT comment parser ===

TEST_CASE("parse_park_hint_line extracts extruder/x/y/strategy from well-formed line", "[park_and_wait][cooling]") {
    auto hint = Slic3r::parse_park_hint_line(
        "; PARK_HINT extruder=0 x=15.500 y=12.300 strategy=park_and_wait");
    REQUIRE(hint.has_value());
    CHECK(hint->extruder_id == 0u);
    CHECK(hint->point.x() == Approx(15.5f).margin(0.01f));
    CHECK(hint->point.y() == Approx(12.3f).margin(0.01f));
    CHECK(hint->is_park_and_wait);
}

TEST_CASE("parse_park_hint_line accepts trailing newline", "[park_and_wait][cooling]") {
    auto hint = Slic3r::parse_park_hint_line(
        "; PARK_HINT extruder=2 x=100.000 y=200.000 strategy=park_and_wait\n");
    REQUIRE(hint.has_value());
    CHECK(hint->extruder_id == 2u);
    CHECK(hint->point.x() == Approx(100.f).margin(0.01f));
    CHECK(hint->point.y() == Approx(200.f).margin(0.01f));
    CHECK(hint->is_park_and_wait);
}

TEST_CASE("parse_park_hint_line returns nullopt for non-PARK_HINT lines", "[park_and_wait][cooling]") {
    CHECK_FALSE(Slic3r::parse_park_hint_line("G1 X10 Y10").has_value());
    CHECK_FALSE(Slic3r::parse_park_hint_line("; LAYER_CHANGE").has_value());
    CHECK_FALSE(Slic3r::parse_park_hint_line("").has_value());
    CHECK_FALSE(Slic3r::parse_park_hint_line("; PARK_HINT garbage").has_value());
}

TEST_CASE("parse_park_hint_line flags is_park_and_wait=false for unknown strategy", "[park_and_wait][cooling]") {
    auto hint = Slic3r::parse_park_hint_line(
        "; PARK_HINT extruder=0 x=5.0 y=5.0 strategy=something_else");
    // Well-formed shape, just unknown strategy — graceful degradation
    REQUIRE(hint.has_value());
    CHECK_FALSE(hint->is_park_and_wait);
}

// === Unit tests for compute_park_pause_needed ===

TEST_CASE("compute_park_pause_needed returns 0 when layer already above threshold", "[park_and_wait][cooling]") {
    // Layer takes 15s, threshold is 10s — no pause needed.
    float pause = Slic3r::compute_park_pause_needed(
        /*threshold=*/10.f, /*layer_time=*/15.f,
        /*park_point=*/Vec2f(50.f, 50.f),
        /*current_xy=*/Vec2f(0.f, 0.f),
        /*park_retract_length=*/1.f, /*park_z_hop=*/0.5f,
        /*travel_speed=*/200.f, /*travel_speed_z=*/10.f, /*retract_speed=*/40.f);
    CHECK(pause == Approx(0.f));
}

TEST_CASE("compute_park_pause_needed returns 0 when no park_point available", "[park_and_wait][cooling]") {
    float pause = Slic3r::compute_park_pause_needed(
        10.f, 2.f, std::nullopt, Vec2f(0.f, 0.f),
        1.f, 0.5f, 200.f, 10.f, 40.f);
    CHECK(pause == Approx(0.f));
}

TEST_CASE("compute_park_pause_needed subtracts travel+retract+zhop overhead", "[park_and_wait][cooling]") {
    // Layer = 2s, threshold = 10s, gap = 8s.
    // park_point at (100,0), current at (0,0): dist = 100mm, travel @ 200mm/s
    //   travel_t  = 2 * 100 / 200 = 1.0s
    // retract = 1mm @ 40mm/s -> retract_t = 2 * 1 / 40 = 0.05s
    // z_hop = 0.5mm @ 10mm/s -> zhop_t = 2 * 0.5 / 10 = 0.1s
    // overhead = 1.15s; pause = 8 - 1.15 = 6.85s
    float pause = Slic3r::compute_park_pause_needed(
        /*threshold=*/10.f, /*layer_time=*/2.f,
        Vec2f(100.f, 0.f), Vec2f(0.f, 0.f),
        /*park_retract_length=*/1.f, /*park_z_hop=*/0.5f,
        /*travel_speed=*/200.f, /*travel_speed_z=*/10.f, /*retract_speed=*/40.f);
    CHECK(pause == Approx(6.85f).margin(0.01f));
}

TEST_CASE("compute_park_pause_needed clamps to 0 when overhead exceeds gap", "[park_and_wait][cooling]") {
    // Gap = 0.5s but travel alone is 1s — no pause; just go to park, no extra wait.
    float pause = Slic3r::compute_park_pause_needed(
        /*threshold=*/2.5f, /*layer_time=*/2.f,
        Vec2f(100.f, 0.f), Vec2f(0.f, 0.f),
        1.f, 0.5f, 200.f, 10.f, 40.f);
    CHECK(pause == Approx(0.f));
}

TEST_CASE("compute_park_pause_needed falls back to travel_speed when z_speed is 0", "[park_and_wait][cooling]") {
    // travel_speed_z = 0 -> falls back to travel_speed for z motions.
    // dist_xy = 0, retract = 0, z_hop = 1mm at travel_speed 100mm/s
    //   zhop_t = 2 * 1 / 100 = 0.02s
    // gap = 10 - 2 = 8s; pause = 8 - 0.02 = 7.98s
    float pause = Slic3r::compute_park_pause_needed(
        10.f, 2.f, Vec2f(0.f, 0.f), Vec2f(0.f, 0.f),
        /*park_retract_length=*/0.f, /*park_z_hop=*/1.f,
        /*travel_speed=*/100.f, /*travel_speed_z=*/0.f, /*retract_speed=*/40.f);
    CHECK(pause == Approx(7.98f).margin(0.01f));
}

// === Integration test: end-to-end strategy=park_and_wait skips feedrate stretching ===

TEST_CASE("ParkAndWait strategy does NOT modify feedrates", "[park_and_wait][cooling][.slice_blocked]") {
    // BLOCKED: Test::slice baseline SIGSEGV in arrange_objects/InfiniteBed.
    // This test is checked in for execution once the OrcaSlicer test harness is fixed.
    // The [.slice_blocked] tag hides this case from default runs (Catch2 skips
    // tests tagged with a leading '.' unless that tag is requested explicitly).
    auto config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "min_layer_time_strategy", "park_and_wait" },
        { "slow_down_layer_time",    "10" },
        { "inner_wall_speed",        "60" },
    });

    std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config);

    // Some extrusion line should still run at F3600 (60 mm/s) — i.e. NOT slowed down.
    bool found_at_full_speed = gcode.find(" F3600") != std::string::npos;
    CHECK(found_at_full_speed);
}

// === Unit tests for emit_park_sequence ===

TEST_CASE("emit_park_sequence produces well-formed block with Z-invariant", "[park_and_wait][cooling]") {
    ParkSequenceInputs in;
    in.layer_id            = 7;
    in.park_point          = Vec2f(15.5f, 12.3f);
    in.cur_z               = 1.2f;
    in.park_z_hop          = 0.4f;
    in.pause_needed        = 5.3f;
    in.park_retract_length = 4.0f;
    in.travel_speed        = 200.f;
    in.travel_speed_z      = 10.f;
    in.retract_speed       = 30.f;

    std::string out;
    emit_park_sequence(in, out);

    // Begin/end markers
    CHECK(out.find("; PARK_AND_WAIT layer_id=7") != std::string::npos);
    CHECK(out.find("; END_PARK_AND_WAIT") != std::string::npos);
    // G-code components (std::fixed + setprecision(2) -> always 2 decimals)
    CHECK(out.find("G92 E0") != std::string::npos);
    CHECK(out.find("G1 E-4.00") != std::string::npos);          // retract
    CHECK(out.find("G0 X15.50 Y12.30") != std::string::npos);   // travel
    CHECK(out.find("G4 P5300") != std::string::npos);           // dwell
    // Z-invariant: park_z = cur_z + hop = 1.60, return to 1.20
    CHECK(out.find("G1 Z1.60") != std::string::npos);
    CHECK(out.find("G1 Z1.20") != std::string::npos);
    // Final unretract (integer literal in source)
    CHECK(out.find("G1 E0 F") != std::string::npos);
}

// Park sequence emits its retract/unretract pair as `G1 E-<N>` / `G1 E0` around
// a `G92 E0` reset. That pair only round-trips correctly in **absolute** E mode
// (where `G1 E0` means "move E to position 0"). When the active filament uses
// `use_relative_e_distances = 1`, `G1 E0` becomes a no-op (relative move of 0)
// and the filament stays retracted, starving the next layer's first extrusion
// of pressure. Fix: wrap the block in M82/M83 when relative-E is active.
// Regression: same return-to-model invariant as emit_cooling_tower_visit. The
// pre-fix sequence dropped Z back to cur_z while still at the park point, then
// any deferred wipe/next-print code executed assuming the head was on the model.
// Result: a long diagonal traverse across the build at print Z, depositing ooze
// across upper-layer features. Fix: end the block with a travel back to
// `return_xy` BEFORE the closing Z-drop and deretract.
TEST_CASE("emit_park_sequence returns head to return_xy before dropping Z",
          "[park_and_wait][cooling]") {
    ParkSequenceInputs in{};
    in.layer_id            = 7;
    in.park_point          = Vec2f(15.5f, 12.3f);
    in.return_xy           = Vec2f(80.f, 75.f);  // distinct from park_point
    in.cur_z               = 1.2f;
    in.park_z_hop          = 0.4f;
    in.pause_needed        = 5.3f;
    in.park_retract_length = 4.0f;
    in.travel_speed        = 200.f;
    in.travel_speed_z      = 10.f;
    in.retract_speed       = 30.f;

    std::string out;
    emit_park_sequence(in, out);

    const size_t end_marker = out.find("; END_PARK_AND_WAIT");
    REQUIRE(end_marker != std::string::npos);
    const std::string body = out.substr(0, end_marker);

    // After the G4 dwell(s), the block must travel back to return_xy BEFORE the
    // Z-drop + deretract pair. Locate the return travel and verify ordering.
    const size_t pos_return  = body.find("G0 X80.00 Y75.00");
    const size_t pos_z_drop  = body.find("G1 Z1.20");
    const size_t pos_unret   = body.find("G1 E0 F");
    REQUIRE(pos_return  != std::string::npos);
    REQUIRE(pos_z_drop  != std::string::npos);
    REQUIRE(pos_unret   != std::string::npos);
    CHECK(pos_return < pos_z_drop);
    CHECK(pos_z_drop < pos_unret);
}

TEST_CASE("emit_park_sequence wraps block in M82/M83 when use_relative_e_distances=true",
          "[park_and_wait][cooling]") {
    ParkSequenceInputs in{};
    in.layer_id                 = 5;
    in.park_point               = Vec2f(50.f, 50.f);
    in.cur_z                    = 1.0f;
    in.park_z_hop               = 0.4f;
    in.pause_needed             = 5.0f;
    in.park_retract_length      = 4.0f;
    in.travel_speed             = 200.f;
    in.travel_speed_z           = 10.f;
    in.retract_speed            = 30.f;
    in.use_relative_e_distances = true;

    std::string out;
    emit_park_sequence(in, out);

    const size_t pos_header  = out.find("; PARK_AND_WAIT");
    const size_t pos_m82     = out.find("M82");
    const size_t pos_g92     = out.find("G92 E0");
    const size_t pos_retract = out.find("G1 E-4");
    const size_t pos_unret   = out.find("G1 E0 F");
    const size_t pos_m83     = out.find("M83");
    const size_t pos_end     = out.find("; END_PARK_AND_WAIT");

    REQUIRE(pos_header  != std::string::npos);
    REQUIRE(pos_m82     != std::string::npos);
    REQUIRE(pos_g92     != std::string::npos);
    REQUIRE(pos_retract != std::string::npos);
    REQUIRE(pos_unret   != std::string::npos);
    REQUIRE(pos_m83     != std::string::npos);
    REQUIRE(pos_end     != std::string::npos);

    // M82 must enter the block AFTER the header comment, but BEFORE G92/retract,
    // so the closing `G1 E0` resolves to "move E to absolute 0" and actually
    // un-retracts the filament. M83 restores the surrounding relative-E contract
    // AFTER the unretract, BEFORE the END marker.
    CHECK(pos_header  < pos_m82);
    CHECK(pos_m82     < pos_g92);
    CHECK(pos_g92     < pos_retract);
    CHECK(pos_retract < pos_unret);
    CHECK(pos_unret   < pos_m83);
    CHECK(pos_m83     < pos_end);
}

TEST_CASE("emit_park_sequence emits NO M82/M83 in absolute-E mode (default)",
          "[park_and_wait][cooling]") {
    ParkSequenceInputs in{};
    in.layer_id                 = 5;
    in.park_point               = Vec2f(50.f, 50.f);
    in.cur_z                    = 1.0f;
    in.park_z_hop               = 0.4f;
    in.pause_needed             = 5.0f;
    in.park_retract_length      = 4.0f;
    in.travel_speed             = 200.f;
    in.travel_speed_z           = 10.f;
    in.retract_speed            = 30.f;
    // Default: in.use_relative_e_distances == false (absolute E).

    std::string out;
    emit_park_sequence(in, out);

    // Absolute-E callers must not have their E mode toggled by the park block.
    CHECK(out.find("M82") == std::string::npos);
    CHECK(out.find("M83") == std::string::npos);
    // The `G1 E-4` retract and `G1 E0` round-trip are still emitted unchanged.
    CHECK(out.find("G1 E-4") != std::string::npos);
    CHECK(out.find("G1 E0 F") != std::string::npos);
}

TEST_CASE("emit_park_sequence splits long pauses into 60s G4 chunks", "[park_and_wait][cooling]") {
    ParkSequenceInputs in{};
    in.layer_id            = 1;
    in.park_point          = Vec2f(100.f, 100.f);
    in.cur_z               = 0.2f;
    in.park_z_hop          = 0.4f;
    in.pause_needed        = 130.f;  // 130s = 60+60+10
    in.park_retract_length = 4.0f;
    in.travel_speed        = 200.f;
    in.travel_speed_z      = 10.f;
    in.retract_speed       = 30.f;

    std::string out;
    emit_park_sequence(in, out);

    // Three G4 lines: P60000, P60000, P10000
    size_t pos = 0;
    int chunks = 0;
    while ((pos = out.find("G4 P", pos)) != std::string::npos) { ++chunks; pos += 4; }
    CHECK(chunks == 3);
    CHECK(out.find("G4 P60000") != std::string::npos);
    CHECK(out.find("G4 P10000") != std::string::npos);
}

// === Integration test: end-to-end park sequence emission ===

TEST_CASE("ParkAndWait emits full park sequence with Z-invariant (integration)",
          "[park_and_wait][cooling][.slice_blocked]") {
    // BLOCKED: Test::slice baseline SIGSEGV. Checked in for future.
    auto config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "min_layer_time_strategy",        "park_and_wait" },
        { "slow_down_layer_time",           "10" },
        { "park_and_wait_retract_length",   "4.0" },
        { "park_and_wait_z_hop",            "0.4" },
    });

    std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config);

    size_t blocks = 0, start = 0;
    while ((start = gcode.find("; PARK_AND_WAIT ", start)) != std::string::npos) {
        size_t end = gcode.find("; END_PARK_AND_WAIT", start);
        REQUIRE(end != std::string::npos);
        ++blocks;
        start = end;
    }
    CHECK(blocks > 0);
}

// === Edge case integration tests (Task 14) ===

TEST_CASE("First layer never gets PARK_AND_WAIT regardless of strategy",
          "[park_and_wait][cooling][.slice_blocked]") {
    // BLOCKED: Test::slice baseline SIGSEGV. Checked in for future.
    // Task 9 guards layer_id == 0 from PARK_HINT emission. This test
    // codifies the contract once the slice harness is fixed.
    auto config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "min_layer_time_strategy", "park_and_wait" },
        { "slow_down_layer_time",    "60" },  // huge threshold forces a pause
    });

    std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config);

    // Find first ;LAYER_CHANGE; everything before is layer 0.
    size_t first_lc = gcode.find(";LAYER_CHANGE");
    REQUIRE(first_lc != std::string::npos);
    std::string layer0 = gcode.substr(0, first_lc);
    CHECK(layer0.find("PARK_AND_WAIT") == std::string::npos);
    CHECK(layer0.find("PARK_HINT")     == std::string::npos);
}

TEST_CASE("Pause < 1.0s skips PARK_AND_WAIT block",
          "[park_and_wait][cooling][.slice_blocked]") {
    // BLOCKED: Test::slice baseline SIGSEGV. Checked in for future.
    // Task 13 caller guards on `pause_needed >= 1.0f` before invoking
    // emit_park_sequence. With a tiny threshold any computed pause is
    // sub-second so the block must be suppressed (PARK_HINT may still
    // appear — Task 9 doesn't gate on pause).
    auto config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "min_layer_time_strategy", "park_and_wait" },
        { "slow_down_layer_time",    "0.5" },  // tiny threshold -> tiny pause
    });

    std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config);

    CHECK(gcode.find("PARK_AND_WAIT") == std::string::npos);
}

#ifndef slic3r_CoolingBuffer_hpp_
#define slic3r_CoolingBuffer_hpp_

#include "../libslic3r.h"
#include <map>
#include <optional>
#include <string>
#include <cfloat>

namespace Slic3r {

class GCode;
class Layer;
struct PerExtruderAdjustments;

// Parsed contents of a "; PARK_HINT extruder=N x=X y=Y strategy=..." comment
// emitted by GCode::process_layer when min_layer_time_strategy=park_and_wait.
// Exposed in the header (rather than .cpp) so it can be unit-tested without
// constructing a full CoolingBuffer + GCode instance.
struct ParkHintData {
    unsigned int extruder_id;
    Vec2f        point;
    // True iff the strategy token equals "park_and_wait". Unknown strategies
    // are returned with both flags false so callers can degrade gracefully
    // (typically by falling back to Slowdown).
    bool         is_park_and_wait = false;
    // True iff the strategy token equals "cooling_tower". Mutually exclusive
    // with is_park_and_wait; the parser will only ever set one of them.
    bool         is_cooling_tower = false;
};
std::optional<ParkHintData> parse_park_hint_line(const std::string &line);

// Compute the pause required for a ParkAndWait-strategy extruder to reach
// the minimum layer time threshold, after subtracting fixed travel/retract/
// z-hop overhead from going to and returning from the park point.
//
// All speeds are in mm/s, distances in mm, times in s. The function returns
// 0 when the layer is already above threshold, when no park_point was
// computed (geometry-degenerate layer), or when overhead alone exceeds the
// gap. Exposed in the header so it can be unit-tested without constructing
// a full CoolingBuffer + GCode instance.
float compute_park_pause_needed(
    float threshold,
    float layer_time,
    std::optional<Vec2f> park_point,
    Vec2f current_xy,
    float park_retract_length,
    float park_z_hop,
    float travel_speed,
    float travel_speed_z,
    float retract_speed);

// Inputs needed to format the per-layer park-and-wait gcode sequence.
struct ParkSequenceInputs {
    int     layer_id;
    Vec2f   park_point;
    float   cur_z;                  // mm
    float   park_z_hop;             // mm
    float   pause_needed;           // seconds
    float   park_retract_length;    // mm
    float   travel_speed;           // mm/s (converted to mm/min internally)
    float   travel_speed_z;         // mm/s (0 -> falls back to travel_speed)
    float   retract_speed;          // mm/s
    // Surrounding G-code's E-coordinate mode. The block always uses
    // `G92 E0` + `G1 E-N` ... `G1 E0` to retract and un-retract symmetrically,
    // which only round-trips correctly in ABSOLUTE E mode. When the active
    // filament profile sets `use_relative_e_distances = 1`, the block is
    // wrapped in M82/M83 so the un-retract resolves to "move E to absolute 0"
    // (otherwise `G1 E0` becomes a no-op and filament stays retracted across
    // the whole rest of the print).
    bool    use_relative_e_distances = false;
};

// Format the gcode block (retract, Z-hop, travel, G4 dwell(s), Z-return,
// unretract) and append to `out`. The pause is split into <=60s G4 chunks
// for firmware compatibility. Z-invariant: the second G1 Z restores the
// original cur_z BEFORE the unretract, so any ooze drips on infill, not
// on the next layer.
void emit_park_sequence(const ParkSequenceInputs &in, std::string &out);

// Inputs needed to format a per-layer cooling-tower visit (the
// `cooling_tower` min-layer-time strategy). Unlike park_and_wait, the
// nozzle does NOT dwell with G4 inside the visit — it extrudes one
// continuous arc of a spiral-vase tower wall for up to `pause_needed`
// seconds, keeping the hotend under pressure for the whole wait so the
// model itself never gets oozed on.
//
// V2 contract (tower height bounded by model rate):
//  - `prev_tower_top_z` is the Z where the previous visit ended (or the
//    brim's z after the first ever visit); this visit's spiral starts at
//    EXACTLY `prev_tower_top_z` (no step-up gap, so the tower's vertical
//    growth across visits is bounded by the caller-supplied target rise
//    rather than doubling it).
//  - `tower_layer_height` is the TARGET RISE for this single visit, in mm.
//    Typically the caller passes `cur_z - prev_tower_top_z` so the tower
//    tops out at the model's current Z. Pass 0 to lay a flat ring at
//    `prev_tower_top_z` (used when the tower is already at model height).
//  - The spiral is CAPPED at one full loop (SEGMENTS_PER_LOOP segments),
//    so total rise per visit ≤ `tower_layer_height`. Caller controls
//    visit timing by picking `tower_speed` via compute_tower_visit_speed
//    and absorbs any leftover dwell via compute_tower_residual_dwell +
//    G4 after the visit.
struct CoolingTowerVisitInputs {
    int     layer_id;
    Vec2f   tower_xy;
    float   prev_tower_top_z;       // mm; 0 at print start
    float   cur_z;                  // mm; model's current Z (return point)
    float   z_hop;                  // mm
    float   pause_needed;           // seconds
    float   retract_length;         // mm
    float   diameter;               // mm
    float   tower_layer_height;     // mm; target rise for THIS visit
    float   tower_line_width;       // mm
    float   filament_diameter;      // mm
    float   tower_speed;            // mm/s (XY extrusion)
    float   travel_speed;           // mm/s
    float   travel_speed_z;         // mm/s (0 -> falls back to travel_speed)
    float   retract_speed;          // mm/s
    bool    use_relative_e_distances = false;
};

// Format and append the cooling-tower visit gcode. Returns the new
// tower_top_z (where the spiral ended), which the caller stores for the
// next layer's visit. Spiral is approximated as 20 polyline segments per
// loop (18° step) and CAPPED at one full loop. Wrapped in M82/M83 when
// use_relative_e_distances=true, same contract as emit_park_sequence.
float emit_cooling_tower_visit(const CoolingTowerVisitInputs &in, std::string &out);

// Pick the spiral extrusion speed so a single full loop of `circumference`
// mm takes exactly `pause_needed` seconds. Clamped to [min_speed, max_speed]:
//  - desired > max_speed (very short pause): returns max_speed; the loop
//    finishes faster than pause_needed, no residual G4 needed.
//  - desired < min_speed (very long pause): returns min_speed; the loop
//    finishes faster than pause_needed; caller pairs with
//    compute_tower_residual_dwell to emit a G4 for the leftover.
// All units mm and s.
float compute_tower_visit_speed(float pause_needed,
                                float circumference,
                                float min_speed,
                                float max_speed);

// Return the residual dwell (in seconds) that must still be absorbed via
// a G4 after a one-loop visit at `actual_speed` over `circumference` mm.
// Returns 0 when one loop already covers `pause_needed` — typical when
// `actual_speed` was not clamped. Positive only when speed was clamped to
// the floor by compute_tower_visit_speed.
float compute_tower_residual_dwell(float pause_needed,
                                   float actual_speed,
                                   float circumference);

// Inputs needed to format the first-layer brim under a cooling tower.
// Emitted once at the start of the first object layer (BEFORE the first
// visit), so the tower has bed adhesion before any spiral runs above it.
struct CoolingTowerBrimInputs {
    Vec2f   tower_xy;
    int     loops;                  // number of concentric brim rings; 0 disables
    float   first_layer_z;          // mm; matches the model's first layer height
    float   tower_diameter;         // mm; outer Ø of the spiral above
    float   line_width;             // mm; first-layer line width
    float   filament_diameter;      // mm
    float   speed;                  // mm/s; XY extrusion speed
    float   travel_speed;           // mm/s
    float   travel_speed_z;         // mm/s
    float   retract_length;         // mm
    float   retract_speed;          // mm/s
    bool    use_relative_e_distances = false;
};
void emit_cooling_tower_brim(const CoolingTowerBrimInputs &in, std::string &out);

// Pick a free XY for the cooling tower next to the model. Tries — in order —
// the right, left, back, and front edge of the model bbox (margin from bbox +
// half tower diameter), and falls back to the first bed corner that fits if
// none of those land inside the bed. Always returns a point inside `bed`.
Vec2f compute_cooling_tower_xy(
    const BoundingBoxf &model_bbox,
    const BoundingBoxf &bed,
    float diameter,
    float margin);

// A standalone G-code filter, to control cooling of the print.
// The G-code is processed per layer. Once a layer is collected, fan start / stop commands are edited
// and the print is modified to stretch over a minimum layer time.
//
// The simple it sounds, the actual implementation is significantly more complex.
// Namely, for a multi-extruder print, each material may require a different cooling logic.
// For example, some materials may not like to print too slowly, while with some materials 
// we may slow down significantly.
//
class CoolingBuffer {
public:
    CoolingBuffer(GCode &gcodegen);
    void        reset(const Vec3d &position);
    void        set_current_extruder(unsigned int extruder_id) { m_current_extruder = extruder_id; }
    std::string process_layer(std::string &&gcode, size_t layer_id, bool flush);

private:
	CoolingBuffer& operator=(const CoolingBuffer&) = delete;
    std::vector<PerExtruderAdjustments> parse_layer_gcode(const std::string &gcode, std::vector<float> &current_pos) const;
    float       calculate_layer_slowdown(std::vector<PerExtruderAdjustments> &per_extruder_adjustments);
    // Apply slow down over G-code lines stored in per_extruder_adjustments, enable fan if needed.
    // Returns the adjusted G-code.
    std::string apply_layer_cooldown(const std::string &gcode, size_t layer_id, float layer_time, std::vector<PerExtruderAdjustments> &per_extruder_adjustments);

    // G-code snippet cached for the support layers preceding an object layer.
    std::string                 m_gcode;
    // Internal data.
    // BBS: X,Y,Z,E,F,I,J
    std::vector<char>           m_axis;
    std::vector<float>          m_current_pos;
    // Current known fan speed or -1 if not known yet.
    int                         m_fan_speed;
    int                         m_additional_fan_speed;
    // Cached from GCodeWriter.
    // Printing extruder IDs, zero based.
    std::vector<unsigned int>   m_extruder_ids;
    // Highest of m_extruder_ids plus 1.
    unsigned int                m_num_extruders { 0 };
    const std::string           m_toolchange_prefix;
    // Referencs GCode::m_config, which is FullPrintConfig. While the PrintObjectConfig slice of FullPrintConfig is being modified,
    // the PrintConfig slice of FullPrintConfig is constant, thus no thread synchronization is required.
    const PrintConfig          &m_config;
    unsigned int                m_current_extruder;
    //BBS: current fan speed
    int                         m_current_fan_speed;

    // Cooling-tower state, shared across all extruders for V1 (the strategy
    // is intended for single-tool prints; IDEX would need per-tool towers and
    // is left for a follow-up). m_cooling_tower_top_z is the Z where the LAST
    // visit's spiral ended — the next visit starts one tower_layer_height
    // above this. m_cooling_tower_brim_emitted flips true after the brim is
    // laid (once per print).
    float                       m_cooling_tower_top_z { 0.f };
    bool                        m_cooling_tower_brim_emitted { false };
};

}

#endif

#include "FanPaint.hpp"

#include "ClipperUtils.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
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

// Parameters in (0, 1) at which segment a->b crosses the edges of the closed `polygon`.
void append_crossings(const Vec2d &a, const Vec2d &b, const Polygon &polygon, std::vector<double> &params)
{
    const Vec2d   r      = b - a;
    const Points &points = polygon.points;
    for (size_t i = 0; i < points.size(); ++ i) {
        const Vec2d  c           = points[i].cast<double>();
        const Vec2d  s           = points[(i + 1) % points.size()].cast<double>() - c;
        const double denominator = cross2(r, s);
        if (denominator == 0.)
            continue; // parallel: the runs on either side are told apart by their midpoints
        const Vec2d  ca = c - a;
        const double t  = cross2(ca, s) / denominator;
        const double u  = cross2(ca, r) / denominator;
        if (t > 0. && t < 1. && u >= 0. && u <= 1.)
            params.push_back(t);
    }
}

// Highest speed of the zones holding `point`, -1 when none does.
int painted_speed_at(const Point &point, const std::vector<const FanPaintZone*> &zones)
{
    int speed = -1;
    for (const FanPaintZone *zone : zones) {
        if (int(zone->speed_pct) <= speed || ! zone->bbox.contains(point))
            continue;
        if (std::any_of(zone->area.begin(), zone->area.end(), [&point](const ExPolygon &expolygon) { return expolygon.contains(point); }))
            speed = zone->speed_pct;
    }
    return speed;
}

Point round_point(const Vec2d &point)
{
    return Point(coord_t(std::llround(point.x())), coord_t(std::llround(point.y())));
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

FanPaintZones build_fan_paint_layer_zones(const std::vector<std::pair<uint8_t, Polygons>>   &projections,
                                          const std::vector<std::pair<uint8_t, ExPolygons>> &bands,
                                          coord_t                                            expansion)
{
    FanPaintZones zones;
    // A projected triangle thinner than this is a painted vertical face seen edge-on: it has no area to union or
    // offset. (SCALING_FACTOR is a runtime value in this fork, so this cannot be a constexpr.)
    const double sliver_area = double(SCALED_EPSILON);
    const auto add_zone = [&zones](const FanPaintValue &decoded, ExPolygons &&area) {
        if (area.empty())
            return;
        const BoundingBox bbox = get_extents(area);
        zones.push_back(FanPaintZone{ std::move(area), bbox, decoded.speed_pct, decoded.line_mask });
    };
    for (const auto &[value, polygons] : projections) {
        const std::optional<FanPaintValue> decoded = decode_fan_paint_value(value);
        if (! decoded)
            continue;
        Polygons  solid;
        Polylines slivers;
        for (const Polygon &polygon : polygons) {
            if (std::abs(polygon.area()) > sliver_area)
                solid.push_back(polygon);
            else if (polygon.points.size() >= 2)
                slivers.emplace_back(polygon.points);
        }
        ExPolygons area = offset_ex(union_ex(solid), float(expansion));
        if (! slivers.empty())
            area = union_ex(area, offset(slivers, float(expansion), ClipperLib::jtSquare, 3., ClipperLib::etOpenSquare));
        add_zone(*decoded, std::move(area));
    }
    for (const auto &[value, outline] : bands)
        if (const std::optional<FanPaintValue> decoded = decode_fan_paint_value(value))
            add_zone(*decoded, ExPolygons(outline));
    return zones;
}

std::vector<FanPaintSpan> split_by_fan_paint_zones(const Points &polyline, uint8_t line_bit, const FanPaintZones &zones)
{
    std::vector<FanPaintSpan> spans;
    if (line_bit == 0 || polyline.size() < 2)
        return spans;
    const BoundingBox                path_bbox(polyline);
    std::vector<const FanPaintZone*> candidates;
    for (const FanPaintZone &zone : zones)
        if ((zone.line_mask & line_bit) != 0 && zone.bbox.overlap(path_bbox))
            candidates.push_back(&zone);
    if (candidates.empty())
        return spans;

    // A run that collapsed to one point adds nothing; a run of the previous run's speed continues it.
    const auto close_run = [&spans](FanPaintSpan &&run) {
        if (run.points.size() < 2)
            return;
        if (! spans.empty() && spans.back().speed_pct == run.speed_pct) {
            spans.back().points.insert(spans.back().points.end(), run.points.begin() + 1, run.points.end());
            return;
        }
        spans.push_back(std::move(run));
    };

    FanPaintSpan        run { { polyline.front() }, painted_speed_at(polyline.front(), candidates) };
    std::vector<double> params;
    for (size_t i = 1; i < polyline.size(); ++ i) {
        const Vec2d a = polyline[i - 1].cast<double>();
        const Vec2d b = polyline[i].cast<double>();
        params.assign({ 0., 1. });
        for (const FanPaintZone *zone : candidates)
            for (const ExPolygon &expolygon : zone->area) {
                append_crossings(a, b, expolygon.contour, params);
                for (const Polygon &hole : expolygon.holes)
                    append_crossings(a, b, hole, params);
            }
        std::sort(params.begin(), params.end());
        for (size_t k = 1; k < params.size(); ++ k) {
            if (params[k] <= params[k - 1])
                continue;
            const int   speed = painted_speed_at(round_point(a + (b - a) * (0.5 * (params[k - 1] + params[k]))), candidates);
            const Point end   = k + 1 == params.size() ? polyline[i] : round_point(a + (b - a) * params[k]);
            if (speed != run.speed_pct) {
                const Point start = run.points.back();
                close_run(std::move(run));
                run = FanPaintSpan{ { start }, speed };
            }
            if (end != run.points.back())
                run.points.push_back(end);
        }
    }
    close_run(std::move(run));

    if (std::none_of(spans.begin(), spans.end(), [](const FanPaintSpan &span) { return span.speed_pct >= 0; }))
        spans.clear();
    return spans;
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

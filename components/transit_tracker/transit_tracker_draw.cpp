// ============================================================
// transit_tracker_draw.cpp  — drop this file into your fork at
//   components/transit_tracker/transit_tracker_draw.cpp
//
// This replaces / supplements the draw_schedule() rendering logic.
// If the upstream draw_schedule() is inline in transit_tracker.cpp,
// move it here and #include this file, or paste the body in directly.
// ============================================================

#include "transit_tracker.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"       // millis()
#include "esphome/components/display/display_buffer.h"

namespace esphome {
namespace transit_tracker {

// ── Helpers ────────────────────────────────────────────────

// Returns true if routeId is an express variant (ends in "X": FX, 6X, 7X).
static bool is_express(const std::string &route_id) {
  return !route_id.empty() && route_id.back() == 'X';
}

// Returns the display letter for a badge: strips trailing X (FX→F, 6X→6).
static std::string badge_letter(const std::string &route_id) {
  if (is_express(route_id) && route_id.size() > 1) {
    return route_id.substr(0, route_id.size() - 1);
  }
  return route_id;
}

// Draw the wifi signal icon at pixel (cx, cy).
// Matches the stacked-arcs style from the original firmware simulator.
static void draw_wifi_icon(display::DisplayBuffer &it, int cx, int cy, Color col) {
  // bottom dot
  it.draw_pixel_at(cx,   cy,   col);
  it.draw_pixel_at(cx+1, cy,   col);
  // small arc
  it.draw_pixel_at(cx-1, cy-2, col);
  it.draw_pixel_at(cx,   cy-2, col);
  it.draw_pixel_at(cx+1, cy-2, col);
  it.draw_pixel_at(cx+2, cy-2, col);
  // medium arc
  it.draw_pixel_at(cx-2, cy-4, col);
  it.draw_pixel_at(cx-1, cy-4, col);
  it.draw_pixel_at(cx+2, cy-4, col);
  it.draw_pixel_at(cx+3, cy-4, col);
  it.draw_pixel_at(cx-3, cy-3, col);
  it.draw_pixel_at(cx+4, cy-3, col);
  // large arc
  it.draw_pixel_at(cx-3, cy-6, col);
  it.draw_pixel_at(cx-2, cy-6, col);
  it.draw_pixel_at(cx+3, cy-6, col);
  it.draw_pixel_at(cx+4, cy-6, col);
  it.draw_pixel_at(cx-4, cy-5, col);
  it.draw_pixel_at(cx+5, cy-5, col);
}

// Draw a filled circle of radius r centered at (cx, cy).
static void fill_circle(display::DisplayBuffer &it, int cx, int cy, int r, Color col) {
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++)
      if (dx*dx + dy*dy <= r*r)
        it.draw_pixel_at(cx+dx, cy+dy, col);
}

// Draw a filled diamond of radius r centered at (cx, cy).
static void fill_diamond(display::DisplayBuffer &it, int cx, int cy, int r, Color col) {
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++)
      if (abs(dx) + abs(dy) <= r)
        it.draw_pixel_at(cx+dx, cy+dy, col);
}

// Draw a route badge (circle or diamond) with letter inside.
// RouteStyle carries color, textColor, and shape from route_styles_config JSON.
static void draw_badge(display::DisplayBuffer &it,
                       const Font *font,
                       int cx, int cy,
                       const std::string &route_id,
                       const RouteStyle &style) {
  const int R = 5;  // badge radius in pixels — fits on 32px-tall display
  std::string letter = badge_letter(route_id);

  Color bg  = style.color;
  Color fg  = style.text_color;

  if (is_express(route_id)) {
    fill_diamond(it, cx, cy, R, bg);
  } else {
    fill_circle(it, cx, cy, R, bg);
  }

  // Center letter inside badge.
  // it.print() with CENTER_BOTH alignment handles single-char centering.
  it.printf(cx, cy, font, fg, display::TextAlign::CENTER, "%s", letter.c_str());
}

// Format minutes-to-arrival as display string.
// < 1 min  → "NOW"
// >= 60    → "1:02" style (keeps it short on the narrow display)
// otherwise → plain integer minutes
static std::string format_time(int32_t seconds_until) {
  if (seconds_until < 60) return "NOW";
  int32_t mins = seconds_until / 60;
  if (mins >= 60) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d:%02d", mins/60, mins%60);
    return std::string(buf);
  }
  return std::to_string(mins);
}

// ── Main draw_schedule() ───────────────────────────────────

void TransitTracker::draw_schedule() {
  if (this->display_ == nullptr || this->font_ == nullptr) return;
  display::DisplayBuffer &it = *this->display_;

  const int DISPLAY_W = it.get_width();   // 128
  const int DISPLAY_H = it.get_height();  // 32
  const int ROW_H     = 10;              // pixels per row (3 rows × 10 = 30, centered in 32)
  const int BADGE_R   = 5;              // badge radius
  const int START_Y   = (DISPLAY_H - 3 * ROW_H) / 2 + 1;  // vertically center 3 rows

  // ── Page alternation ──────────────────────────────────
  // Only alternate if we have >3 trips available.
  const auto &trips = this->trips_;  // std::vector<Trip> — adjust name to match upstream
  const size_t trip_count = trips.size();

  uint32_t now_ms = millis();
  if (trip_count > 3 &&
      (now_ms - this->alt_page_last_switch_) >= ALT_PAGE_INTERVAL_MS) {
    this->alt_page_b_ = !this->alt_page_b_;
    this->alt_page_last_switch_ = now_ms;
  }
  // If we have ≤3 trips, always show page A (no alternation needed).
  if (trip_count <= 3) {
    this->alt_page_b_ = false;
  }

  // ── Build the 3 row indices to display ────────────────
  // Row 0 (seq "1."): always trips[0]
  // Row 1 (seq "2." or "4."): page A → trips[1], page B → trips[3]
  // Row 2 (seq "3." or "5."): page A → trips[2], page B → trips[4]
  struct DisplayRow {
    size_t trip_index;
    int    seq;        // ordinal number shown to rider (1-5)
  };

  DisplayRow rows[3] = {
    { 0, 1 },
    { this->alt_page_b_ ? 3u : 1u,  this->alt_page_b_ ? 4 : 2 },
    { this->alt_page_b_ ? 4u : 2u,  this->alt_page_b_ ? 5 : 3 },
  };

  // ── Draw each row ─────────────────────────────────────
  for (int r = 0; r < 3; r++) {
    size_t idx = rows[r].trip_index;
    if (idx >= trip_count) continue;          // no trip available for this slot

    const Trip &trip = trips[idx];            // Trip struct from upstream
    int y    = START_Y + r * ROW_H;
    int midY = y + (ROW_H / 2) - 1;          // vertical center of row

    // 1. Ordinal ("1." "2." … "5.") — white, left edge
    char ord_buf[4];
    snprintf(ord_buf, sizeof(ord_buf), "%d.", rows[r].seq);
    it.print(1, y + 1, this->font_, Color(0xFF, 0xFF, 0xFF), ord_buf);

    int ord_w = strlen(ord_buf) * 5;   // approx width using 5px-wide pixel font
                                        // replace with actual font->get_string_width() if available

    // 2. Route badge (circle or diamond)
    int badge_cx = 1 + ord_w + 1 + BADGE_R + 1;  // 1px left pad, ordinal, 1px gap, radius, 1px gap
    RouteStyle style = this->get_route_style(trip.route_id);  // looks up route_styles_config
    draw_badge(it, this->font_, badge_cx, midY, trip.route_id, style);

    int after_badge = badge_cx + BADGE_R + 2;

    // 3. Headsign / destination — white, truncated to available width
    //    Reserve ~20px on right for wifi icon + time string.
    //    The upstream component may already handle truncation — check before overriding.
    std::string dest = trip.headsign;
    // Apply any abbreviations from abbreviations_config (upstream handles this)
    it.print(after_badge, y + 1, this->font_, Color(0xFF, 0xFF, 0xFF),
             display::TextAlign::TOP_LEFT, dest.c_str());

    // 4. Wifi icon — blue if real-time, dim gray if scheduled
    Color wifi_col = trip.is_realtime ? WIFI_COLOR_REALTIME : WIFI_COLOR_SCHEDULED;
    std::string time_str = format_time(trip.arrival_time - (int32_t)(esphome::millis() / 1000));
    int time_w   = time_str.size() * 5;  // approx; replace with font width if available
    int wifi_x   = DISPLAY_W - time_w - 8;
    draw_wifi_icon(it, wifi_x, midY + 2, wifi_col);

    // 5. Arrival time — white, right-aligned
    it.printf(DISPLAY_W - time_w, y + 1, this->font_,
              Color(0xFF, 0xFF, 0xFF), display::TextAlign::TOP_LEFT,
              "%s", time_str.c_str());
  }
}

}  // namespace transit_tracker
}  // namespace esphome

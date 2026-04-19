#include "transit_tracker.h"
#include "string_utils.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/watchdog/watchdog.h"
#include "esphome/components/network/util.h"

namespace esphome {
namespace transit_tracker {

static const char *TAG = "transit_tracker.component";

void TransitTracker::setup() {
  this->ws_client_.onMessage([this](websockets::WebsocketsMessage message) {
    this->on_ws_message_(message);
  });

  this->ws_client_.onEvent([this](websockets::WebsocketsEvent event, String data) {
    this->on_ws_event_(event, data);
  });

  this->connect_ws_();

  this->set_interval("check_stale_trips", 10000, [this]() {
    if (this->ws_client_.available() && !this->schedule_state_.trips.empty()) {
      bool has_stale_trips = false;

      this->schedule_state_.mutex.lock();

      auto now = this->rtc_->now();
      if (now.is_valid()) {
        for (auto &trip : this->schedule_state_.trips) {
          if (now.timestamp - trip.departure_time > 60) {
            has_stale_trips = true;
            break;
          }
        }
      }

      this->schedule_state_.mutex.unlock();

      if (has_stale_trips) {
        ESP_LOGD(TAG, "Stale trips detected, reconnecting");
        ESP_LOGD(TAG, "  Current RTC time: %d", now.timestamp);
        ESP_LOGD(TAG, "  Last heartbeat: %d", this->last_heartbeat_);
        this->reconnect();
      }
    }
  });
}

void TransitTracker::loop() {
  this->ws_client_.poll();

  if (this->last_heartbeat_ != 0 && millis() - this->last_heartbeat_ > 60000) {
    ESP_LOGW(TAG, "Heartbeat timeout, reconnecting");
    this->reconnect();
    return;
  }
}

void TransitTracker::dump_config() {
  ESP_LOGCONFIG(TAG, "Transit Tracker:");
  ESP_LOGCONFIG(TAG, "  Base URL: %s", this->base_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Schedule: %s", this->schedule_string_.c_str());
  ESP_LOGCONFIG(TAG, "  Limit: %d", this->limit_);
  ESP_LOGCONFIG(TAG, "  List mode: %s", this->list_mode_.c_str());
  ESP_LOGCONFIG(TAG, "  Display departure times: %s", this->display_departure_times_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Scroll Headsigns: %s", this->scroll_headsigns_ ? "true" : "false");
}

void TransitTracker::reconnect() {
  this->close();
  this->connect_ws_();
}

void TransitTracker::close(bool fully) {
  if (fully) {
    this->fully_closed_ = true;
  }

  this->ws_client_.close();
}

void TransitTracker::on_shutdown() {
  this->cancel_interval("check_stale_trips");
  this->close(true);
}

void TransitTracker::on_ws_message_(websockets::WebsocketsMessage message) {
  ESP_LOGV(TAG, "Received message: %s", message.rawData().c_str());

  bool valid = json::parse_json(message.rawData(), [this](JsonObject root) -> bool {
    if (root["event"].as<std::string>() == "heartbeat") {
      ESP_LOGD(TAG, "Received heartbeat");
      this->last_heartbeat_ = millis();
      return true;
    }

    if (root["event"].as<std::string>() != "schedule") {
      return true;
    }

    ESP_LOGD(TAG, "Received schedule update");

    this->schedule_state_.mutex.lock();

    this->schedule_state_.trips.clear();

    auto data = root["data"].as<JsonObject>();

    for (auto trip : data["trips"].as<JsonArray>()) {
      std::string headsign = trip["headsign"].as<std::string>();
      for (const auto &abbr : this->abbreviations_) {
        size_t pos = headsign.find(abbr.first);
        if (pos != std::string::npos) {
          ESP_LOGV(TAG, "Applying abbreviation '%s' -> '%s' in headsign", abbr.first.c_str(), abbr.second.c_str());
          headsign.replace(pos, abbr.first.length(), abbr.second);
        }
      }

      auto route_id = trip["routeId"].as<std::string>();
      auto route_style = this->route_styles_.find(route_id);

      Color route_color = this->default_route_color_;
      std::string route_name = trip["routeName"].as<std::string>();

      if (route_style != this->route_styles_.end()) {
        route_color = route_style->second.color;
        route_name = route_style->second.name;
      } else if (!trip["routeColor"].isNull()) {
        route_color = Color(std::stoul(trip["routeColor"].as<std::string>(), nullptr, 16));
      }

      this->schedule_state_.trips.push_back({
        .route_id = route_id,
        .route_name = route_name,
        .route_color = route_color,
        .headsign = headsign,
        .arrival_time = trip["arrivalTime"].as<time_t>(),
        .departure_time = trip["departureTime"].as<time_t>(),
        .is_realtime = trip["isRealtime"].as<bool>(),
      });
    }

    this->schedule_state_.mutex.unlock();

    return true;
  });

  if (!valid) {
    this->status_set_error(LOG_STR("Failed to parse schedule data"));
    return;
  }
}

void TransitTracker::on_ws_event_(websockets::WebsocketsEvent event, String data) {
  if (event == websockets::WebsocketsEvent::ConnectionOpened) {
    ESP_LOGD(TAG, "WebSocket connection opened");

    auto message = json::build_json([this](JsonObject root) {
      root["event"] = "schedule:subscribe";

      auto data = root["data"].to<JsonObject>();

      if (!this->feed_code_.empty()) {
        data["feedCode"] = this->feed_code_;
      }

      data["routeStopPairs"] = this->schedule_string_;
      data["limit"] = this->limit_;
      data["sortByDeparture"] = this->display_departure_times_;
      data["listMode"] = this->list_mode_;
    });

    ESP_LOGV(TAG, "Sending message: %s", message.c_str());
    this->ws_client_.send(message.c_str());
  } else if (event == websockets::WebsocketsEvent::ConnectionClosed) {
    ESP_LOGD(TAG, "WebSocket connection closed");
    if (!this->fully_closed_ && this->connection_attempts_ == 0) {
      this->defer([this]() {
        this->connect_ws_();
      });
    }
  } else if (event == websockets::WebsocketsEvent::GotPing) {
    ESP_LOGV(TAG, "Received ping");
  } else if (event == websockets::WebsocketsEvent::GotPong) {
    ESP_LOGV(TAG, "Received pong");
  }
}

void TransitTracker::connect_ws_() {
  if (this->base_url_.empty()) {
    ESP_LOGW(TAG, "No base URL set, not connecting");
    return;
  }

  if (this->fully_closed_) {
    ESP_LOGW(TAG, "Connection fully closed, not reconnecting");
    return;
  }

  if (this->ws_client_.available(true)) {
    ESP_LOGV(TAG, "Not reconnecting, already connected");
    return;
  }

  watchdog::WatchdogManager wdm(20000);

  this->last_heartbeat_ = 0;

  ESP_LOGD(TAG, "Connecting to WebSocket server (attempt %d): %s", this->connection_attempts_, this->base_url_.c_str());

  bool connection_success = false;
  if (esphome::network::is_connected()) {
    connection_success = this->ws_client_.connect(this->base_url_.c_str());
  } else {
    ESP_LOGW(TAG, "Not connected to network; skipping connection attempt");
  }

  if (!connection_success) {
    this->connection_attempts_++;

    if (this->connection_attempts_ >= 3) {
      this->status_set_error(LOG_STR("Failed to connect to WebSocket server"));
    }

    if (this->connection_attempts_ >= 15) {
      ESP_LOGE(TAG, "Could not connect to WebSocket server within 15 attempts.");
      ESP_LOGE(TAG, "It's likely that the network is not truly connected; rebooting the device to try to recover.");
      App.reboot();
    }

    auto timeout = std::min(15000, this->connection_attempts_ * 5000);
    ESP_LOGW(TAG, "Failed to connect, retrying in %ds", timeout / 1000);

    this->set_timeout("reconnect", timeout, [this]() {
      this->connect_ws_();
    });
  } else {
    this->has_ever_connected_ = true;
    this->connection_attempts_ = 0;
    this->status_clear_error();
  }
}

void TransitTracker::set_abbreviations_from_text(const std::string &text) {
  this->abbreviations_.clear();
  for (const auto &line : split(text, '\n')) {
    auto parts = split(line, ';');

    if (parts.size() == 1) {
      // If only one part is provided, treat it as a removal (replace with empty string)
      this->add_abbreviation(parts[0], "");
      continue;
    }

    if (parts.size() != 2) {
      ESP_LOGW(TAG, "Invalid abbreviation line: %s", line.c_str());
      continue;
    }

    this->add_abbreviation(parts[0], parts[1]);
  }
}

void TransitTracker::set_route_styles_from_text(const std::string &text) {
  this->route_styles_.clear();
  for (const auto &line : split(text, '\n')) {
    auto parts = split(line, ';');
    if (parts.size() != 3) {
      ESP_LOGW(TAG, "Invalid route style line: %s", line.c_str());
      continue;
    }
    uint32_t color = std::stoul(parts[2], nullptr, 16);
    this->add_route_style(parts[0], parts[1], Color(color));
  }
}

void TransitTracker::draw_text_centered_(const char *text, Color color) {
  int display_center_x = this->display_->get_width() / 2;
  int display_center_y = this->display_->get_height() / 2;
  this->display_->print(display_center_x, display_center_y, this->font_, color, display::TextAlign::CENTER, text);
}

void TransitTracker::set_realtime_color(const Color &color) {
  this->realtime_color_ = color;
  this->realtime_color_dark_ = Color(
    (color.r * 0.5),
    (color.g * 0.5),
    (color.b * 0.5)
  );
}

const uint8_t realtime_icon[6][6] = {
  {0, 0, 0, 3, 3, 3},
  {0, 0, 3, 0, 0, 0},
  {0, 3, 0, 0, 2, 2},
  {3, 0, 0, 2, 0, 0},
  {3, 0, 2, 0, 0, 1},
  {3, 0, 2, 0, 1, 1}
};

void HOT TransitTracker::draw_realtime_icon_(int bottom_right_x, int bottom_right_y, unsigned long uptime) {
  const int num_frames = 6;
  const int idle_frame_duration = 3000;
  const int anim_frame_duration = 200;
  const int cycle_duration = idle_frame_duration + (num_frames - 1) * anim_frame_duration;

  unsigned long cycle_time = uptime % cycle_duration;

  int frame;
  if (cycle_time < idle_frame_duration) {
    frame = 0;
  } else {
    frame = 1 + (cycle_time - idle_frame_duration) / anim_frame_duration;
  }

  auto is_segment_lit = [frame](uint8_t segment) {
    switch (segment) {
      case 1: return frame >= 1 && frame <= 3;
      case 2: return frame >= 2 && frame <= 4;
      case 3: return frame >= 3 && frame <= 5;
      default: return false;
    }
  };

  for (uint8_t i = 0; i < 6; ++i) {
    for (uint8_t j = 0; j < 6; ++j) {
      uint8_t segment_number = realtime_icon[i][j];
      if (segment_number == 0) {
        continue;
      }

      Color icon_color = is_segment_lit(segment_number) ? this->realtime_color_ : this->realtime_color_dark_;
      this->display_->draw_pixel_at(bottom_right_x - (5 - j), bottom_right_y - (5 - i), icon_color);
    }
  }
}

void TransitTracker::draw_trip(
    const Trip &trip, int y_offset, int font_height, unsigned long uptime, uint rtc_now,
    bool no_draw, int *headsign_overflow_out, int scroll_cycle_duration
) {
    if (!no_draw) {
      this->display_->print(0, y_offset, this->font_, trip.route_color, display::TextAlign::TOP_LEFT, trip.route_name.c_str());
    }

    int route_width, _;
    this->font_->measure(trip.route_name.c_str(), &route_width, &_, &_, &_);

    auto time_display = this->localization_.fmt_duration_from_now(
      this->display_departure_times_ ? trip.departure_time : trip.arrival_time,
      rtc_now
    );

    int time_width;
    this->font_->measure(time_display.c_str(), &time_width, &_, &_, &_);

    int headsign_clipping_start = route_width + 3;
    int headsign_clipping_end = this->display_->get_width() - time_width - 2;

    if (!no_draw) {
      Color time_color = trip.is_realtime ? this->realtime_color_ : Color(0xa7a7a7);
      this->display_->print(this->display_->get_width() + 1, y_offset, this->font_, time_color, display::TextAlign::TOP_RIGHT, time_display.c_str());
    }

    if (trip.is_realtime) {
      headsign_clipping_end -= 8;

      if(!no_draw) {
        int icon_bottom_right_x = this->display_->get_width() - time_width - 2;
        int icon_bottom_right_y = y_offset + font_height - 6;

        this->draw_realtime_icon_(icon_bottom_right_x, icon_bottom_right_y, uptime);
      }
    }

    int headsign_max_width = headsign_clipping_end - headsign_clipping_start;

    int headsign_actual_width;
    this->font_->measure(trip.headsign.c_str(), &headsign_actual_width, &_, &_, &_);

    int headsign_overflow = headsign_actual_width - headsign_max_width;
    if (headsign_overflow_out) {
      *headsign_overflow_out = headsign_overflow;
    }

    if (no_draw) {
      return;
    }

    int scroll_offset = 0;
    if (headsign_overflow > 0 && scroll_cycle_duration > 0) {
      /// Note: The scroll may jump if headsign_clipping_end changes (e.g. due to the width of the arrival time changing).
      /// This is probably not a big deal, since the display makes sudden changes anyway (e.g. when routes are updated)
      /// and this happens relatively infrequently.

      int scroll_time = headsign_overflow * 1000 / scroll_speed;
      int scroll_cycle_time = uptime % scroll_cycle_duration;

      // Scroll idle (left side - default)
      if(scroll_cycle_time < idle_time_left) {
        // scroll_offset = 0; do nothing
      } else if (scroll_cycle_time < idle_time_left + scroll_time) {
        // Scrolling left
        int time_since_scroll_start = scroll_cycle_time - idle_time_left;
        scroll_offset = time_since_scroll_start * scroll_speed / 1000;
      } else if (scroll_cycle_time < idle_time_left + scroll_time + idle_time_right) {
        // Scroll idle (right side)
        scroll_offset = headsign_overflow;
      } else if (scroll_cycle_time < idle_time_left + 2 * scroll_time + idle_time_right){
        // Scrolling right
        int time_since_scroll_start = scroll_cycle_time - (idle_time_left + scroll_time + idle_time_right);
        scroll_offset = headsign_overflow - (time_since_scroll_start * scroll_speed / 1000);
      } else {
        // Waiting for other headsigns to finish scrolling
        // scroll_offset = 0; do nothing
      }
    }

    this->display_->start_clipping(headsign_clipping_start, 0, headsign_clipping_end, this->display_->get_height());
    this->display_->print(headsign_clipping_start - scroll_offset, y_offset, this->font_, trip.headsign.c_str());
    this->display_->end_clipping();
}

void HOT TransitTracker::draw_schedule() { 
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


  }

  if (!esphome::network::is_connected()) {
    this->draw_text_centered_("Waiting for network", Color(0x252627));
    return;
  }

  if (!this->rtc_->now().is_valid()) {
    this->draw_text_centered_("Waiting for time sync", Color(0x252627));
    return;
  }

  if (this->base_url_.empty()) {
    this->draw_text_centered_("No base URL set", Color(0x252627));
    return;
  }

  if (this->status_has_error()) {
    this->draw_text_centered_("Error loading schedule", Color(0xFE4C5C));
    return;
  }

  if (!this->has_ever_connected_) {
    this->draw_text_centered_("Loading...", Color(0x252627));
    return;
  }

  if (this->schedule_state_.trips.empty()) {
    auto message = "No upcoming arrivals";
    if (this->display_departure_times_) {
      message = "No upcoming departures";
    }

    this->draw_text_centered_(message, Color(0x252627));
    return;
  }

  this->schedule_state_.mutex.lock();

  int nominal_font_height = this->font_->get_ascender() + this->font_->get_descender();
  unsigned long uptime = millis();
  uint rtc_now = this->rtc_->now().timestamp;

  int scroll_cycle_duration = 0;
  if (this->scroll_headsigns_) {
    int largest_headsign_overflow = 0;
    for (const Trip &trip : this->schedule_state_.trips) {
      int headsign_overflow;
      this->draw_trip(trip, 0, nominal_font_height, uptime, rtc_now, true, &headsign_overflow);
      largest_headsign_overflow = max(largest_headsign_overflow, headsign_overflow);
    }

    if (largest_headsign_overflow > 0) {
      int longest_scroll_time = largest_headsign_overflow * 1000 / scroll_speed;
      scroll_cycle_duration = idle_time_left + idle_time_right + 2*longest_scroll_time;
    }
  }

  int max_trips_height = (this->limit_ * this->font_->get_ascender()) + ((this->limit_ - 1) * this->font_->get_descender());
  int y_offset = (this->display_->get_height() % max_trips_height) / 2;

  for (const Trip &trip : this->schedule_state_.trips) {
    this->draw_trip(trip, y_offset, nominal_font_height, uptime, rtc_now, false, nullptr, scroll_cycle_duration);
    y_offset += nominal_font_height;
  }

  this->schedule_state_.mutex.unlock();

   // Reset alternating page to A whenever fresh data arrives,
  // so riders always see the nearest trains (2./3.) first.
  this->on_schedule_updated();
  
}

}  // namespace transit_tracker
}  // namespace esphome

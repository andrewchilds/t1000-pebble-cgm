/**
 * T1000 CGM Watchface
 *
 * A Pebble watchface for displaying Dexcom CGM data.
 * Displays: Time, Date, CGM value, trend arrow, delta, and 120-minute chart.
 */

#include <pebble.h>

// AppMessage keys (must match appinfo.json)
#define KEY_CGM_VALUE     0
#define KEY_CGM_DELTA     1
#define KEY_CGM_TREND     2
#define KEY_CGM_TIME_AGO  3
#define KEY_CGM_HISTORY   4
#define KEY_CGM_ALERT     5
#define KEY_REQUEST_DATA  6
#define KEY_LOW_THRESHOLD 7
#define KEY_HIGH_THRESHOLD 8
#define KEY_NEEDS_SETUP   9
#define KEY_REVERSED      10

// Trend arrow indices (Dexcom trend values)
#define TREND_NONE        0
#define TREND_DOUBLE_UP   1
#define TREND_UP          2
#define TREND_UP_45       3
#define TREND_FLAT        4
#define TREND_DOWN_45     5
#define TREND_DOWN        6
#define TREND_DOUBLE_DOWN 7

// Alert types
#define ALERT_NONE        0
#define ALERT_LOW_SOON    1
#define ALERT_HIGH        2

// Chart configuration
#define CHART_MAX_POINTS  24  // 120 minutes / 5 minutes = 24 points
#define CHART_DOT_SPACING 6   // Pixels between dots (was ~8px when auto-calculated)
#define CHART_Y_MIN       40
#define CHART_Y_MAX       300
#define CHART_DOT_RADIUS  3

// Display layout constants for Aplite (144x168)
#define SCREEN_WIDTH      144
#define SCREEN_HEIGHT     168

// Window and layers
static Window *s_main_window;
static Layer *s_chart_layer;
static Layer *s_battery_layer;
static Layer *s_sync_layer;
static Layer *s_alert_layer;
static TextLayer *s_time_date_layer;
static TextLayer *s_cgm_value_layer;
static TextLayer *s_delta_layer;
static TextLayer *s_time_ago_layer;
static BitmapLayer *s_trend_layer;
static GBitmap *s_trend_bitmap;
static TextLayer *s_setup_layer;
static TextLayer *s_no_data_layer;
static Layer *s_loading_layer;
static AppTimer *s_loading_timer;

// Battery state
static int s_battery_level = 0;
static bool s_battery_charging = false;

// Trend arrow resources (white on black for normal mode)
static const uint32_t TREND_ICONS_WHITE[] = {
    RESOURCE_ID_IMAGE_TREND_NONE_WHITE,
    RESOURCE_ID_IMAGE_TREND_DOUBLE_UP_WHITE,
    RESOURCE_ID_IMAGE_TREND_UP_WHITE,
    RESOURCE_ID_IMAGE_TREND_UP_45_WHITE,
    RESOURCE_ID_IMAGE_TREND_FLAT_WHITE,
    RESOURCE_ID_IMAGE_TREND_DOWN_45_WHITE,
    RESOURCE_ID_IMAGE_TREND_DOWN_WHITE,
    RESOURCE_ID_IMAGE_TREND_DOUBLE_DOWN_WHITE
};

// Trend arrow resources (black on white for reversed mode)
static const uint32_t TREND_ICONS_BLACK[] = {
    RESOURCE_ID_IMAGE_TREND_NONE_BLACK,
    RESOURCE_ID_IMAGE_TREND_DOUBLE_UP_BLACK,
    RESOURCE_ID_IMAGE_TREND_UP_BLACK,
    RESOURCE_ID_IMAGE_TREND_UP_45_BLACK,
    RESOURCE_ID_IMAGE_TREND_FLAT_BLACK,
    RESOURCE_ID_IMAGE_TREND_DOWN_45_BLACK,
    RESOURCE_ID_IMAGE_TREND_DOWN_BLACK,
    RESOURCE_ID_IMAGE_TREND_DOUBLE_DOWN_BLACK
};

// Text buffers
static char s_time_date_buffer[24];
static char s_cgm_value_buffer[8];
static char s_delta_buffer[12];
static char s_time_ago_buffer[16];

// Chart data
static int16_t s_chart_values[CHART_MAX_POINTS];
static int16_t s_chart_minutes_ago[CHART_MAX_POINTS];  // Minutes ago for each point
static int s_chart_count = 0;

// Current trend
static uint8_t s_current_trend = TREND_NONE;

// Time ago tracking
static int s_last_minutes_ago = -1;  // -1 = no data received yet
static time_t s_last_data_time = 0;   // When we last received data from phone

// Threshold settings (defaults, updated from phone)
static int s_low_threshold = 70;
static int s_high_threshold = 180;

// Display mode (false = white on black, true = black on white)
static bool s_reversed = false;

// Retry tracking for outbox failures
static bool s_is_retry = false;
static bool s_has_outbox_failure = false;  // True after retry also fails

// Sync spinner state (shown during data send/receive)
static bool s_is_syncing = false;
static int s_sync_frame = 0;
static AppTimer *s_sync_timer = NULL;
static AppTimer *s_sync_stop_timer = NULL;  // Timer to auto-stop spinner
#define SYNC_SPINNER_FRAMES 8
#define SYNC_SPINNER_INTERVAL 100  // ms per frame
#define SYNC_DISPLAY_MS 400  // Show sync spinner for a certain period of time on data send/receive

// Loading state
static bool s_is_loading = true;
static int s_loading_frame = 0;
static AppTimer *s_loading_timeout_timer;
#define LOADING_DOT_COUNT 3
#define LOADING_FRAMES_PER_DOT 6
#define LOADING_ANIMATION_INTERVAL 100  // ms per frame
#define LOADING_TIMEOUT_MS 15000  // 15 seconds

// Forward declarations
static void update_trend_icon(uint8_t trend);
static void update_layout_for_cgm_text(const char *cgm_text);
static void update_time_ago_display(void);
static void loading_timer_callback(void *data);
static void loading_timeout_callback(void *data);
static void show_data_layers(void);
static void hide_data_layers(void);
static void hide_loading_show_data(void);
static void sync_timer_callback(void *data);
static void sync_stop_timer_callback(void *data);
static void start_sync_spinner(void);
static void stop_sync_spinner(void);
static void update_alert_visibility(void);

/**
 * Apply colors based on reversed mode to all UI elements
 */
static void apply_colors() {
    GColor bg_color = s_reversed ? GColorWhite : GColorBlack;
    GColor fg_color = s_reversed ? GColorBlack : GColorWhite;

    // Update window background
    window_set_background_color(s_main_window, bg_color);

    // Update text layer colors
    text_layer_set_text_color(s_time_date_layer, fg_color);
    text_layer_set_text_color(s_cgm_value_layer, fg_color);
    text_layer_set_text_color(s_delta_layer, fg_color);
    text_layer_set_text_color(s_time_ago_layer, fg_color);
    text_layer_set_text_color(s_setup_layer, fg_color);
    text_layer_set_text_color(s_no_data_layer, fg_color);

    // Update bitmap compositing mode and reload trend icon
    // GCompOpOr for white-on-black icons, GCompOpAnd for black-on-white icons
    bitmap_layer_set_compositing_mode(s_trend_layer, s_reversed ? GCompOpAnd : GCompOpOr);
    update_trend_icon(s_current_trend);

    // Mark chart layer dirty to redraw with new colors
    if (s_chart_layer) {
        layer_mark_dirty(s_chart_layer);
    }

    // Mark loading layer dirty if visible
    if (s_loading_layer) {
        layer_mark_dirty(s_loading_layer);
    }

    // Mark battery layer dirty to redraw with new colors
    if (s_battery_layer) {
        layer_mark_dirty(s_battery_layer);
    }

    // Mark sync layer dirty to redraw with new colors
    if (s_sync_layer) {
        layer_mark_dirty(s_sync_layer);
    }

    // Mark alert layer dirty to redraw with new colors
    if (s_alert_layer) {
        layer_mark_dirty(s_alert_layer);
    }
}

/**
 * Draw the loading animation (three jumping dots)
 * Animation has 6 frames per dot cycle for smoother motion
 */
static void loading_layer_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    GColor fg_color = s_reversed ? GColorBlack : GColorWhite;

    graphics_context_set_fill_color(ctx, fg_color);

    // Dot configuration
    int dot_radius = 3;
    int dot_spacing = 14;
    int total_width = (LOADING_DOT_COUNT - 1) * dot_spacing;
    int start_x = (bounds.size.w - total_width) / 2;
    int base_y = bounds.size.h / 2;

    // Y offsets for smooth jump animation
    // Frame 0: starting up, 1: peak, 2: coming down, 3-5: at rest
    static const int jump_offsets[LOADING_FRAMES_PER_DOT] = { -4, -7, -3, 0, 0, 0 };

    for (int i = 0; i < LOADING_DOT_COUNT; i++) {
        int x = start_x + i * dot_spacing;

        // Calculate which frame this dot is in based on global frame
        // Each dot is offset by 2 frames to stagger the jumps evenly
        int dot_frame = (s_loading_frame - i * 2 + LOADING_FRAMES_PER_DOT * LOADING_DOT_COUNT) % LOADING_FRAMES_PER_DOT;
        int y_offset = jump_offsets[dot_frame];

        int y = base_y + y_offset;
        graphics_fill_circle(ctx, GPoint(x, y), dot_radius);
    }
}

/**
 * Draw the battery icon
 * Shows battery outline with fill level, and charging indicator if plugged in
 */
static void battery_layer_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    GColor fg_color = s_reversed ? GColorBlack : GColorWhite;

    // Battery icon dimensions
    int battery_width = 20;
    int battery_height = 10;
    int tip_width = 2;
    int tip_height = 4;
    int x = (bounds.size.w - battery_width - tip_width) / 2;
    int y = (bounds.size.h - battery_height) / 2;

    // Draw battery outline (rounded corners)
    graphics_context_set_stroke_color(ctx, fg_color);
    graphics_draw_round_rect(ctx, GRect(x, y, battery_width, battery_height), 1);

    // Draw battery tip (positive terminal)
    graphics_context_set_fill_color(ctx, fg_color);
    graphics_fill_rect(ctx, GRect(x + battery_width, y + (battery_height - tip_height) / 2, tip_width, tip_height), 0, GCornerNone);

    // Calculate fill width based on battery level (with 1px padding inside)
    int fill_padding = 2;
    int max_fill_width = battery_width - (fill_padding * 2);
    int fill_width = (s_battery_level * max_fill_width) / 100;

    // Draw fill
    if (fill_width > 0) {
        graphics_fill_rect(ctx, GRect(x + fill_padding, y + fill_padding, fill_width, battery_height - (fill_padding * 2)), 0, GCornerNone);
    }

    // Draw charging bolt if charging
    if (s_battery_charging) {
        // Simple lightning bolt in center of battery
        int bolt_x = x + battery_width / 2;
        int bolt_y = y + battery_height / 2;

        // Draw bolt using lines (inverted color for visibility)
        GColor bolt_color = s_reversed ? GColorWhite : GColorBlack;
        graphics_context_set_stroke_color(ctx, bolt_color);
        graphics_draw_line(ctx, GPoint(bolt_x + 1, y + 1), GPoint(bolt_x - 1, bolt_y));
        graphics_draw_line(ctx, GPoint(bolt_x - 1, bolt_y), GPoint(bolt_x + 1, bolt_y));
        graphics_draw_line(ctx, GPoint(bolt_x + 1, bolt_y), GPoint(bolt_x - 1, y + battery_height - 2));
    }
}

/**
 * Battery state change handler
 */
static void battery_handler(BatteryChargeState charge_state) {
    s_battery_level = charge_state.charge_percent;
    s_battery_charging = charge_state.is_charging;

    if (s_battery_layer) {
        layer_mark_dirty(s_battery_layer);
    }
}

/**
 * Draw the sync spinner (small rotating arc)
 */
static void sync_layer_update_proc(Layer *layer, GContext *ctx) {
    if (!s_is_syncing) {
        return;
    }

    GRect bounds = layer_get_bounds(layer);
    GColor fg_color = s_reversed ? GColorBlack : GColorWhite;

    int cx = bounds.size.w / 2;
    int cy = bounds.size.h / 2;

    graphics_context_set_stroke_color(ctx, fg_color);
    graphics_context_set_stroke_width(ctx, 2);

    // Draw spinning arc
    int radius = 4;

    // Draw arc segments based on current frame
    // Each frame rotates the arc by 45 degrees (360 / 8 frames)
    int start_angle = s_sync_frame * (360 / SYNC_SPINNER_FRAMES);

    // Draw a 270-degree arc (leaving a 90-degree gap for spinner effect)
    graphics_draw_arc(ctx,
        GRect(cx - radius, cy - radius, radius * 2, radius * 2),
        GOvalScaleModeFitCircle,
        DEG_TO_TRIGANGLE(start_angle),
        DEG_TO_TRIGANGLE(start_angle + 270));
}

/**
 * Draw the alert triangle icon (shown when data is stale AND connection failed)
 */
static void alert_layer_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    GColor fg_color = s_reversed ? GColorBlack : GColorWhite;
    GColor bg_color = s_reversed ? GColorWhite : GColorBlack;

    int cx = bounds.size.w / 2;
    int cy = bounds.size.h / 2;

    // Triangle points (pointing up)
    GPoint top = GPoint(cx, cy - 6);
    GPoint bottom_left = GPoint(cx - 7, cy + 4);
    GPoint bottom_right = GPoint(cx + 7, cy + 4);

    // Fill triangle with foreground color
    graphics_context_set_fill_color(ctx, fg_color);
    GPathInfo triangle_path_info = {
        .num_points = 3,
        .points = (GPoint[]) { top, bottom_left, bottom_right }
    };
    GPath *triangle_path = gpath_create(&triangle_path_info);
    gpath_draw_filled(ctx, triangle_path);

    // Draw exclamation mark inside with background color (1px wide, centered)
    graphics_context_set_fill_color(ctx, bg_color);
    graphics_fill_rect(ctx, GRect(cx, cy - 2, 2, 4), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(cx, cy + 3, 2, 1), 0, GCornerNone);
}

/**
 * Update alert icon visibility based on data staleness and connection status
 * Alert shown when: data 10+ min old AND outbox failure (including failed retry)
 * Alert hidden when: sync spinner is showing
 */
static void update_alert_visibility(void) {
    if (!s_alert_layer) {
        return;
    }

    // Don't update alert while syncing - it will be updated when sync stops
    if (s_is_syncing) {
        return;
    }

    // Calculate current data age
    int current_minutes_ago = 0;
    if (s_last_minutes_ago >= 0 && s_last_data_time > 0) {
        time_t now = time(NULL);
        int elapsed_minutes = (int)((now - s_last_data_time) / 60);
        current_minutes_ago = s_last_minutes_ago + elapsed_minutes;
    }

    // Show alert if data is 15+ minutes old AND we have an outbox failure
    bool show_alert = (current_minutes_ago >= 15) && s_has_outbox_failure;
    layer_set_hidden(s_alert_layer, !show_alert);
}

/**
 * Sync spinner timer callback
 */
static void sync_timer_callback(void *data) {
    if (!s_is_syncing) {
        s_sync_timer = NULL;
        return;
    }

    // Advance to next frame
    s_sync_frame = (s_sync_frame + 1) % SYNC_SPINNER_FRAMES;

    // Redraw the sync layer
    if (s_sync_layer) {
        layer_mark_dirty(s_sync_layer);
    }

    // Schedule next frame
    s_sync_timer = app_timer_register(SYNC_SPINNER_INTERVAL, sync_timer_callback, NULL);
}

/**
 * Timer callback to auto-stop sync spinner
 */
static void sync_stop_timer_callback(void *data) {
    s_sync_stop_timer = NULL;
    stop_sync_spinner();
}

/**
 * Start the sync spinner animation (auto-stops after SYNC_DISPLAY_MS)
 */
static void start_sync_spinner(void) {
    // Cancel any pending stop timer and restart the display period
    if (s_sync_stop_timer) {
        app_timer_cancel(s_sync_stop_timer);
    }
    s_sync_stop_timer = app_timer_register(SYNC_DISPLAY_MS, sync_stop_timer_callback, NULL);

    // Hide alert while syncing
    if (s_alert_layer) {
        layer_set_hidden(s_alert_layer, true);
    }

    if (s_is_syncing) {
        return;  // Animation already running, just reset the stop timer
    }

    s_is_syncing = true;
    s_sync_frame = 0;

    if (s_sync_layer) {
        layer_mark_dirty(s_sync_layer);
    }

    // Start animation timer
    s_sync_timer = app_timer_register(SYNC_SPINNER_INTERVAL, sync_timer_callback, NULL);
}

/**
 * Stop the sync spinner animation
 */
static void stop_sync_spinner(void) {
    if (!s_is_syncing) {
        return;  // Not running
    }

    s_is_syncing = false;

    if (s_sync_timer) {
        app_timer_cancel(s_sync_timer);
        s_sync_timer = NULL;
    }

    if (s_sync_stop_timer) {
        app_timer_cancel(s_sync_stop_timer);
        s_sync_stop_timer = NULL;
    }

    if (s_sync_layer) {
        layer_mark_dirty(s_sync_layer);
    }

    // Re-evaluate alert visibility now that sync is done
    update_alert_visibility();
}

/**
 * Loading animation timer callback
 */
static void loading_timer_callback(void *data) {
    if (!s_is_loading) {
        s_loading_timer = NULL;
        return;
    }

    // Advance to next frame
    s_loading_frame = (s_loading_frame + 1) % LOADING_FRAMES_PER_DOT;

    // Redraw the loading layer
    if (s_loading_layer) {
        layer_mark_dirty(s_loading_layer);
    }

    // Schedule next frame
    s_loading_timer = app_timer_register(LOADING_ANIMATION_INTERVAL, loading_timer_callback, NULL);
}

/**
 * Loading timeout callback - stop animation and show error message
 */
static void loading_timeout_callback(void *data) {
    s_loading_timeout_timer = NULL;

    if (!s_is_loading) {
        return;
    }

    s_is_loading = false;

    // Cancel animation timer
    if (s_loading_timer) {
        app_timer_cancel(s_loading_timer);
        s_loading_timer = NULL;
    }

    // Hide loading layer, show error in setup layer
    layer_set_hidden(s_loading_layer, true);
    text_layer_set_text(s_setup_layer, "Unable to connect");
    layer_set_hidden(text_layer_get_layer(s_setup_layer), false);
}

/**
 * Show all CGM data layers (except CGM value/trend/delta which are controlled by staleness check)
 */
static void show_data_layers(void) {
    // Note: CGM value, trend arrow, and delta visibility are controlled by
    // update_time_ago_display() based on data staleness, not shown unconditionally here.
    // This prevents a flash of stale data before the staleness check runs.
    layer_set_hidden(text_layer_get_layer(s_time_ago_layer), false);
    layer_set_hidden(s_chart_layer, false);
}

/**
 * Hide all CGM data layers
 */
static void hide_data_layers(void) {
    layer_set_hidden(text_layer_get_layer(s_cgm_value_layer), true);
    layer_set_hidden(bitmap_layer_get_layer(s_trend_layer), true);
    layer_set_hidden(text_layer_get_layer(s_delta_layer), true);
    layer_set_hidden(text_layer_get_layer(s_time_ago_layer), true);
    layer_set_hidden(s_chart_layer, true);
    layer_set_hidden(text_layer_get_layer(s_no_data_layer), true);
}

/**
 * Hide loading state and show CGM data
 */
static void hide_loading_show_data(void) {
    if (!s_is_loading) {
        return;
    }

    s_is_loading = false;

    // Cancel loading timers
    if (s_loading_timer) {
        app_timer_cancel(s_loading_timer);
        s_loading_timer = NULL;
    }
    if (s_loading_timeout_timer) {
        app_timer_cancel(s_loading_timeout_timer);
        s_loading_timeout_timer = NULL;
    }

    // Hide loading layer, show data layers
    layer_set_hidden(s_loading_layer, true);
    show_data_layers();
    // Update CGM value/trend/delta visibility based on staleness
    // (will be called again when KEY_CGM_TIME_AGO is processed, but that's fine)
    update_time_ago_display();
}

/**
 * Parse chart history data with timestamps
 * Format: "120:0,125:5,130:10,..." (value:minutesAgo pairs, most recent first)
 */
static void parse_chart_history(const char *history) {
    if (history == NULL || strlen(history) == 0) {
        s_chart_count = 0;
        return;
    }

    s_chart_count = 0;
    const char *ptr = history;

    while (*ptr && s_chart_count < CHART_MAX_POINTS) {
        // Parse glucose value
        int value = 0;
        while (*ptr >= '0' && *ptr <= '9') {
            value = value * 10 + (*ptr - '0');
            ptr++;
        }

        // Parse minutes ago (after colon)
        int minutes_ago = 0;
        if (*ptr == ':') {
            ptr++;
            while (*ptr >= '0' && *ptr <= '9') {
                minutes_ago = minutes_ago * 10 + (*ptr - '0');
                ptr++;
            }
        }

        if (value > 0) {
            s_chart_values[s_chart_count] = (int16_t)value;
            s_chart_minutes_ago[s_chart_count] = (int16_t)minutes_ago;
            s_chart_count++;
        }

        // Skip comma
        if (*ptr == ',') {
            ptr++;
        } else if (*ptr != '\0') {
            break;
        }
    }
}

/**
 * Get color for a glucose value (color platforms only)
 * Returns red for low, orange for high, green for in-range
 */
#ifdef PBL_COLOR
static GColor get_glucose_color(int value) {
    if (value <= s_low_threshold) {
        return GColorRed;
    } else if (value >= s_high_threshold) {
        return GColorOrange;
    } else {
        return GColorGreen;
    }
}
#endif

/**
 * Draw the CGM dot chart
 */
static void chart_layer_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);

    // Set colors based on reversed mode
    GColor bg_color = s_reversed ? GColorWhite : GColorBlack;
    GColor fg_color = s_reversed ? GColorBlack : GColorWhite;

    // Draw background
    graphics_context_set_fill_color(ctx, bg_color);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    if (s_chart_count == 0) {
        return;
    }

    // Calculate chart dimensions with margins
    int margin = 4;
    int chart_height = bounds.size.h - (margin * 2);

    // Map thresholds to Y coordinates
    int low_y = bounds.origin.y + margin + chart_height -
                ((s_low_threshold - CHART_Y_MIN) * chart_height / (CHART_Y_MAX - CHART_Y_MIN));
    int high_y = bounds.origin.y + margin + chart_height -
                 ((s_high_threshold - CHART_Y_MIN) * chart_height / (CHART_Y_MAX - CHART_Y_MIN));

    // Draw dashed threshold lines
    int dash_length = 4;
    int gap_length = 3;
    for (int x = bounds.origin.x + margin; x < bounds.origin.x + bounds.size.w - margin; x += dash_length + gap_length) {
        int end_x = x + dash_length - 1;
        if (end_x > bounds.origin.x + bounds.size.w - margin) {
            end_x = bounds.origin.x + bounds.size.w - margin;
        }
#ifdef PBL_COLOR
        // Color platforms: red for low threshold, orange for high threshold
        graphics_context_set_stroke_color(ctx, GColorRed);
        graphics_draw_line(ctx, GPoint(x, low_y), GPoint(end_x, low_y));
        graphics_context_set_stroke_color(ctx, GColorOrange);
        graphics_draw_line(ctx, GPoint(x, high_y), GPoint(end_x, high_y));
#else
        // Monochrome platforms: use foreground color for both
        graphics_context_set_stroke_color(ctx, fg_color);
        graphics_draw_line(ctx, GPoint(x, low_y), GPoint(end_x, low_y));
        graphics_draw_line(ctx, GPoint(x, high_y), GPoint(end_x, high_y));
#endif
    }

    // Draw dots for each data point
    // Data comes in most-recent-first, so we plot right-to-left
    // X position is based on actual timestamp, not array index

    // Calculate elapsed time since data was received to adjust positions
    int elapsed_minutes = 0;
    if (s_last_data_time > 0) {
        time_t now = time(NULL);
        elapsed_minutes = (int)((now - s_last_data_time) / 60);
    }

    for (int i = 0; i < s_chart_count; i++) {
        int value = s_chart_values[i];
        int original_value = value;  // Keep original for color determination

        // Clamp value to chart range
        if (value < CHART_Y_MIN) value = CHART_Y_MIN;
        if (value > CHART_Y_MAX) value = CHART_Y_MAX;

        // Calculate X position based on actual minutes ago (plus elapsed time)
        // Right edge = 0 minutes ago, left edge = 120 minutes ago
        // pixels_per_minute = CHART_DOT_SPACING / 5
        int total_minutes_ago = s_chart_minutes_ago[i] + elapsed_minutes;
        int pixel_offset = (total_minutes_ago * CHART_DOT_SPACING) / 5;
        int x = bounds.origin.x + bounds.size.w - margin - pixel_offset;

        // Skip points that have scrolled off the left edge
        if (x < bounds.origin.x + margin) {
            continue;
        }

        // Calculate Y position (invert because screen Y increases downward)
        int y = bounds.origin.y + margin + chart_height -
                ((value - CHART_Y_MIN) * chart_height / (CHART_Y_MAX - CHART_Y_MIN));

        // Set dot color based on platform
#ifdef PBL_COLOR
        graphics_context_set_fill_color(ctx, get_glucose_color(original_value));
#else
        graphics_context_set_fill_color(ctx, fg_color);
#endif

        // Draw filled circle for each point
        // Most recent dot (i=0) uses full radius if within 10 minutes, others are 1px smaller
        int radius = (i == 0 && total_minutes_ago < 10) ? CHART_DOT_RADIUS : CHART_DOT_RADIUS - 1;
        graphics_fill_circle(ctx, GPoint(x, y), radius);
    }
}

/**
 * Update the trend arrow icon
 */
static void update_trend_icon(uint8_t trend) {
    if (trend > TREND_DOUBLE_DOWN) {
        trend = TREND_NONE;
    }

    s_current_trend = trend;

    // Destroy old bitmap if exists
    if (s_trend_bitmap) {
        gbitmap_destroy(s_trend_bitmap);
    }

    // Load new bitmap based on reversed mode
    const uint32_t *icons = s_reversed ? TREND_ICONS_BLACK : TREND_ICONS_WHITE;
    s_trend_bitmap = gbitmap_create_with_resource(icons[trend]);
    bitmap_layer_set_bitmap(s_trend_layer, s_trend_bitmap);
}

/**
 * Update layout positions based on CGM text width
 * Dynamically positions trend arrow and delta based on actual rendered text width
 * Hides delta for LOW/HIGH values since there's no room
 */
static void update_layout_for_cgm_text(const char *cgm_text) {
    int cgmValueYPos = 24;

    // Check if this is a LOW or HIGH value - hide delta in these cases
    bool hide_delta = (strcmp(cgm_text, "LOW") == 0 || strcmp(cgm_text, "HIGH") == 0);
    layer_set_hidden(text_layer_get_layer(s_delta_layer), hide_delta);

    // Get the actual rendered width of the CGM text
    GSize text_size = graphics_text_layout_get_content_size(
        cgm_text,
        fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD),
        GRect(0, 0, 110, 48),
        GTextOverflowModeTrailingEllipsis,
        GTextAlignmentLeft
    );

    // Position trend arrow just after the CGM text
    // 4 is CGM layer x offset, add small gap after text
    int trend_x = 4 + text_size.w + 3;
    int delta_x = trend_x + 32;

    layer_set_frame(bitmap_layer_get_layer(s_trend_layer),
                    GRect(trend_x, cgmValueYPos + 13, 30, 30));
    layer_set_frame(text_layer_get_layer(s_delta_layer),
                    GRect(delta_x, cgmValueYPos + 10, 38, 28));
}

/**
 * Update time ago display based on stored data
 * Also handles showing "No Data" when CGM data is 60+ minutes old
 */
static void update_time_ago_display() {
    if (s_last_minutes_ago < 0) {
        // No data received yet
        return;
    }

    // Calculate current minutes ago based on elapsed time since last data
    time_t now = time(NULL);
    int elapsed_minutes = (int)((now - s_last_data_time) / 60);
    int current_minutes_ago = s_last_minutes_ago + elapsed_minutes;

    // Check if data is stale (60+ minutes old)
    bool is_stale = current_minutes_ago >= 60;

    // Show/hide CGM value, trend arrow, and delta based on staleness
    layer_set_hidden(text_layer_get_layer(s_cgm_value_layer), is_stale);
    layer_set_hidden(bitmap_layer_get_layer(s_trend_layer), is_stale);
    layer_set_hidden(text_layer_get_layer(s_delta_layer), is_stale);
    layer_set_hidden(text_layer_get_layer(s_no_data_layer), !is_stale);

    // Update display
    if (current_minutes_ago == 0) {
        snprintf(s_time_ago_buffer, sizeof(s_time_ago_buffer), "now");
    } else if (current_minutes_ago >= 90) {
        int hours = current_minutes_ago / 60;
        int mins = current_minutes_ago % 60;
        snprintf(s_time_ago_buffer, sizeof(s_time_ago_buffer), "%dh %dm ago", hours, mins);
    } else {
        snprintf(s_time_ago_buffer, sizeof(s_time_ago_buffer), "%dm ago", current_minutes_ago);
    }
    text_layer_set_text(s_time_ago_layer, s_time_ago_buffer);

    // Update alert visibility based on staleness
    update_alert_visibility();
}

/**
 * Update time display
 */
static void update_time() {
    time_t now = time(NULL);
    struct tm *tick_time = localtime(&now);

    // Format time (12-hour format without leading zero)
    char time_str[8];
    strftime(time_str, sizeof(time_str),
             clock_is_24h_style() ? "%H:%M" : "%l:%M", tick_time);

    // Trim leading space for 12-hour format
    char *time_ptr = time_str;
    if (time_ptr[0] == ' ') {
        time_ptr++;
    }

    // Format date (day of week + day number)
    char date_str[12];
    strftime(date_str, sizeof(date_str), "%a %d", tick_time);

    // Combine with two spaces between
    snprintf(s_time_date_buffer, sizeof(s_time_date_buffer), "%s  %s", time_ptr, date_str);
    text_layer_set_text(s_time_date_layer, s_time_date_buffer);
}

/**
 * Tick handler - called every minute
 */
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    update_time();
    update_time_ago_display();

    // Redraw chart to shift dots based on elapsed time
    if (s_chart_layer) {
        layer_mark_dirty(s_chart_layer);
    }

    // Only request data from phone if CGM reading is 4+ minutes old
    // (Dexcom only updates every 5 minutes, so no point asking more frequently)
    if (s_last_data_time > 0 && s_last_minutes_ago >= 0) {
        time_t now = time(NULL);
        int elapsed_minutes = (int)((now - s_last_data_time) / 60);
        int current_cgm_age = s_last_minutes_ago + elapsed_minutes;

        if (current_cgm_age < 4) {
            // Data is still fresh, no need to request update
            return;
        }
    }

    // Request data update from phone
    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);
    if (iter) {
        dict_write_uint8(iter, KEY_REQUEST_DATA, 1);
        app_message_outbox_send();
        start_sync_spinner();
    }
}

/**
 * AppMessage received callback
 */
static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
    // Clear outbox failure flag on successful communication
    s_has_outbox_failure = false;

    // Show sync spinner briefly to indicate data reception
    start_sync_spinner();

    // Hide loading state on first data received
    if (s_is_loading) {
        hide_loading_show_data();
    }

    // Read CGM value
    Tuple *cgm_value_tuple = dict_find(iterator, KEY_CGM_VALUE);
    if (cgm_value_tuple) {
        snprintf(s_cgm_value_buffer, sizeof(s_cgm_value_buffer), "%s", cgm_value_tuple->value->cstring);
        text_layer_set_text(s_cgm_value_layer, s_cgm_value_buffer);
        update_layout_for_cgm_text(s_cgm_value_buffer);
    }

    // Read delta
    Tuple *delta_tuple = dict_find(iterator, KEY_CGM_DELTA);
    if (delta_tuple) {
        snprintf(s_delta_buffer, sizeof(s_delta_buffer), "%s", delta_tuple->value->cstring);
        text_layer_set_text(s_delta_layer, s_delta_buffer);
    }

    // Read trend
    Tuple *trend_tuple = dict_find(iterator, KEY_CGM_TREND);
    if (trend_tuple) {
        update_trend_icon(trend_tuple->value->uint8);
    }

    // Read time ago
    Tuple *time_ago_tuple = dict_find(iterator, KEY_CGM_TIME_AGO);
    if (time_ago_tuple) {
        s_last_minutes_ago = time_ago_tuple->value->int32;
        s_last_data_time = time(NULL);
        update_time_ago_display();
    }

    // Read chart history
    Tuple *history_tuple = dict_find(iterator, KEY_CGM_HISTORY);
    if (history_tuple) {
        parse_chart_history(history_tuple->value->cstring);
        layer_mark_dirty(s_chart_layer);
    }

    // Read threshold settings
    Tuple *low_threshold_tuple = dict_find(iterator, KEY_LOW_THRESHOLD);
    if (low_threshold_tuple) {
        s_low_threshold = low_threshold_tuple->value->int32;
        layer_mark_dirty(s_chart_layer);
    }

    Tuple *high_threshold_tuple = dict_find(iterator, KEY_HIGH_THRESHOLD);
    if (high_threshold_tuple) {
        s_high_threshold = high_threshold_tuple->value->int32;
        layer_mark_dirty(s_chart_layer);
    }

    // Handle alert vibration
    Tuple *alert_tuple = dict_find(iterator, KEY_CGM_ALERT);
    if (alert_tuple) {
        uint8_t alert_type = alert_tuple->value->uint8;
        if (alert_type == ALERT_LOW_SOON) {
            // Low soon alert: accelerating pattern
            static const uint32_t low_soon_pattern[] = { 70, 300, 70, 200, 70, 120, 70, 80, 70 };
            vibes_enqueue_custom_pattern((VibePattern) {
                .durations = low_soon_pattern,
                .num_segments = ARRAY_LENGTH(low_soon_pattern)
            });
            APP_LOG(APP_LOG_LEVEL_INFO, "Low soon alert vibration triggered");
        } else if (alert_type == ALERT_HIGH) {
            // High alert pattern
            static const uint32_t high_pattern[] = { 90, 120, 90, 200, 90, 300, 90 };
            vibes_enqueue_custom_pattern((VibePattern) {
                .durations = high_pattern,
                .num_segments = ARRAY_LENGTH(high_pattern)
            });
            APP_LOG(APP_LOG_LEVEL_INFO, "High alert vibration triggered");
        }
    }

    // Read reversed setting
    Tuple *reversed_tuple = dict_find(iterator, KEY_REVERSED);
    if (reversed_tuple) {
        bool new_reversed = reversed_tuple->value->uint8 != 0;
        if (new_reversed != s_reversed) {
            s_reversed = new_reversed;
            apply_colors();
        }
    }

    // Check for setup needed message
    Tuple *needs_setup_tuple = dict_find(iterator, KEY_NEEDS_SETUP);
    if (needs_setup_tuple && needs_setup_tuple->value->uint8) {
        // Hide CGM data, show setup message
        hide_data_layers();
        layer_set_hidden(text_layer_get_layer(s_setup_layer), false);
    } else if (needs_setup_tuple) {
        // Show CGM data, hide setup message
        show_data_layers();
        layer_set_hidden(text_layer_get_layer(s_setup_layer), true);
        // Update CGM value/trend/delta visibility based on staleness
        update_time_ago_display();
    }
}

/**
 * AppMessage dropped callback
 */
static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped: %d", reason);
}

/**
 * AppMessage failed callback - retry once on failure
 */
static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed: %d", reason);

    // Only retry once to avoid infinite loops
    if (!s_is_retry) {
        APP_LOG(APP_LOG_LEVEL_INFO, "Retrying outbox send...");
        s_is_retry = true;

        DictionaryIterator *retry_iter;
        AppMessageResult result = app_message_outbox_begin(&retry_iter);
        if (result == APP_MSG_OK && retry_iter) {
            dict_write_uint8(retry_iter, KEY_REQUEST_DATA, 1);
            app_message_outbox_send();
        } else {
            APP_LOG(APP_LOG_LEVEL_ERROR, "Retry outbox_begin failed: %d", result);
            s_is_retry = false;
            s_has_outbox_failure = true;
            stop_sync_spinner();
        }
    } else {
        APP_LOG(APP_LOG_LEVEL_ERROR, "Retry also failed, giving up");
        s_is_retry = false;
        s_has_outbox_failure = true;
        stop_sync_spinner();
    }
}

/**
 * AppMessage sent callback
 */
static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Outbox send success");
    // Reset retry flag on success so next failure can retry
    s_is_retry = false;
    // Spinner will auto-stop via the timer scheduled in start_sync_spinner
}

/**
 * Create a text layer with common settings
 */
static TextLayer* create_text_layer(GRect frame, GFont font, GTextAlignment alignment) {
    TextLayer *layer = text_layer_create(frame);
    text_layer_set_background_color(layer, GColorClear);
    text_layer_set_text_color(layer, GColorWhite);
    text_layer_set_font(layer, font);
    text_layer_set_text_alignment(layer, alignment);
    return layer;
}

/**
 * Main window load
 */
static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    window_set_background_color(window, GColorBlack);

    // Layout (top to bottom):
    // - Time + Date (single row, medium font) - height ~28
    // - CGM value (large) + trend arrow + delta - height ~50
    // - Time ago - height ~20
    // - Chart - remaining space

    // Time and date layer - single row at top, left-aligned
    s_time_date_layer = create_text_layer(
        GRect(6, -4, bounds.size.w - 6, 34),
        fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
        GTextAlignmentLeft
    );
    layer_add_child(window_layer, text_layer_get_layer(s_time_date_layer));

    int cgmValueYPos = 24;

    // CGM value layer - centered vertically at y=26, font height ~34px
    s_cgm_value_layer = create_text_layer(
        GRect(4, cgmValueYPos, 110, 48),
        fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD),
        GTextAlignmentLeft
    );
    text_layer_set_text(s_cgm_value_layer, "");
    layer_add_child(window_layer, text_layer_get_layer(s_cgm_value_layer));

    s_trend_layer = bitmap_layer_create(GRect(78, cgmValueYPos + 13, 30, 30));
    bitmap_layer_set_compositing_mode(s_trend_layer, GCompOpOr);
    bitmap_layer_set_alignment(s_trend_layer, GAlignCenter);
    const uint32_t *icons = s_reversed ? TREND_ICONS_BLACK : TREND_ICONS_WHITE;
    s_trend_bitmap = gbitmap_create_with_resource(icons[TREND_NONE]);
    bitmap_layer_set_bitmap(s_trend_layer, s_trend_bitmap);
    layer_add_child(window_layer, bitmap_layer_get_layer(s_trend_layer));

    s_delta_layer = create_text_layer(
        GRect(110, cgmValueYPos + 12, 38, 28),
        fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
        GTextAlignmentLeft
    );
    text_layer_set_text(s_delta_layer, "");
    layer_add_child(window_layer, text_layer_get_layer(s_delta_layer));

    // "No Data" layer - shown when CGM data is 60+ minutes old, centered in CGM value area
    s_no_data_layer = create_text_layer(
        GRect(0, cgmValueYPos + 10, bounds.size.w, 28),
        fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
        GTextAlignmentCenter
    );
    text_layer_set_text(s_no_data_layer, "No Data");
    layer_set_hidden(text_layer_get_layer(s_no_data_layer), true);
    layer_add_child(window_layer, text_layer_get_layer(s_no_data_layer));

    // Chart layer - below CGM value row
    s_chart_layer = layer_create(GRect(0, 70, bounds.size.w, 74));
    layer_set_update_proc(s_chart_layer, chart_layer_update_proc);
    layer_add_child(window_layer, s_chart_layer);

    // Time ago layer - bottom of screen, right-aligned
    s_time_ago_layer = create_text_layer(
        GRect(0, 138, bounds.size.w - 6, 28),
        fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
        GTextAlignmentRight
    );
    text_layer_set_text(s_time_ago_layer, "---");
    layer_add_child(window_layer, text_layer_get_layer(s_time_ago_layer));

    // Battery layer - bottom left corner
    s_battery_layer = layer_create(GRect(4, 145, 30, 22));
    layer_set_update_proc(s_battery_layer, battery_layer_update_proc);
    layer_add_child(window_layer, s_battery_layer);

    // Sync spinner layer - to the right of battery icon
    s_sync_layer = layer_create(GRect(34, 148, 16, 16));
    layer_set_update_proc(s_sync_layer, sync_layer_update_proc);
    layer_add_child(window_layer, s_sync_layer);

    // Alert triangle layer - same position as sync layer (mutually exclusive visibility)
    s_alert_layer = layer_create(GRect(33, 146, 20, 20));
    layer_set_update_proc(s_alert_layer, alert_layer_update_proc);
    layer_set_hidden(s_alert_layer, true);  // Hidden by default
    layer_add_child(window_layer, s_alert_layer);

    // Setup message layer - centered, covers chart area, hidden by default
    s_setup_layer = create_text_layer(
        GRect(6, 50, bounds.size.w - 12, 74),
        fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
        GTextAlignmentCenter
    );
    text_layer_set_text(s_setup_layer, "Go to T1000 >\nSettings to\nfinish setup.");
    layer_set_hidden(text_layer_get_layer(s_setup_layer), true);
    layer_add_child(window_layer, text_layer_get_layer(s_setup_layer));

    // Loading layer - centered in the data area, shows jumping dots
    s_loading_layer = layer_create(GRect(0, 24, bounds.size.w, 120));
    layer_set_update_proc(s_loading_layer, loading_layer_update_proc);
    layer_add_child(window_layer, s_loading_layer);

    // Start in loading state - hide data layers, start animation and timeout
    hide_data_layers();
    s_loading_timer = app_timer_register(LOADING_ANIMATION_INTERVAL, loading_timer_callback, NULL);
    s_loading_timeout_timer = app_timer_register(LOADING_TIMEOUT_MS, loading_timeout_callback, NULL);

    // Initialize time display
    update_time();
}

/**
 * Main window unload
 */
static void main_window_unload(Window *window) {
    // Cancel loading timers if running
    if (s_loading_timer) {
        app_timer_cancel(s_loading_timer);
        s_loading_timer = NULL;
    }
    if (s_loading_timeout_timer) {
        app_timer_cancel(s_loading_timeout_timer);
        s_loading_timeout_timer = NULL;
    }

    // Cancel sync timers if running
    if (s_sync_timer) {
        app_timer_cancel(s_sync_timer);
        s_sync_timer = NULL;
    }
    if (s_sync_stop_timer) {
        app_timer_cancel(s_sync_stop_timer);
        s_sync_stop_timer = NULL;
    }

    text_layer_destroy(s_time_date_layer);
    text_layer_destroy(s_cgm_value_layer);
    text_layer_destroy(s_delta_layer);
    text_layer_destroy(s_time_ago_layer);
    text_layer_destroy(s_setup_layer);
    text_layer_destroy(s_no_data_layer);
    bitmap_layer_destroy(s_trend_layer);
    layer_destroy(s_chart_layer);
    layer_destroy(s_loading_layer);
    layer_destroy(s_battery_layer);
    layer_destroy(s_sync_layer);
    layer_destroy(s_alert_layer);

    if (s_trend_bitmap) {
        gbitmap_destroy(s_trend_bitmap);
    }
}

/**
 * Initialize app
 */
static void init() {
    // Create main window
    s_main_window = window_create();
    window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load = main_window_load,
        .unload = main_window_unload
    });
    window_stack_push(s_main_window, true);

    // Register tick handler
    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

    // Register battery state handler and get initial state
    battery_state_service_subscribe(battery_handler);
    battery_handler(battery_state_service_peek());

    // Register AppMessage callbacks
    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    app_message_register_outbox_sent(outbox_sent_callback);

    // Open AppMessage with appropriate buffer sizes
    // Inbox needs to hold chart history (24 values * ~8 chars each = ~192) plus other fields
    app_message_open(512, 64);
}

/**
 * Deinitialize app
 */
static void deinit() {
    tick_timer_service_unsubscribe();
    battery_state_service_unsubscribe();
    window_destroy(s_main_window);
}

/**
 * Main entry point
 */
int main(void) {
    init();
    app_event_loop();
    deinit();
}

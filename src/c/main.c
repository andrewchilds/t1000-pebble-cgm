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
static TextLayer *s_time_date_layer;
static TextLayer *s_cgm_value_layer;
static TextLayer *s_delta_layer;
static TextLayer *s_time_ago_layer;
static BitmapLayer *s_trend_layer;
static GBitmap *s_trend_bitmap;
static TextLayer *s_setup_layer;

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

// Forward declaration
static void update_trend_icon(uint8_t trend);
static void update_layout_for_cgm_text(const char *cgm_text);

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

    // Update bitmap compositing mode and reload trend icon
    // GCompOpOr for white-on-black icons, GCompOpAnd for black-on-white icons
    bitmap_layer_set_compositing_mode(s_trend_layer, s_reversed ? GCompOpAnd : GCompOpOr);
    update_trend_icon(s_current_trend);

    // Mark chart layer dirty to redraw with new colors
    if (s_chart_layer) {
        layer_mark_dirty(s_chart_layer);
    }
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
    int chart_width = bounds.size.w - (margin * 2);
    int chart_height = bounds.size.h - (margin * 2);

    // Map thresholds to Y coordinates
    int low_y = bounds.origin.y + margin + chart_height -
                ((s_low_threshold - CHART_Y_MIN) * chart_height / (CHART_Y_MAX - CHART_Y_MIN));
    int high_y = bounds.origin.y + margin + chart_height -
                 ((s_high_threshold - CHART_Y_MIN) * chart_height / (CHART_Y_MAX - CHART_Y_MIN));

    // Draw dashed threshold lines
    graphics_context_set_stroke_color(ctx, fg_color);
    int dash_length = 4;
    int gap_length = 3;
    for (int x = bounds.origin.x + margin; x < bounds.origin.x + bounds.size.w - margin; x += dash_length + gap_length) {
        int end_x = x + dash_length - 1;
        if (end_x > bounds.origin.x + bounds.size.w - margin) {
            end_x = bounds.origin.x + bounds.size.w - margin;
        }
        graphics_draw_line(ctx, GPoint(x, low_y), GPoint(end_x, low_y));
        graphics_draw_line(ctx, GPoint(x, high_y), GPoint(end_x, high_y));
    }

    // Draw dots for each data point
    // Data comes in most-recent-first, so we plot right-to-left
    // X position is based on actual timestamp, not array index
    graphics_context_set_fill_color(ctx, fg_color);

    // Calculate elapsed time since data was received to adjust positions
    int elapsed_minutes = 0;
    if (s_last_data_time > 0) {
        time_t now = time(NULL);
        elapsed_minutes = (int)((now - s_last_data_time) / 60);
    }

    for (int i = 0; i < s_chart_count; i++) {
        int value = s_chart_values[i];

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
    int delta_x = trend_x + 33;

    layer_set_frame(bitmap_layer_get_layer(s_trend_layer),
                    GRect(trend_x, cgmValueYPos + 13, 30, 30));
    layer_set_frame(text_layer_get_layer(s_delta_layer),
                    GRect(delta_x, cgmValueYPos + 10, 38, 28));
}

/**
 * Update time ago display based on stored data
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

    // Update display
    if (current_minutes_ago == 0) {
        snprintf(s_time_ago_buffer, sizeof(s_time_ago_buffer), "now");
    } else {
        snprintf(s_time_ago_buffer, sizeof(s_time_ago_buffer), "%dm ago", current_minutes_ago);
    }
    text_layer_set_text(s_time_ago_layer, s_time_ago_buffer);
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

    // Request data update from phone
    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);
    if (iter) {
        dict_write_uint8(iter, KEY_REQUEST_DATA, 1);
        app_message_outbox_send();
    }
}

/**
 * AppMessage received callback
 */
static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
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
        layer_set_hidden(text_layer_get_layer(s_cgm_value_layer), true);
        layer_set_hidden(bitmap_layer_get_layer(s_trend_layer), true);
        layer_set_hidden(text_layer_get_layer(s_delta_layer), true);
        layer_set_hidden(text_layer_get_layer(s_time_ago_layer), true);
        layer_set_hidden(text_layer_get_layer(s_setup_layer), false);
    } else if (needs_setup_tuple) {
        // Show CGM data, hide setup message
        layer_set_hidden(text_layer_get_layer(s_cgm_value_layer), false);
        layer_set_hidden(bitmap_layer_get_layer(s_trend_layer), false);
        layer_set_hidden(text_layer_get_layer(s_delta_layer), false);
        layer_set_hidden(text_layer_get_layer(s_time_ago_layer), false);
        layer_set_hidden(text_layer_get_layer(s_setup_layer), true);
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
        }
    } else {
        APP_LOG(APP_LOG_LEVEL_ERROR, "Retry also failed, giving up");
        s_is_retry = false;
    }
}

/**
 * AppMessage sent callback
 */
static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Outbox send success");
    // Reset retry flag on success so next failure can retry
    s_is_retry = false;
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
    text_layer_set_text(s_cgm_value_layer, "---");
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

    // Setup message layer - centered, covers chart area, hidden by default
    s_setup_layer = create_text_layer(
        GRect(6, 60, bounds.size.w - 12, 74),
        fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
        GTextAlignmentCenter
    );
    text_layer_set_text(s_setup_layer, "Go to T1000 >\nSettings to\nfinish setup.");
    layer_set_hidden(text_layer_get_layer(s_setup_layer), true);
    layer_add_child(window_layer, text_layer_get_layer(s_setup_layer));

    // Initialize time display
    update_time();
}

/**
 * Main window unload
 */
static void main_window_unload(Window *window) {
    text_layer_destroy(s_time_date_layer);
    text_layer_destroy(s_cgm_value_layer);
    text_layer_destroy(s_delta_layer);
    text_layer_destroy(s_time_ago_layer);
    text_layer_destroy(s_setup_layer);
    bitmap_layer_destroy(s_trend_layer);
    layer_destroy(s_chart_layer);

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

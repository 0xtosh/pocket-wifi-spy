#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <FastLED.h>
#include <lvgl.h>
#include <ctype.h>
#include <stdarg.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7735.h"

#ifndef BOOT_PIN
#define BOOT_PIN 0
#endif

#define LED_DI_PIN 40
#define LED_CI_PIN 39

#define LCD_HOST SPI2_HOST
#define PIN_NUM_MOSI 3
#define PIN_NUM_CLK 5
#define PIN_NUM_CS 4
#define PIN_NUM_DC 2
#define PIN_NUM_RST 1
#define PIN_NUM_BCKL 38

#define SD_MMC_D0_PIN 14
#define SD_MMC_D1_PIN 17
#define SD_MMC_D2_PIN 21
#define SD_MMC_D3_PIN 18
#define SD_MMC_CLK_PIN 12
#define SD_MMC_CMD_PIN 16

#define LCD_PIXEL_WIDTH 160
#define LCD_PIXEL_HEIGHT 80
#define LEDC_BACKLIGHT_FREQ 1000
#define LEDC_BACKLIGHT_BIT_WIDTH 8
#define LEDC_BACKLIGHT_CHANNEL 3

#define CHANNEL_MIN 1
#define CHANNEL_MAX 13
#define CHANNEL_HOP_MS 350
#define CAPTURE_DISCOVERY_HOP_MS 900
#define CAPTURE_LOCK_HOP_MS 2200
#define CAPTURE_EAPOL_LOCK_MS 5000
#define CAPTURE_TARGET_TTL_MS 30000
#define BUTTON_DEBOUNCE_MS 250
#define MODE_SWITCH_DELAY_MS 2000
#define WIFI_INIT_DELAY_MS 1800
#define UI_UPDATE_MS 125
#define LOG_FLUSH_MS 1000
#define ALERT_TIMEOUT_MS 8000
#define ALERT_EXTRA_HOLD_MS 1000
#define BURST_WINDOW_MS 1500
#define ALERT_THRESHOLD 2
#define DEAUTH_LOG_WINDOW_MS 3000
#define DEAUTH_LOG_FRAMES_PER_WINDOW 3
#define MAX_TARGETS 32
#define MAX_CAPTURE_OBSERVATIONS 48
#define MAX_SSID_LEN 32
#define EVENT_QUEUE_SIZE 128
#define MAX_FRAME_LEN 384
#define PCAP_LINKTYPE_IEEE802_11 105

enum OperatingMode : uint8_t {
    MODE_TARGETS = 1,
    MODE_ALL = 2,
    MODE_CAPTURE = 3,
    MODE_DEAUTH = 4,
    MODE_PAUSE = 5,
};

enum CapturedFrameKind : uint8_t {
    FRAME_PROBE_REQUEST = 1,
    FRAME_EAPOL = 2,
    FRAME_DEAUTH = 3,
};

enum AlertKind : uint8_t {
    ALERT_NONE = 0,
    ALERT_AP_TARGETED,
    ALERT_CLIENT_TARGETED,
    ALERT_UNKNOWN
};

struct ProbeEvent {
    uint32_t uptime_ms;
    uint16_t capture_len;
    uint16_t original_len;
    int8_t rssi;
    uint8_t channel;
    uint8_t client[6];
    uint8_t destination[6];
    uint8_t bssid[6];
    char ssid[MAX_SSID_LEN + 1];
    bool wildcard_ssid;
    CapturedFrameKind kind;
    uint16_t reason;
    uint8_t eapol_message;
    uint8_t burst_count;
    AlertKind alert_kind;
    bool destination_is_broadcast;
    uint8_t frame[MAX_FRAME_LEN];
};

struct PcapGlobalHeader {
    uint32_t magic_number;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;
};

struct PcapPacketHeader {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};

struct CaptureObservation {
    bool valid;
    char ssid[MAX_SSID_LEN + 1];
    uint8_t bssid[6];
    uint8_t channel;
    uint32_t last_seen_ms;
};

static esp_lcd_panel_handle_t panel_handle = nullptr;
static esp_lcd_panel_io_handle_t io_handle = nullptr;
static spi_device_handle_t spi_handle = nullptr;
static esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = PIN_NUM_RST,
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    .color_space = ESP_LCD_COLOR_SPACE_BGR,
#else
    .color_space = LCD_RGB_ELEMENT_ORDER_BGR,
    .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
#endif
    .bits_per_pixel = 16,
};
static spi_bus_config_t spi_config = ST7735_PANEL_BUS_SPI_CONFIG(
    PIN_NUM_CLK, PIN_NUM_MOSI, LCD_PIXEL_WIDTH * LCD_PIXEL_HEIGHT * sizeof(uint16_t));
static esp_lcd_panel_io_spi_config_t io_config = ST7735_PANEL_IO_SPI_CONFIG(
    PIN_NUM_CS, PIN_NUM_DC, nullptr, nullptr);

static CRGB leds[1];
static portMUX_TYPE event_mux = portMUX_INITIALIZER_UNLOCKED;

static ProbeEvent event_queue[EVENT_QUEUE_SIZE] = {};
static volatile uint8_t event_head = 0;
static volatile uint8_t event_tail = 0;

static bool wifi_init_done = false;
static bool monitoring_ready = false;
static bool monitoring_failed = false;
static bool sd_ready = false;
static bool log_ready = false;
static bool pcap_ready = false;
static bool fallback_to_all = false;
static bool mode_switch_pending = false;

static OperatingMode selected_mode = MODE_TARGETS;
static OperatingMode effective_mode = MODE_ALL;
static OperatingMode pending_mode = MODE_TARGETS;
static uint8_t current_channel = CHANNEL_MIN;
static uint32_t wifi_init_due_ms = WIFI_INIT_DELAY_MS;
static uint32_t last_channel_hop_ms = 0;
static uint32_t last_button_ms = 0;
static uint32_t pending_mode_apply_ms = 0;
static uint32_t last_ui_update_ms = 0;
static uint32_t last_log_flush_ms = 0;
static uint32_t dropped_events = 0;
static uint32_t captured_events = 0;
static uint32_t last_event_ms = 0;
static uint32_t targeted_probe_count = 0;
static uint32_t all_probe_count = 0;
static uint32_t capture_eapol_count = 0;
static uint32_t deauth_frame_count = 0;
static uint8_t deauth_recent_burst_count = 0;
static uint32_t deauth_recent_last_ms = 0;
static uint8_t capture_channel_cursor = 0;
static uint32_t last_eapol_seen_ms = 0;

static char session_id[16] = "";
static char session_dir_path[96] = "";
static char log_file_path[160] = "";
static char pcap_file_path[160] = "";
static char target_essids[MAX_TARGETS][MAX_SSID_LEN + 1] = {};
static uint8_t target_count = 0;
static ProbeEvent last_event = {};
static ProbeEvent deauth_tracking_event = {};
static CaptureObservation capture_observations[MAX_CAPTURE_OBSERVATIONS] = {};
static OperatingMode active_log_mode = MODE_PAUSE;
static OperatingMode active_monitor_mode = MODE_PAUSE;
static uint32_t session_start_ms = 0;
static uint32_t deauth_log_window_start_ms = 0;
static uint8_t deauth_log_window_entries = 0;
static uint32_t last_logged_deauth_ms = 0;

static lv_obj_t *top_bar = nullptr;
static lv_obj_t *mode_label = nullptr;
static lv_obj_t *channel_label = nullptr;
static lv_obj_t *headline_label = nullptr;
static lv_obj_t *count_label = nullptr;
static lv_obj_t *footer_label = nullptr;

static char mode_text_cache[24] = "";
static char channel_text_cache[16] = "";
static char headline_text_cache[32] = "";
static char count_text_cache[32] = "";
static char footer_text_cache[64] = "";

static inline bool mac_equals(const uint8_t *left, const uint8_t *right)
{
    return memcmp(left, right, 6) == 0;
}

static inline bool mac_is_broadcast(const uint8_t *mac)
{
    static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    return mac_equals(mac, broadcast);
}

static void format_mac(char *buffer, size_t buffer_len, const uint8_t *mac)
{
    snprintf(buffer, buffer_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static AlertKind classify_deauth(const uint8_t *destination, const uint8_t *source, const uint8_t *bssid)
{
    if (mac_equals(source, bssid)) {
        return ALERT_CLIENT_TARGETED;
    }
    if (mac_equals(destination, bssid)) {
        return ALERT_AP_TARGETED;
    }
    if (mac_is_broadcast(destination)) {
        return ALERT_CLIENT_TARGETED;
    }
    return ALERT_UNKNOWN;
}

static bool same_deauth_signature(const ProbeEvent &left, const ProbeEvent &right)
{
    return left.kind == FRAME_DEAUTH &&
           right.kind == FRAME_DEAUTH &&
           left.reason == right.reason &&
           left.channel == right.channel &&
           left.destination_is_broadcast == right.destination_is_broadcast &&
           mac_equals(left.client, right.client) &&
           mac_equals(left.destination, right.destination) &&
           mac_equals(left.bssid, right.bssid);
}

static bool should_enqueue_deauth_event(const ProbeEvent &event)
{
    if (deauth_log_window_start_ms == 0 ||
        (event.uptime_ms - deauth_log_window_start_ms) > DEAUTH_LOG_WINDOW_MS) {
        deauth_log_window_start_ms = event.uptime_ms;
        deauth_log_window_entries = 0;
        last_logged_deauth_ms = 0;
    }

    if (deauth_log_window_entries >= DEAUTH_LOG_FRAMES_PER_WINDOW ||
        event.uptime_ms == last_logged_deauth_ms) {
        return false;
    }

    last_logged_deauth_ms = event.uptime_ms;
    deauth_log_window_entries++;
    return true;
}

static bool is_deauth_alert_active(uint32_t now_ms)
{
    return effective_mode == MODE_DEAUTH &&
           last_event.kind == FRAME_DEAUTH &&
           last_event.burst_count >= ALERT_THRESHOLD &&
           (now_ms - last_event_ms) <= (ALERT_TIMEOUT_MS + ALERT_EXTRA_HOLD_MS);
}

static uint32_t lv_tick_get_callback(void)
{
    return millis();
}

static void display_flush(lv_display_t *display, const lv_area_t *area, uint8_t *color_p)
{
    const size_t area_size = lv_area_get_size(area);
    lv_draw_sw_rgb565_swap(color_p, area_size);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                              reinterpret_cast<uint16_t *>(color_p));
    lv_display_flush_ready(display);
}

static void set_label_text_if_changed(lv_obj_t *label, char *cache, size_t cache_size, const char *text)
{
    if (strncmp(cache, text, cache_size) == 0) {
        return;
    }

    strncpy(cache, text, cache_size - 1);
    cache[cache_size - 1] = '\0';
    lv_label_set_text(label, cache);
}

static void set_led(const CRGB &color)
{
    leds[0] = color;
    FastLED.show();
}

static const CRGB &mode_led_color(OperatingMode mode)
{
    static const CRGB targets_color(0, 145, 110);
    static const CRGB all_color(0, 72, 200);
    static const CRGB capture_color(220, 0, 80);
    static const CRGB deauth_color(0, 170, 40);
    static const CRGB pause_color(255, 110, 0);
    static const CRGB fail_color(90, 0, 90);

    if (monitoring_failed) {
        return fail_color;
    }
    switch (mode) {
    case MODE_TARGETS:
        return targets_color;
    case MODE_ALL:
        return all_color;
    case MODE_CAPTURE:
        return capture_color;
    case MODE_DEAUTH:
        return deauth_color;
    case MODE_PAUSE:
    default:
        return pause_color;
    }
}

static const char *selected_mode_name(OperatingMode mode)
{
    switch (mode) {
    case MODE_TARGETS:
        return "TARGETS";
    case MODE_ALL:
        return "ALL";
    case MODE_CAPTURE:
        return "CAPTURE";
    case MODE_DEAUTH:
        return "DEAUTH";
    case MODE_PAUSE:
    default:
        return "STANDBY";
    }
}

static const char *display_mode_name(OperatingMode mode)
{
    switch (mode) {
    case MODE_TARGETS:
        return "SCOPE PROBES";
    case MODE_ALL:
        return "ALL PROBES";
    case MODE_CAPTURE:
        return "EAPOL STEAL";
    case MODE_DEAUTH:
        return "DEAUTH SCAN";
    case MODE_PAUSE:
    default:
        return "STANDBY";
    }
}

static const char *effective_mode_name()
{
    return display_mode_name(effective_mode);
}

static const char *pending_mode_name()
{
    return display_mode_name(mode_switch_pending ? pending_mode : effective_mode);
}

static uint32_t current_mode_count()
{
    switch (effective_mode) {
    case MODE_TARGETS:
        return targeted_probe_count;
    case MODE_ALL:
        return all_probe_count;
    case MODE_CAPTURE:
        return capture_eapol_count;
    case MODE_DEAUTH:
        return deauth_frame_count;
    case MODE_PAUSE:
    default:
        return 0;
    }
}

static void log_line(const char *fmt, ...)
{
    char message[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    Serial.print(message);

    if (log_ready && log_file_path[0] != '\0') {
        File file = SD_MMC.open(log_file_path, FILE_APPEND);
        if (!file) {
            Serial.printf("SD logging disabled: failed to append log file %s\n", log_file_path);
            log_ready = false;
            return;
        }
        if (file.print(message) == 0) {
            Serial.println("SD logging disabled: log append failure");
            log_ready = false;
        }
        file.flush();
        file.close();
    }
}

static void flush_logs(uint32_t now_ms)
{
    if ((now_ms - last_log_flush_ms) < LOG_FLUSH_MS) {
        return;
    }

    last_log_flush_ms = now_ms;
}

static void close_logs()
{
    log_ready = false;
    pcap_ready = false;
    log_file_path[0] = '\0';
    pcap_file_path[0] = '\0';
}

static const char *mode_directory_name(OperatingMode mode)
{
    switch (mode) {
    case MODE_TARGETS:
        return "scope_probes";
    case MODE_ALL:
        return "all_probes";
    case MODE_CAPTURE:
        return "capture";
    case MODE_DEAUTH:
        return "deauth";
    case MODE_PAUSE:
    default:
        return "standby";
    }
}

static bool mode_uses_pcap(OperatingMode mode)
{
    return mode == MODE_TARGETS || mode == MODE_ALL || mode == MODE_CAPTURE || mode == MODE_DEAUTH;
}

static void ensure_mode_log_files(OperatingMode mode, uint32_t timestamp_ms, bool force_reopen)
{
    if (!sd_ready || session_dir_path[0] == '\0') {
        return;
    }

    if (!force_reopen && log_ready && active_log_mode == mode) {
        return;
    }

    close_logs();

    const char *mode_dir = mode_directory_name(mode);
    char mode_dir_path[128];
    snprintf(mode_dir_path, sizeof(mode_dir_path), "%s/%s", session_dir_path, mode_dir);
    SD_MMC.mkdir(mode_dir_path);

    snprintf(log_file_path, sizeof(log_file_path), "%s/pocket_wifi_spy_%010lu_%s.txt",
             mode_dir_path,
             static_cast<unsigned long>(timestamp_ms),
             mode_dir);

    File log_create_file = SD_MMC.open(log_file_path, FILE_WRITE);
    if (log_create_file) {
        char header_line[256];
        snprintf(header_line, sizeof(header_line),
                 "[%010lu ms] LOG OPEN session=%s mode=%s dir=%s pcap=%u\n",
                 static_cast<unsigned long>(timestamp_ms),
                 session_id,
                 selected_mode_name(mode),
                 mode_dir,
                 mode_uses_pcap(mode) ? 1U : 0U);
        log_create_file.print(header_line);
        log_create_file.flush();
        log_create_file.close();
        log_ready = true;
    } else {
        Serial.printf("SD logging disabled: failed to create log file %s\n", log_file_path);
        log_file_path[0] = '\0';
    }

    if (mode_uses_pcap(mode)) {
        snprintf(pcap_file_path, sizeof(pcap_file_path), "%s/pocket_wifi_spy_%010lu_%s.pcap",
                 mode_dir_path,
                 static_cast<unsigned long>(timestamp_ms),
                 mode_dir);

        File pcap_create_file = SD_MMC.open(pcap_file_path, FILE_WRITE);
        if (pcap_create_file) {
            pcap_create_file.flush();
            pcap_create_file.close();
            pcap_ready = true;
            write_pcap_header();
        } else {
            Serial.printf("PCAP logging disabled: failed to create pcap file %s\n", pcap_file_path);
            pcap_file_path[0] = '\0';
        }
    }

    active_log_mode = mode;
}

static void write_pcap_header()
{
    if (!pcap_ready || pcap_file_path[0] == '\0') {
        return;
    }

    PcapGlobalHeader header = {
        0xa1b2c3d4,
        2,
        4,
        0,
        0,
        MAX_FRAME_LEN,
        PCAP_LINKTYPE_IEEE802_11,
    };

    File file = SD_MMC.open(pcap_file_path, FILE_APPEND);
    if (!file) {
        Serial.printf("PCAP logging disabled: failed to append pcap file %s\n", pcap_file_path);
        pcap_ready = false;
        return;
    }

    if (file.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header)) != sizeof(header)) {
        Serial.println("PCAP logging disabled: failed to write header");
        pcap_ready = false;
    }
    file.flush();
    file.close();
}

static bool mount_sd_card()
{
    SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN, SD_MMC_D1_PIN, SD_MMC_D2_PIN, SD_MMC_D3_PIN);
    if (!SD_MMC.begin()) {
        Serial.println("SD disabled: card mount failed");
        return false;
    }

    if (SD_MMC.cardType() == CARD_NONE) {
        Serial.println("SD disabled: no card attached");
        return false;
    }

    sd_ready = true;
    SD_MMC.mkdir("/logs");
    return true;
}

static void create_session_files()
{
    session_start_ms = millis();
    const uint32_t raw_session = esp_random();
    snprintf(session_id, sizeof(session_id), "%08lX", static_cast<unsigned long>(raw_session));

    if (!sd_ready) {
        return;
    }

    snprintf(session_dir_path, sizeof(session_dir_path), "/logs/pocket_wifi_spy_session_%010lu_%s",
             static_cast<unsigned long>(session_start_ms),
             session_id);
    SD_MMC.mkdir(session_dir_path);
}

static bool load_targets_from_sd()
{
    target_count = 0;
    fallback_to_all = false;

    if (!sd_ready) {
        fallback_to_all = true;
        return false;
    }

    File file = SD_MMC.open("/targets.txt", FILE_READ);
    if (!file) {
        fallback_to_all = true;
        return false;
    }

    while (file.available() && target_count < MAX_TARGETS) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) {
            continue;
        }
        if (line.length() > MAX_SSID_LEN) {
            line = line.substring(0, MAX_SSID_LEN);
        }
        line.toCharArray(target_essids[target_count], MAX_SSID_LEN + 1);
        target_count++;
    }

    file.close();

    if (target_count == 0) {
        fallback_to_all = true;
        return false;
    }

    return true;
}

static bool match_target_ssid(const char *ssid)
{
    for (uint8_t index = 0; index < target_count; ++index) {
        if (strcmp(target_essids[index], ssid) == 0) {
            return true;
        }
    }
    return false;
}

static void reset_capture_observations()
{
    memset(capture_observations, 0, sizeof(capture_observations));
    capture_channel_cursor = 0;
}

static bool is_capture_discovery_subtype(uint8_t frame_subtype)
{
    return frame_subtype == 0x05 || frame_subtype == 0x08;
}

static uint8_t extract_ap_channel(const uint8_t *ies, size_t ies_len, uint8_t fallback_channel)
{
    uint8_t ht_primary_channel = 0;
    size_t offset = 0;

    while (offset + 2 <= ies_len) {
        const uint8_t tag = ies[offset];
        const uint8_t len = ies[offset + 1];
        offset += 2;

        if (offset + len > ies_len) {
            break;
        }

        if (tag == 3 && len >= 1) {
            const uint8_t ds_channel = ies[offset];
            if (ds_channel >= CHANNEL_MIN && ds_channel <= CHANNEL_MAX) {
                return ds_channel;
            }
        }

        if (tag == 61 && len >= 1) {
            const uint8_t primary_channel = ies[offset];
            if (primary_channel >= CHANNEL_MIN && primary_channel <= CHANNEL_MAX) {
                ht_primary_channel = primary_channel;
            }
        }

        offset += len;
    }

    if (ht_primary_channel >= CHANNEL_MIN && ht_primary_channel <= CHANNEL_MAX) {
        return ht_primary_channel;
    }

    if (fallback_channel >= CHANNEL_MIN && fallback_channel <= CHANNEL_MAX) {
        return fallback_channel;
    }

    return CHANNEL_MIN;
}

static void update_capture_observation(const char *ssid, const uint8_t *bssid, uint8_t channel, uint32_t now_ms)
{
    if (channel < CHANNEL_MIN || channel > CHANNEL_MAX) {
        return;
    }

    int free_index = -1;
    for (uint8_t index = 0; index < MAX_CAPTURE_OBSERVATIONS; ++index) {
        if (!capture_observations[index].valid) {
            if (free_index < 0) {
                free_index = index;
            }
            continue;
        }

        if (strcmp(capture_observations[index].ssid, ssid) == 0 &&
            mac_equals(capture_observations[index].bssid, bssid)) {
            capture_observations[index].channel = channel;
            capture_observations[index].last_seen_ms = now_ms;
            return;
        }
    }

    if (free_index < 0) {
        uint8_t oldest_index = 0;
        uint32_t oldest_seen_ms = capture_observations[0].last_seen_ms;
        for (uint8_t index = 1; index < MAX_CAPTURE_OBSERVATIONS; ++index) {
            if (capture_observations[index].last_seen_ms < oldest_seen_ms) {
                oldest_seen_ms = capture_observations[index].last_seen_ms;
                oldest_index = index;
            }
        }
        free_index = oldest_index;
    }

    capture_observations[free_index].valid = true;
    strncpy(capture_observations[free_index].ssid, ssid, sizeof(capture_observations[free_index].ssid) - 1);
    capture_observations[free_index].ssid[sizeof(capture_observations[free_index].ssid) - 1] = '\0';
    memcpy(capture_observations[free_index].bssid, bssid, 6);
    capture_observations[free_index].channel = channel;
    capture_observations[free_index].last_seen_ms = now_ms;
}

static void learn_capture_target(const uint8_t *payload, uint16_t frame_len, uint8_t frame_subtype, uint8_t channel, uint32_t now_ms)
{
    if (target_count == 0 || !is_capture_discovery_subtype(frame_subtype) || frame_len < 36) {
        return;
    }

    const uint8_t advertised_channel = extract_ap_channel(payload + 36, frame_len - 36, channel);
    char ssid[MAX_SSID_LEN + 1];
    bool wildcard = false;
    extract_ssid(payload + 36, frame_len - 36, ssid, sizeof(ssid), &wildcard);
    if (wildcard || !match_target_ssid(ssid)) {
        return;
    }

    update_capture_observation(ssid, payload + 16, advertised_channel, now_ms);
}

static uint8_t collect_capture_channels(uint32_t now_ms, uint8_t *channels, uint8_t max_channels)
{
    bool present[CHANNEL_MAX + 1] = {};
    uint8_t count = 0;

    for (uint8_t index = 0; index < MAX_CAPTURE_OBSERVATIONS; ++index) {
        const CaptureObservation &observation = capture_observations[index];
        if (!observation.valid) {
            continue;
        }
        if ((now_ms - observation.last_seen_ms) > CAPTURE_TARGET_TTL_MS) {
            continue;
        }
        if (observation.channel < CHANNEL_MIN || observation.channel > CHANNEL_MAX) {
            continue;
        }
        if (present[observation.channel]) {
            continue;
        }

        present[observation.channel] = true;
        if (count < max_channels) {
            channels[count++] = observation.channel;
        }
    }

    return count;
}

static bool capture_channel_is_targeted(uint8_t channel, uint32_t now_ms)
{
    uint8_t channels[CHANNEL_MAX] = {};
    const uint8_t channel_count = collect_capture_channels(now_ms, channels, CHANNEL_MAX);
    for (uint8_t index = 0; index < channel_count; ++index) {
        if (channels[index] == channel) {
            return true;
        }
    }
    return false;
}

static bool capture_bssid_is_targeted(const uint8_t *mac, uint32_t now_ms)
{
    for (uint8_t index = 0; index < MAX_CAPTURE_OBSERVATIONS; ++index) {
        const CaptureObservation &observation = capture_observations[index];
        if (!observation.valid) {
            continue;
        }
        if ((now_ms - observation.last_seen_ms) > CAPTURE_TARGET_TTL_MS) {
            continue;
        }
        if (mac_equals(observation.bssid, mac)) {
            return true;
        }
    }
    return false;
}

static bool capture_addresses_match_target(const ProbeEvent &event)
{
    return capture_bssid_is_targeted(event.destination, event.uptime_ms) ||
           capture_bssid_is_targeted(event.client, event.uptime_ms) ||
           capture_bssid_is_targeted(event.bssid, event.uptime_ms);
}

static bool should_capture_eapol_frame(const ProbeEvent &event)
{
    if (target_count == 0) {
        return false;
    }

    return capture_addresses_match_target(event) ||
           capture_channel_is_targeted(current_channel, event.uptime_ms) ||
           capture_channel_is_targeted(event.channel, event.uptime_ms);
}

static bool capture_mode_available()
{
    return target_count > 0;
}

static OperatingMode compute_effective_mode()
{
    fallback_to_all = false;

    if (selected_mode == MODE_PAUSE) {
        return MODE_PAUSE;
    }

    if (selected_mode == MODE_ALL || selected_mode == MODE_CAPTURE || selected_mode == MODE_DEAUTH) {
        return selected_mode;
    }

    if (target_count == 0) {
        fallback_to_all = true;
        return MODE_ALL;
    }

    return MODE_TARGETS;
}

static OperatingMode compute_monitoring_mode()
{
    if (selected_mode == MODE_CAPTURE || selected_mode == MODE_DEAUTH) {
        return selected_mode;
    }
    return compute_effective_mode();
}

static bool apply_promiscuous_filter_for_mode(OperatingMode mode)
{
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT,
    };

    if (mode == MODE_CAPTURE) {
        uint32_t capture_mask = 0xFFFFFFFFu;
        filter.filter_mask = capture_mask;
    }

    esp_err_t result = esp_wifi_set_promiscuous_filter(&filter);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_promiscuous_filter failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return false;
    }

    return true;
}

static bool configure_monitoring(bool enable)
{
    esp_err_t result = esp_wifi_set_promiscuous(enable);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_promiscuous(%d) failed: %s\n", enable, esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return false;
    }

    monitoring_failed = false;
    monitoring_ready = enable;
    return true;
}

static bool reconfigure_monitoring_for_mode(OperatingMode mode)
{
    if (!configure_monitoring(false)) {
        return false;
    }
    if (!apply_promiscuous_filter_for_mode(mode)) {
        return false;
    }
    if (mode == MODE_PAUSE) {
        return true;
    }
    return configure_monitoring(true);
}

static bool mode_requires_monitor_restart(OperatingMode previous_mode, OperatingMode next_mode)
{
    if (previous_mode == next_mode) {
        return false;
    }

    return previous_mode == MODE_CAPTURE || next_mode == MODE_CAPTURE ||
           previous_mode == MODE_DEAUTH || next_mode == MODE_DEAUTH;
}

static bool restart_monitoring_stack_for_mode(OperatingMode mode)
{
    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char *>(wifi_config.ap.ssid), "PocketSpy", sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen("PocketSpy");
    wifi_config.ap.channel = current_channel;
    strncpy(reinterpret_cast<char *>(wifi_config.ap.password), "pocketspy", sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.ssid_hidden = 1;
    wifi_config.ap.max_connection = 1;
    wifi_config.ap.beacon_interval = 100;

    esp_err_t result = esp_wifi_set_promiscuous(false);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_promiscuous(false) failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return false;
    }

    result = esp_wifi_stop();
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_stop failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return false;
    }

    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_storage failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return false;
    }

    result = esp_wifi_set_mode(WIFI_MODE_AP);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_mode(AP) failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return false;
    }

    result = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_config(AP) failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return false;
    }

    result = esp_wifi_start();
    if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) {
        Serial.printf("esp_wifi_start failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return false;
    }

    result = esp_wifi_set_promiscuous(false);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_promiscuous(false) failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return false;
    }

    if (!apply_promiscuous_filter_for_mode(mode)) {
        return false;
    }

    result = esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_callback);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_promiscuous_rx_cb failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return false;
    }

    result = esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_channel failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return false;
    }

    if (!configure_monitoring(mode != MODE_PAUSE)) {
        return false;
    }

    active_monitor_mode = mode;
    return true;
}

static void apply_selected_mode(bool emit_log)
{
    const uint32_t now_ms = millis();

    if (selected_mode == MODE_TARGETS || selected_mode == MODE_CAPTURE) {
        load_targets_from_sd();
    }

    if (selected_mode == MODE_CAPTURE && !capture_mode_available()) {
        selected_mode = MODE_DEAUTH;
    }

    if (selected_mode != MODE_DEAUTH) {
        deauth_tracking_event = {};
        deauth_log_window_start_ms = 0;
        deauth_log_window_entries = 0;
        last_logged_deauth_ms = 0;
    }
    if (selected_mode != MODE_CAPTURE) {
        reset_capture_observations();
        last_eapol_seen_ms = 0;
    }

    effective_mode = compute_monitoring_mode();

    ensure_mode_log_files(effective_mode, now_ms, active_log_mode != effective_mode);

    if (wifi_init_done) {
        const bool restart_required = mode_requires_monitor_restart(active_monitor_mode, effective_mode);
        const bool reconfigured = restart_required ?
            restart_monitoring_stack_for_mode(effective_mode) :
            reconfigure_monitoring_for_mode(effective_mode);
        if (!reconfigured) {
            return;
        }
        active_monitor_mode = effective_mode;
    }

    if (emit_log) {
        log_line("[%010lu ms] MODE selected=%s effective=%s targets=%u fallback=%u\n",
                 static_cast<unsigned long>(now_ms),
                 selected_mode_name(selected_mode),
                 selected_mode_name(effective_mode),
                 static_cast<unsigned>(target_count),
                 fallback_to_all ? 1U : 0U);
    }
}

static OperatingMode next_mode_after(OperatingMode mode)
{
    if (mode == MODE_TARGETS) {
        return MODE_ALL;
    }
    if (mode == MODE_ALL) {
        return capture_mode_available() ? MODE_CAPTURE : MODE_DEAUTH;
    }
    if (mode == MODE_CAPTURE) {
        return MODE_DEAUTH;
    }
    if (mode == MODE_DEAUTH) {
        return MODE_PAUSE;
    }
    return MODE_TARGETS;
}

static void queue_mode_switch(OperatingMode mode, uint32_t now_ms)
{
    pending_mode = mode;
    pending_mode_apply_ms = now_ms + MODE_SWITCH_DELAY_MS;
    mode_switch_pending = true;

    if (wifi_init_done && monitoring_ready) {
        if (!configure_monitoring(false)) {
            return;
        }
    }

    log_line("[%010lu ms] MODE queued=%s apply_at=%lu\n",
             static_cast<unsigned long>(now_ms),
             selected_mode_name(pending_mode),
             static_cast<unsigned long>(pending_mode_apply_ms));
}

static void service_mode_switch(uint32_t now_ms)
{
    if (!mode_switch_pending || now_ms < pending_mode_apply_ms) {
        return;
    }

    selected_mode = pending_mode;
    mode_switch_pending = false;
    apply_selected_mode(true);
}

static bool is_eapol_frame(const uint8_t *payload, uint16_t frame_len, uint16_t frame_control, uint8_t frame_subtype)
{
    (void)frame_subtype;
    return find_eapol_llc_offset(payload, frame_len, frame_control) >= 0;
}

static int find_eapol_llc_offset(const uint8_t *payload, uint16_t frame_len, uint16_t frame_control)
{
    static const uint8_t eapol_llc[] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};

    if (frame_len < (24 + sizeof(eapol_llc) + 4)) {
        return -1;
    }

    if (frame_len >= 32 && memcmp(payload + 24, eapol_llc, sizeof(eapol_llc)) == 0) {
        return 24;
    }
    if (frame_len >= 34 && memcmp(payload + 26, eapol_llc, sizeof(eapol_llc)) == 0) {
        return 26;
    }

    int header_len = 24;
    if ((frame_control & 0x0300U) == 0x0300U) {
        header_len += 6;
    }
    if ((payload[0] & 0x0F) == 0x08) {
        header_len += 2;
    }
    if ((frame_control & 0x8000U) != 0) {
        header_len += 4;
    }

    if (frame_len >= static_cast<uint16_t>(header_len + sizeof(eapol_llc)) &&
        memcmp(payload + header_len, eapol_llc, sizeof(eapol_llc)) == 0) {
        return header_len;
    }

    const int search_end = static_cast<int>(frame_len) - static_cast<int>(sizeof(eapol_llc));
    for (int offset = 24; offset <= search_end && offset <= 40; ++offset) {
        if (memcmp(payload + offset, eapol_llc, sizeof(eapol_llc)) == 0) {
            return offset;
        }
    }

    return -1;
}

static uint8_t classify_eapol_message(const uint8_t *payload, uint16_t frame_len, uint16_t frame_control)
{
    const int llc_offset = find_eapol_llc_offset(payload, frame_len, frame_control);
    if (llc_offset < 0) {
        return 0;
    }

    const int key_info_offset = llc_offset + 8 + 4 + 1;
    if (frame_len < (key_info_offset + 2)) {
        return 0;
    }

    const uint16_t key_info = (static_cast<uint16_t>(payload[key_info_offset]) << 8) |
                              static_cast<uint16_t>(payload[key_info_offset + 1]);
    const bool install = (key_info & (1 << 6)) != 0;
    const bool ack = (key_info & (1 << 7)) != 0;
    const bool mic = (key_info & (1 << 8)) != 0;
    const bool secure = (key_info & (1 << 9)) != 0;

    if (ack && !mic && !install) return 1;
    if (!ack && mic && !install && !secure) return 2;
    if (ack && mic && install) return 3;
    if (!ack && mic && !install && secure) return 4;
    return 0;
}

static bool extract_ssid(const uint8_t *ies, size_t ies_len, char *ssid_buffer, size_t buffer_size, bool *wildcard)
{
    size_t offset = 0;

    while (offset + 2 <= ies_len) {
        const uint8_t tag = ies[offset];
        const uint8_t len = ies[offset + 1];
        offset += 2;

        if (offset + len > ies_len) {
            break;
        }

        if (tag == 0) {
            const size_t copy_len = len < (buffer_size - 1) ? len : (buffer_size - 1);
            memcpy(ssid_buffer, ies + offset, copy_len);
            ssid_buffer[copy_len] = '\0';
            for (size_t index = 0; index < copy_len; ++index) {
                if (!isprint(static_cast<unsigned char>(ssid_buffer[index]))) {
                    ssid_buffer[index] = '.';
                }
            }
            *wildcard = (len == 0);
            return true;
        }

        offset += len;
    }

    strncpy(ssid_buffer, "<missing>", buffer_size - 1);
    ssid_buffer[buffer_size - 1] = '\0';
    *wildcard = false;
    return false;
}

static bool enqueue_event(const ProbeEvent &event)
{
    bool queued = false;

    portENTER_CRITICAL(&event_mux);
    const uint8_t next_head = static_cast<uint8_t>((event_head + 1) % EVENT_QUEUE_SIZE);
    if (next_head == event_tail) {
        dropped_events++;
    } else {
        event_queue[event_head] = event;
        event_head = next_head;
        queued = true;
    }
    portEXIT_CRITICAL(&event_mux);

    return queued;
}

static bool dequeue_event(ProbeEvent *event)
{
    bool has_event = false;

    portENTER_CRITICAL(&event_mux);
    if (event_tail != event_head) {
        *event = event_queue[event_tail];
        event_tail = static_cast<uint8_t>((event_tail + 1) % EVENT_QUEUE_SIZE);
        has_event = true;
    }
    portEXIT_CRITICAL(&event_mux);

    return has_event;
}

static void wifi_sniffer_callback(void *buffer, wifi_promiscuous_pkt_type_t packet_type)
{
    if (effective_mode == MODE_PAUSE) {
        return;
    }

    const wifi_promiscuous_pkt_t *packet = static_cast<const wifi_promiscuous_pkt_t *>(buffer);
    if (packet->rx_ctrl.sig_len < 24) {
        return;
    }

    const uint8_t *payload = packet->payload;
    const uint16_t frame_control = static_cast<uint16_t>(payload[0]) |
                                   (static_cast<uint16_t>(payload[1]) << 8);
    const uint8_t frame_type = (frame_control >> 2) & 0x3;
    const uint8_t frame_subtype = (frame_control >> 4) & 0xf;

    if (effective_mode == MODE_CAPTURE) {
        const uint32_t now_ms = millis();
        if (packet_type == WIFI_PKT_MGMT && frame_type == 0 && is_capture_discovery_subtype(frame_subtype)) {
            learn_capture_target(payload, packet->rx_ctrl.sig_len, frame_subtype, packet->rx_ctrl.channel, now_ms);
        }

        const int llc_offset = find_eapol_llc_offset(payload, packet->rx_ctrl.sig_len, frame_control);
        if (llc_offset < 0) {
            return;
        }

        ProbeEvent event = {};
        event.uptime_ms = now_ms;
        event.original_len = packet->rx_ctrl.sig_len;
        event.capture_len = packet->rx_ctrl.sig_len > MAX_FRAME_LEN ? MAX_FRAME_LEN : packet->rx_ctrl.sig_len;
        event.rssi = packet->rx_ctrl.rssi;
        event.channel = packet->rx_ctrl.channel;
        event.kind = FRAME_EAPOL;
        event.eapol_message = classify_eapol_message(payload, packet->rx_ctrl.sig_len, frame_control);
        memcpy(event.destination, payload + 4, 6);
        memcpy(event.client, payload + 10, 6);
        memcpy(event.bssid, payload + 16, 6);
        strncpy(event.ssid, "EAPOL", sizeof(event.ssid) - 1);
        if (!should_capture_eapol_frame(event)) {
            return;
        }
        last_eapol_seen_ms = event.uptime_ms;
        memcpy(event.frame, payload, event.capture_len);
        enqueue_event(event);
        return;
    }

    if (effective_mode == MODE_DEAUTH) {
        if (packet_type != WIFI_PKT_MGMT || frame_type != 0 || frame_subtype != 0x0c || packet->rx_ctrl.sig_len < 26) {
            return;
        }

        ProbeEvent event = {};
        event.uptime_ms = millis();
        event.original_len = packet->rx_ctrl.sig_len;
        event.capture_len = packet->rx_ctrl.sig_len > MAX_FRAME_LEN ? MAX_FRAME_LEN : packet->rx_ctrl.sig_len;
        event.rssi = packet->rx_ctrl.rssi;
        event.channel = packet->rx_ctrl.channel;
        event.kind = FRAME_DEAUTH;
        memcpy(event.destination, payload + 4, 6);
        memcpy(event.client, payload + 10, 6);
        memcpy(event.bssid, payload + 16, 6);
        event.reason = static_cast<uint16_t>(payload[24]) |
                       (static_cast<uint16_t>(payload[25]) << 8);
        event.alert_kind = classify_deauth(event.destination, event.client, event.bssid);
        event.destination_is_broadcast = mac_is_broadcast(event.destination);

        if (deauth_tracking_event.kind == FRAME_DEAUTH &&
            same_deauth_signature(event, deauth_tracking_event) &&
            (event.uptime_ms - deauth_tracking_event.uptime_ms) <= BURST_WINDOW_MS) {
            event.burst_count = static_cast<uint8_t>(min<uint16_t>(deauth_tracking_event.burst_count + 1, 255));
        } else {
            event.burst_count = 1;
        }

        deauth_tracking_event = event;

        if (!should_enqueue_deauth_event(event)) {
            return;
        }

        memcpy(event.frame, payload, event.capture_len);
        enqueue_event(event);
        return;
    }

    if (packet_type != WIFI_PKT_MGMT) {
        return;
    }

    if (frame_type != 0 || frame_subtype != 0x04) {
        return;
    }

    ProbeEvent event = {};
    event.uptime_ms = millis();
    event.original_len = packet->rx_ctrl.sig_len;
    event.capture_len = packet->rx_ctrl.sig_len > MAX_FRAME_LEN ? MAX_FRAME_LEN : packet->rx_ctrl.sig_len;
    event.rssi = packet->rx_ctrl.rssi;
    event.channel = packet->rx_ctrl.channel;
    event.kind = FRAME_PROBE_REQUEST;
    memcpy(event.destination, payload + 4, 6);
    memcpy(event.client, payload + 10, 6);
    memcpy(event.bssid, payload + 16, 6);

    extract_ssid(payload + 24, packet->rx_ctrl.sig_len - 24, event.ssid, sizeof(event.ssid), &event.wildcard_ssid);

    if (effective_mode == MODE_TARGETS) {
        if (event.wildcard_ssid || !match_target_ssid(event.ssid)) {
            return;
        }
    }

    memcpy(event.frame, payload, event.capture_len);
    enqueue_event(event);
}

static void init_wifi_monitor()
{
    if (wifi_init_done) {
        return;
    }

    wifi_init_done = true;
    WiFi.persistent(false);
    WiFi.mode(WIFI_MODE_AP);
    delay(150);

    apply_selected_mode(false);
    if (!restart_monitoring_stack_for_mode(effective_mode)) {
        return;
    }

    log_line("[%010lu ms] START session=%s selected=%s effective=%s targets=%u sd=%u log=%u pcap=%u\n",
             static_cast<unsigned long>(millis()),
             session_id,
             selected_mode_name(selected_mode),
             selected_mode_name(effective_mode),
             static_cast<unsigned>(target_count),
             sd_ready ? 1U : 0U,
             log_ready ? 1U : 0U,
             pcap_ready ? 1U : 0U);

    if (selected_mode == MODE_TARGETS && sd_ready && target_count == 0) {
        log_line("[%010lu ms] TARGETS unavailable, defaulting to ALL mode\n", static_cast<unsigned long>(millis()));
    } else if (selected_mode == MODE_CAPTURE && target_count == 0) {
        log_line("[%010lu ms] CAPTURE target discovery disabled: targets.txt missing or empty\n",
                 static_cast<unsigned long>(millis()));
    }

    if (log_ready) {
        log_line("[%010lu ms] FILES log=%s pcap=%s\n",
                 static_cast<unsigned long>(millis()),
                 log_file_path,
                 pcap_ready ? pcap_file_path : "<none>");
    }
}

static void write_pcap_event(const ProbeEvent &event)
{
    if (!pcap_ready || pcap_file_path[0] == '\0') {
        return;
    }

    PcapPacketHeader header = {
        event.uptime_ms / 1000,
        (event.uptime_ms % 1000) * 1000,
        event.capture_len,
        event.original_len,
    };

    File file = SD_MMC.open(pcap_file_path, FILE_APPEND);
    if (!file) {
        Serial.printf("PCAP logging disabled: failed to append pcap file %s\n", pcap_file_path);
        pcap_ready = false;
        return;
    }

    if (file.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header)) != sizeof(header) ||
        file.write(event.frame, event.capture_len) != event.capture_len) {
        Serial.println("PCAP logging disabled: write failure");
        pcap_ready = false;
    }
    file.flush();
    file.close();
}

static void process_event(const ProbeEvent &event)
{
    captured_events++;
    ProbeEvent display_event = event;

    if (event.kind == FRAME_EAPOL) {
        capture_eapol_count++;
    } else if (event.kind == FRAME_DEAUTH) {
        deauth_frame_count++;
        if (deauth_recent_last_ms != 0 && (event.uptime_ms - deauth_recent_last_ms) <= BURST_WINDOW_MS) {
            deauth_recent_burst_count = static_cast<uint8_t>(min<uint16_t>(deauth_recent_burst_count + 1, 255));
        } else {
            deauth_recent_burst_count = 1;
        }
        deauth_recent_last_ms = event.uptime_ms;
        if (display_event.burst_count < deauth_recent_burst_count) {
            display_event.burst_count = deauth_recent_burst_count;
        }
    } else if (effective_mode == MODE_TARGETS) {
        targeted_probe_count++;
    } else {
        all_probe_count++;
    }

    last_event = display_event;
    last_event_ms = display_event.uptime_ms;

    char client_mac[18];
    char bssid_mac[18];
    char destination_mac[18];
    char ssid_text[40];

    format_mac(client_mac, sizeof(client_mac), event.client);
    format_mac(bssid_mac, sizeof(bssid_mac), event.bssid);
    format_mac(destination_mac, sizeof(destination_mac), event.destination);

    if (event.wildcard_ssid) {
        strncpy(ssid_text, "<wildcard>", sizeof(ssid_text) - 1);
        ssid_text[sizeof(ssid_text) - 1] = '\0';
    } else {
        strncpy(ssid_text, event.ssid, sizeof(ssid_text) - 1);
        ssid_text[sizeof(ssid_text) - 1] = '\0';
    }

    if (event.kind == FRAME_EAPOL) {
        log_line("[%010lu ms] EAPOL msg=%u src=%s bssid=%s dst=%s ch=%u rssi=%d len=%u session=%s\n",
                 static_cast<unsigned long>(event.uptime_ms),
                 static_cast<unsigned>(event.eapol_message),
                 client_mac,
                 bssid_mac,
                 destination_mac,
                 static_cast<unsigned>(event.channel),
                 static_cast<int>(event.rssi),
                 static_cast<unsigned>(event.original_len),
                 session_id);
    } else if (event.kind == FRAME_DEAUTH) {
        log_line("[%010lu ms] DEAUTH src=%s bssid=%s dst=%s ch=%u reason=%u burst=%u rssi=%d len=%u session=%s\n",
                 static_cast<unsigned long>(event.uptime_ms),
                 client_mac,
                 bssid_mac,
                 destination_mac,
                 static_cast<unsigned>(event.channel),
                 static_cast<unsigned>(event.reason),
                 static_cast<unsigned>(display_event.burst_count),
                 static_cast<int>(event.rssi),
                 static_cast<unsigned>(event.original_len),
                 session_id);
    } else {
        log_line("[%010lu ms] PROBE client=%s ssid=%s bssid=%s dst=%s ch=%u rssi=%d len=%u session=%s\n",
                 static_cast<unsigned long>(event.uptime_ms),
                 client_mac,
                 ssid_text,
                 bssid_mac,
                 destination_mac,
                 static_cast<unsigned>(event.channel),
                 static_cast<int>(event.rssi),
                 static_cast<unsigned>(event.original_len),
                 session_id);
    }

    write_pcap_event(event);
}

static void service_event_queue()
{
    ProbeEvent event = {};
    while (dequeue_event(&event)) {
        process_event(event);
    }
}

static void poll_button(uint32_t now_ms)
{
    static bool last_pressed = false;
    const bool pressed = digitalRead(BOOT_PIN) == LOW;

    if (pressed && !last_pressed && (now_ms - last_button_ms) > BUTTON_DEBOUNCE_MS) {
        last_button_ms = now_ms;
        const OperatingMode base_mode = mode_switch_pending ? pending_mode : selected_mode;
        queue_mode_switch(next_mode_after(base_mode), now_ms);
    }

    last_pressed = pressed;
}

static void hop_channel(uint32_t now_ms)
{
    if (!monitoring_ready || effective_mode == MODE_PAUSE) {
        return;
    }

    uint32_t hop_interval_ms = CHANNEL_HOP_MS;
    if (effective_mode == MODE_CAPTURE) {
        if (last_eapol_seen_ms != 0 && (now_ms - last_eapol_seen_ms) < CAPTURE_EAPOL_LOCK_MS) {
            return;
        }

        uint8_t known_channels[CHANNEL_MAX] = {};
        const uint8_t known_channel_count = collect_capture_channels(now_ms, known_channels, CHANNEL_MAX);
        hop_interval_ms = known_channel_count > 0 ? CAPTURE_LOCK_HOP_MS : CAPTURE_DISCOVERY_HOP_MS;
        if ((now_ms - last_channel_hop_ms) < hop_interval_ms) {
            return;
        }

        if (known_channel_count == 1) {
            current_channel = known_channels[0];
        } else if (known_channel_count > 1) {
            current_channel = known_channels[capture_channel_cursor % known_channel_count];
            capture_channel_cursor = static_cast<uint8_t>((capture_channel_cursor + 1) % known_channel_count);
        } else {
            current_channel++;
            if (current_channel > CHANNEL_MAX) {
                current_channel = CHANNEL_MIN;
            }
        }
    } else {
        if ((now_ms - last_channel_hop_ms) < hop_interval_ms) {
            return;
        }

        current_channel++;
        if (current_channel > CHANNEL_MAX) {
            current_channel = CHANNEL_MIN;
        }
    }

    if (esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
        last_channel_hop_ms = now_ms;
    }
}

static void init_display()
{
    pinMode(PIN_NUM_BCKL, OUTPUT);
    digitalWrite(PIN_NUM_BCKL, HIGH);

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &spi_config, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    esp_lcd_panel_set_gap(panel_handle, 1, 26);
    esp_lcd_panel_swap_xy(panel_handle, true);
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    ledcAttach(PIN_NUM_BCKL, LEDC_BACKLIGHT_FREQ, LEDC_BACKLIGHT_BIT_WIDTH);
#else
    ledcSetup(LEDC_BACKLIGHT_CHANNEL, LEDC_BACKLIGHT_FREQ, LEDC_BACKLIGHT_BIT_WIDTH);
    ledcAttachPin(PIN_NUM_BCKL, LEDC_BACKLIGHT_CHANNEL);
#endif
    ledcWrite(LEDC_BACKLIGHT_CHANNEL, 0);

    lv_init();

    static lv_color16_t *draw_buffer = nullptr;
    static lv_color16_t *draw_buffer_secondary = nullptr;
    const size_t draw_buffer_size = LCD_PIXEL_WIDTH * LCD_PIXEL_HEIGHT * sizeof(lv_color16_t);
    draw_buffer = reinterpret_cast<lv_color16_t *>(malloc(draw_buffer_size));
    draw_buffer_secondary = reinterpret_cast<lv_color16_t *>(malloc(draw_buffer_size));
    assert(draw_buffer);
    assert(draw_buffer_secondary);

    lv_display_t *display = lv_display_create(LCD_PIXEL_WIDTH, LCD_PIXEL_HEIGHT);
    lv_display_set_buffers(display, draw_buffer, draw_buffer_secondary, draw_buffer_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, display_flush);
    lv_tick_set_cb(lv_tick_get_callback);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x050816), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    top_bar = lv_obj_create(screen);
    lv_obj_set_size(top_bar, 160, 14);
    lv_obj_set_pos(top_bar, 0, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_shadow_width(top_bar, 0, 0);
    lv_obj_set_style_outline_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_set_style_bg_color(top_bar, lv_color_hex(0x10323a), 0);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);

    mode_label = lv_label_create(top_bar);
    lv_obj_align(mode_label, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(mode_label, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(mode_label, LV_OPA_TRANSP, 0);

    channel_label = lv_label_create(top_bar);
    lv_obj_align(channel_label, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_text_font(channel_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(channel_label, lv_color_hex(0xcde7ff), 0);
    lv_obj_set_style_bg_opa(channel_label, LV_OPA_TRANSP, 0);

    headline_label = lv_label_create(screen);
    lv_obj_set_width(headline_label, 160);
    lv_obj_align(headline_label, LV_ALIGN_CENTER, 0, -10);
    lv_label_set_long_mode(headline_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(headline_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(headline_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(headline_label, lv_color_hex(0x96f5c5), 0);
    lv_obj_set_style_bg_opa(headline_label, LV_OPA_TRANSP, 0);

    count_label = lv_label_create(screen);
    lv_obj_set_pos(count_label, 5, 48);
    lv_obj_set_width(count_label, 150);
    lv_label_set_long_mode(count_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(count_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(count_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(count_label, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(count_label, LV_OPA_TRANSP, 0);

    footer_label = lv_label_create(screen);
    lv_obj_set_pos(footer_label, 5, 68);
    lv_obj_set_width(footer_label, 150);
    lv_label_set_long_mode(footer_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(footer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(footer_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(footer_label, lv_color_hex(0x7ee2ff), 0);
    lv_obj_set_style_bg_opa(footer_label, LV_OPA_TRANSP, 0);

    lv_timer_handler();
    delay(20);
}

static void update_ui(uint32_t now_ms)
{
    if ((now_ms - last_ui_update_ms) < UI_UPDATE_MS) {
        return;
    }

    last_ui_update_ms = now_ms;

    char mode_text[24];
    char channel_text[16];
    char headline_text[32];
    char count_text[32];
    char footer_text[64];
    const bool deauth_alert_active = is_deauth_alert_active(now_ms);

    snprintf(mode_text, sizeof(mode_text), "%s", pending_mode_name());
    if (mode_switch_pending) {
        snprintf(channel_text, sizeof(channel_text), "WAIT");
        count_text[0] = '\0';
        snprintf(footer_text, sizeof(footer_text), "STARTING IN %.1fs",
                 static_cast<double>(pending_mode_apply_ms > now_ms ? (pending_mode_apply_ms - now_ms) : 0U) / 1000.0);
        strncpy(headline_text, "LOADING...", sizeof(headline_text) - 1);
        headline_text[sizeof(headline_text) - 1] = '\0';
    } else {
        snprintf(channel_text, sizeof(channel_text), "CH: %02u", current_channel);
        snprintf(count_text, sizeof(count_text), "COUNT: %lu", static_cast<unsigned long>(current_mode_count()));
        snprintf(footer_text, sizeof(footer_text), "BUTTON: CHANGE MODE");
        strncpy(headline_text, mode_text, sizeof(headline_text) - 1);
        headline_text[sizeof(headline_text) - 1] = '\0';
    }

    if (!mode_switch_pending && effective_mode == MODE_DEAUTH && deauth_alert_active) {
        const char *alert_text = "DEAUTH SEEN";
        if (last_event.alert_kind == ALERT_AP_TARGETED) {
            alert_text = "AP TARGETED";
        } else if (last_event.alert_kind == ALERT_CLIENT_TARGETED) {
            alert_text = last_event.destination_is_broadcast ? "CLIENTS TARGETED" : "CLIENT TARGETED";
        }
        snprintf(headline_text, sizeof(headline_text), "ALERT: %s", alert_text);
        lv_obj_set_style_text_font(headline_label, &lv_font_montserrat_12, 0);
    } else if (!mode_switch_pending && effective_mode == MODE_DEAUTH) {
        strncpy(headline_text, "DEAUTH: CLEAR", sizeof(headline_text) - 1);
        headline_text[sizeof(headline_text) - 1] = '\0';
        lv_obj_set_style_text_font(headline_label, &lv_font_montserrat_14, 0);
    } else {
        lv_obj_set_style_text_font(headline_label, &lv_font_montserrat_18, 0);
    }

    set_label_text_if_changed(mode_label, mode_text_cache, sizeof(mode_text_cache), "wifi spy");
    set_label_text_if_changed(channel_label, channel_text_cache, sizeof(channel_text_cache), channel_text);
    set_label_text_if_changed(headline_label, headline_text_cache, sizeof(headline_text_cache), headline_text);
    if (mode_switch_pending || effective_mode == MODE_PAUSE) {
        set_label_text_if_changed(count_label, count_text_cache, sizeof(count_text_cache), "");
        lv_obj_add_flag(count_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(count_label, LV_OBJ_FLAG_HIDDEN);
        set_label_text_if_changed(count_label, count_text_cache, sizeof(count_text_cache), count_text);
    }
    set_label_text_if_changed(footer_label, footer_text_cache, sizeof(footer_text_cache), footer_text);

    if (mode_switch_pending) {
        if (((now_ms / 180U) % 2U) == 0U) {
            set_led(CRGB(24, 24, 24));
        } else {
            set_led(CRGB::Black);
        }
    } else if (effective_mode == MODE_DEAUTH && deauth_alert_active) {
        if (((now_ms / 180U) % 2U) == 0U) {
            set_led(CRGB::Red);
        } else {
            set_led(CRGB::Black);
        }
    } else {
        const CRGB &color = mode_led_color(effective_mode);
        set_led(color);
    }

    lv_color_t accent = lv_color_hex(0x113a42);
    lv_color_t headline_color = lv_color_hex(0x96f5c5);
    if (mode_switch_pending) {
        accent = lv_color_hex(0x303030);
        headline_color = lv_color_hex(0xf2f2f2);
    } else if (effective_mode == MODE_ALL) {
        accent = lv_color_hex(0x162f58);
        headline_color = lv_color_hex(0x9dc3ff);
    } else if (effective_mode == MODE_CAPTURE) {
        accent = lv_color_hex(0x4b1233);
        headline_color = lv_color_hex(0xff9ac4);
    } else if (effective_mode == MODE_DEAUTH) {
        accent = deauth_alert_active ? lv_color_hex(0x4a1717) : lv_color_hex(0x123d1f);
        headline_color = deauth_alert_active ? lv_color_hex(0xff8d8d) : lv_color_hex(0x8dffab);
    } else if (effective_mode == MODE_PAUSE) {
        accent = lv_color_hex(0x4f3311);
        headline_color = lv_color_hex(0xffd38a);
    }
    if (monitoring_failed) {
        accent = lv_color_hex(0x441144);
        headline_color = lv_color_hex(0xff9ce2);
    }

    lv_obj_set_style_bg_color(top_bar, accent, 0);
    lv_obj_set_style_text_color(headline_label, headline_color, 0);
}

void setup()
{
    Serial.begin(115200);
    delay(1200);
    Serial.println("Pocket WiFi Spy starting");

    mount_sd_card();
    create_session_files();
    load_targets_from_sd();
    effective_mode = compute_monitoring_mode();
    ensure_mode_log_files(effective_mode, millis(), true);
    log_line("[%010lu ms] BOOT session=%s selected=%s effective=%s targets=%u fallback=%u\n",
             static_cast<unsigned long>(millis()),
             session_id,
             selected_mode_name(selected_mode),
             selected_mode_name(effective_mode),
             static_cast<unsigned>(target_count),
             fallback_to_all ? 1U : 0U);

    pinMode(BOOT_PIN, INPUT_PULLUP);

    FastLED.addLeds<APA102, LED_DI_PIN, LED_CI_PIN, BGR>(leds, 1);
    FastLED.setBrightness(72);
    set_led(CRGB::Black);

    init_display();
    update_ui(millis());
    lv_timer_handler();
}

void loop()
{
    const uint32_t now_ms = millis();

    if (!wifi_init_done && now_ms >= wifi_init_due_ms) {
        init_wifi_monitor();
    }

    poll_button(now_ms);
    service_mode_switch(now_ms);
    hop_channel(now_ms);
    service_event_queue();
    flush_logs(now_ms);
    update_ui(now_ms);
    lv_timer_handler();
    delay(5);
}
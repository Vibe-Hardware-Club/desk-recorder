/*
 * KNOB TRANSCRIBER - FIRMWARE
 * ============================================================
 * Turn the knob to pick a topic, tap to record. Audio goes to the SD card in
 * 5-minute chunks and uploads in the background; your Mac transcribes it and
 * files it into that topic's folder. Nothing else to do.
 *
 * WiFi, the ingest host and the bearer token are NOT compiled in. They are
 * written into NVS over USB by setup.sh, so one published binary works for
 * everyone and carries nobody's credentials. See config_store.h.
 *
 * THE RULE THAT SHAPES EVERYTHING: a recording that has not been confirmed
 * stored on the server is never deleted. The SD card is the source of truth
 * until the server says otherwise, so a flat WiFi, a dead Mac or a yanked
 * cable costs nothing but time. Chunking means a crash loses at most 5
 * minutes, not a meeting.
 *
 * Honesty over prettiness on the screen: OFFLINE must never resemble IDLE,
 * and a backlog says UPLOADING rather than pretending it has finished.
 *
 * Hardware notes and pin map: docs/HARDWARE.md
 * Why the dial is not a rotary encoder: docs/THE-DIAL.md
 */

#include <Arduino.h>
#include <Wire.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_sh8601.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "driver/i2s_pdm.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "ff.h"
#include "config_store.h"

// ---- pins, Waveshare's own (see ../../HARDWARE.md) ----
#define LCD_H 360
#define LCD_V 360
#define PIN_LCD_CS    14
#define PIN_LCD_PCLK  13
#define PIN_LCD_D0    15
#define PIN_LCD_D1    16
#define PIN_LCD_D2    17
#define PIN_LCD_D3    18
#define PIN_LCD_RST   21
#define PIN_BK_LIGHT  47
#define TOUCH_SDA     11
#define TOUCH_SCL     12
#define TOUCH_ADDR    0x15
#define ENC_A          8
#define ENC_B          7
#define HAPTIC_ADDR   0x5A   // DRV2605, found on the bus by the touch test
#define LCD_HOST      SPI2_HOST
#define PDM_CLK_PIN   (gpio_num_t)45
#define PDM_DATA_PIN  (gpio_num_t)46
#define SDMMC_CLK_PIN (gpio_num_t)4
#define SDMMC_CMD_PIN (gpio_num_t)3
#define SDMMC_D0_PIN  (gpio_num_t)5
#define SDMMC_D1_PIN  (gpio_num_t)6
#define SDMMC_D2_PIN  (gpio_num_t)42
#define SDMMC_D3_PIN  (gpio_num_t)2
#define C_AMBER  rgb(240, 170, 30)

#include "lcd_init_cmds.inc"   // vendor init sequence, verbatim

static esp_lcd_panel_handle_t panel = NULL;

// RGB565, byte-swapped: the panel takes big-endian, the ESP32 is little.
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  return (uint16_t)((c >> 8) | (c << 8));
}
#define C_BLACK  rgb(0, 0, 0)
#define C_WHITE  rgb(255, 255, 255)
#define C_RED    rgb(230, 30, 40)
#define C_GREY   rgb(90, 90, 90)
#define C_MID    rgb(150, 150, 150)   // the neighbour either side
#define C_DIM    rgb(70, 70, 70)      // two away, on its way out of view

/* One row of pixels, reused. A whole 360x360 frame is 259KB and there is no
 * reason to hold one: everything drawn here is a solid block, so the panel is
 * fed a row at a time from a 720-byte buffer. */
static uint16_t rowbuf[LCD_H];

/* 180 degree rotation, done in software and on purpose. The panel cannot do
 * it: esp_lcd_sh8601.c refuses mirror_y outright ("not supported by this
 * panel"), so MADCTL gets us a mirror, never a rotation. It costs nothing
 * here because every single thing drawn - backgrounds, glyph pixels, blocks -
 * goes through this one function as a rectangle, so rotating each rectangle's
 * position rotates the entire image, text included. Set to 0 to flip back. */
#define ROTATE_180 1

static void fill_rect(int x, int y, int w, int h, uint16_t colour) {
  if (w <= 0 || h <= 0) return;
#if ROTATE_180
  x = LCD_H - x - w;
  y = LCD_V - y - h;
#endif
  for (int i = 0; i < w && i < LCD_H; i++) rowbuf[i] = colour;
  for (int r = 0; r < h; r++) {
    esp_lcd_panel_draw_bitmap(panel, x, y + r, x + w, y + r + 1, rowbuf);
  }
}

/* A 5x7 font. Each glyph is 5 columns, bit 0 = top row.
 *
 * This was originally "the handful of letters this device ever shows", built
 * from one particular topic list, which happened to contain no J, Q, X or Z.
 * The first name added with a J in it rendered PRO ECT, because draw_char
 * skips a glyph it does not have instead of drawing a box. A missing letter
 * looks like a spacing quirk rather than a missing glyph, which is what made
 * it take a while to spot.
 *
 * Now A-Z complete. Three extra glyphs cost 15 bytes and mean whatever you
 * name your topics, the screen can spell it. */
struct Glyph { char c; uint8_t col[5]; };
static const Glyph FONT[] = {
  {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
  {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}}, {'.', {0x00, 0x00, 0x40, 0x00, 0x00}},
  {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
  {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}}, {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
  {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}}, {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
  {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}}, {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
  {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
  {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}}, {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
  {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}}, {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
  {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}}, {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
  {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
  {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}}, {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
  {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}}, {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
  {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}}, {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
  {'X', {0x63, 0x14, 0x08, 0x14, 0x63}}, {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
  {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}}, {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
  {'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
  {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
  {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}}, {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
  {'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
  {':', {0x00, 0x36, 0x36, 0x00, 0x00}}, {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
  {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
};

static void draw_char(int x, int y, char c, int scale, uint16_t colour) {
  if (c >= 'a' && c <= 'z') c -= 32;
  for (const auto &g : FONT) {
    if (g.c != c) continue;
    for (int col = 0; col < 5; col++) {
      for (int row = 0; row < 7; row++) {
        if (g.col[col] & (1 << row)) {
          fill_rect(x + col * scale, y + row * scale, scale, scale, colour);
        }
      }
    }
    return;
  }
}

static void draw_text(int x, int y, const char *s, int scale, uint16_t colour) {
  for (int i = 0; s[i]; i++) draw_char(x + i * 6 * scale, y, s[i], scale, colour);
}

static int text_width(const char *s, int scale) {
  int n = 0;
  while (s[n]) n++;
  return n * 6 * scale - scale;
}

static void draw_text_centred(int y, const char *s, int scale, uint16_t colour) {
  draw_text((LCD_H - text_width(s, scale)) / 2, y, s, scale, colour);
}


/* ============================================================
 * TOPIC LIST - fetched, not flashed
 * ============================================================
 * The server owns this list so adding a topic is a redeploy, never a reflash.
 * The last good list is cached on the SD card, because a device that cannot
 * record until a web request succeeds is a worse device.
 */
#define MAX_CATEGORIES 24
static char cat_slug[MAX_CATEGORIES][40];
static char cat_label[MAX_CATEGORIES][32];
static int  n_categories = 0;
static const char *CATEGORY_CACHE = "/sdcard/categories.txt";

static void categories_fallback() {
  // Only if the server has never been reached and no cache exists. One entry,
  // clearly marked, so the device still records rather than refusing to work.
  strcpy(cat_slug[0], "notes");
  strcpy(cat_label[0], "Notes");
  n_categories = 1;
}

static void categories_save() {
  FILE *f = fopen(CATEGORY_CACHE, "w");
  if (!f) return;
  for (int i = 0; i < n_categories; i++)
    fprintf(f, "%s\t%s\n", cat_slug[i], cat_label[i]);
  fclose(f);
}

static bool categories_load_cache() {
  FILE *f = fopen(CATEGORY_CACHE, "r");
  if (!f) return false;
  n_categories = 0;
  char line[96];
  while (n_categories < MAX_CATEGORIES && fgets(line, sizeof(line), f)) {
    char *tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = 0;
    char *nl = strchr(tab + 1, '\n');
    if (nl) *nl = 0;
    snprintf(cat_slug[n_categories], 40, "%s", line);
    snprintf(cat_label[n_categories], 32, "%s", tab + 1);
    n_categories++;
  }
  fclose(f);
  return n_categories > 0;
}

/* Minimal extraction rather than a JSON library: the payload is ours, its
 * shape is fixed, and 30KB of parser to read two fields per entry is not a
 * trade worth making on a device that also holds an audio buffer. */
static bool categories_fetch() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);
  if (!client.connect(cfg.host, 443)) return false;

  client.printf("GET /functions/v1/transcriber-ingest/categories HTTP/1.1\r\n");
  client.printf("Host: %s\r\n", cfg.host);
  client.printf("Authorization: Bearer %s\r\n", cfg.token);
  client.print("Connection: close\r\n\r\n");

  String body;
  uint32_t t0 = millis();
  while (client.connected() && millis() - t0 < 15000) {
    while (client.available()) body += (char)client.read();
  }
  client.stop();

  int found = 0;
  int at = 0;
  while (found < MAX_CATEGORIES) {
    int s = body.indexOf("\"slug\":\"", at);
    if (s < 0) break;
    s += 8;
    int se = body.indexOf('"', s);
    int l = body.indexOf("\"label\":\"", se);
    if (l < 0) break;
    l += 9;
    int le = body.indexOf('"', l);
    if (se < 0 || le < 0) break;
    snprintf(cat_slug[found], 40, "%s", body.substring(s, se).c_str());
    snprintf(cat_label[found], 32, "%s", body.substring(l, le).c_str());
    found++;
    at = le;
  }
  if (found == 0) return false;
  n_categories = found;
  categories_save();
  return true;
}

/* ============================================================
 * STATE
 * ============================================================ */
static bool recording = false;
static bool configured = false;
static int  category = 0;
static uint32_t started_at = 0;
static volatile int  queue_depth = 0;      // chunks on SD awaiting upload
static volatile bool uploading = false;
static volatile bool wifi_up = false;
static volatile uint32_t oldest_wait_ms = 0;

/*
 * WAVESHARE'S OWN KNOB ALGORITHM, ported from their bidi_switch_knob.c.
 *
 * This knob is NOT a quadrature encoder, which is where a whole afternoon
 * went. There is no phase relationship between the two contacts and they
 * never overlap - by design. Each contact is an independent direction
 * switch: A closing means turned one way, B closing means the other. Their
 * decoder is literally two calls, one per channel, one tagged RIGHT and one
 * LEFT. Proven bidirectional on this exact board by their factory firmware.
 *
 * They also poll rather than interrupt: every 3ms, requiring two consistent
 * samples before believing an edge. That debounce is not optional - an
 * interrupt-driven count sees ~4 bounces per click on channel B and turns 20
 * clicks into 79 events. The event fires on RELEASE (the contact returning
 * high) after it has been held low for at least one tick.
 */
#define KNOB_TICK_US       3000   // their TICKS_INTERVAL, 3ms
#define KNOB_DEBOUNCE_TICKS 2     // their DEBOUNCE_TICKS

static volatile int32_t detent_steps = 0;
static volatile uint8_t prev_state = 3;   // kept for the raw trace only

static volatile uint8_t lvl_a = 1, lvl_b = 1;
static volatile uint8_t dbc_a = 0, dbc_b = 0;

/* One channel, exactly as process_knob_channel() does it. */
static inline void knob_channel(uint8_t level, volatile uint8_t *prev,
                                volatile uint8_t *cnt, int delta) {
  if (level == 0) {
    if (level != *prev) *cnt = 0;
    else (*cnt)++;
  } else {
    if (level != *prev && ++(*cnt) >= KNOB_DEBOUNCE_TICKS) {
      *cnt = 0;
      detent_steps += delta;
    } else {
      *cnt = 0;
    }
  }
  *prev = level;
}

static void knob_poll(void *) {
  uint8_t a = digitalRead(ENC_A);
  uint8_t b = digitalRead(ENC_B);
  knob_channel(a, &lvl_a, &dbc_a, +1);   // A = one direction
  knob_channel(b, &lvl_b, &dbc_b, -1);   // B = the other
}

static void knob_start() {
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  lvl_a = digitalRead(ENC_A);
  lvl_b = digitalRead(ENC_B);
  const esp_timer_create_args_t args = {
      .callback = &knob_poll, .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK, .name = "knob"};
  esp_timer_handle_t h;
  esp_timer_create(&args, &h);
  esp_timer_start_periodic(h, KNOB_TICK_US);
}


static bool read_touch(uint8_t *fingers, uint16_t *x, uint16_t *y) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, 6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  *fingers = b[1];
  *x = ((uint16_t)(b[2] & 0x0F) << 8) | b[3];
  *y = ((uint16_t)(b[4] & 0x0F) << 8) | b[5];
#if ROTATE_180
  /* The panel rotates but the touch panel underneath does not, so its
   * coordinates must be rotated to match or every future tap target is
   * diagonally opposite where it looks. Harmless today, when any tap toggles
   * recording; a genuine bug the moment there are two buttons on screen. */
  if (*x < LCD_H) *x = LCD_H - 1 - *x;
  if (*y < LCD_V) *y = LCD_V - 1 - *y;
#endif
  return true;
}

/* DRV2605 in internal-trigger mode playing one library effect. Confirmation
 * you can feel matters more than confirmation you can see on a device people
 * tap mid-conversation without looking down. */
static void haptic_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(HAPTIC_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void haptic_init() {
  haptic_write(0x01, 0x00);  // out of standby
  haptic_write(0x1A, 0xB6);  // LRA mode
  haptic_write(0x17, 0x00);  // no overdrive clamp
  haptic_write(0x03, 0x06);  // LRA library
  haptic_write(0x0C, 0x00);
}

static void buzz(uint8_t effect) {
  haptic_write(0x04, effect);  // waveform slot 0
  haptic_write(0x05, 0x00);    // end of sequence
  haptic_write(0x0C, 0x01);    // GO
}

static void lcd_init() {
  const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
      PIN_LCD_PCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
      LCD_H * LCD_V * 16 / 8);
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_handle_t io = NULL;
  const esp_lcd_panel_io_spi_config_t io_config =
      SH8601_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);

  sh8601_vendor_config_t vendor_config = {
      .init_cmds = lcd_init_cmds,
      .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
      .flags = {.use_qspi_interface = 1},
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                           &io_config, &io));
  const esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = PIN_LCD_RST,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = 16,
      .vendor_config = &vendor_config,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io, &panel_config, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  esp_lcd_panel_disp_on_off(panel, true);

  // Backlight on PWM. AMOLED, so this is really overall brightness.
  ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE,
                           .duty_resolution = LEDC_TIMER_8_BIT,
                           .timer_num = LEDC_TIMER_3,
                           .freq_hz = 50000,
                           // AUTO, not the vendor's LEDC_SLOW_CLK_RC_FAST:
                           // that enum belongs to the legacy driver and does
                           // not convert under Arduino core 3.x / IDF 5.5.
                           .clk_cfg = LEDC_AUTO_CLK};
  ledc_timer_config(&t);
  ledc_channel_config_t c = {.gpio_num = PIN_BK_LIGHT,
                             .speed_mode = LEDC_LOW_SPEED_MODE,
                             .channel = LEDC_CHANNEL_1,
                             .intr_type = LEDC_INTR_DISABLE,
                             .timer_sel = LEDC_TIMER_3,
                             .duty = 255,
                             .hpoint = 0};
  ledc_channel_config(&c);
}


/* The timer band, sized for the widest clock this will ever show. Kept as
 * constants because two things must agree exactly: the strip cleared and the
 * strip drawn. */

/* ============================================================
 * SD CARD + RECORDING
 * ============================================================ */
#define QUEUE_DIR "/sdcard/queue"
static const uint32_t SAMPLE_RATE = 16000;
static const uint32_t CHUNK_SECONDS = 300;      // 5 minutes, per SPEC
static i2s_chan_handle_t rx_chan = NULL;
static bool sd_ok = false;
static volatile bool write_failed = false;

static void write_wav_header(FILE *f, uint32_t data_bytes) {
  uint32_t byte_rate = SAMPLE_RATE * 2, chunk_size = 36 + data_bytes, fmt = 16;
  uint16_t audio_fmt = 1, channels = 1, block_align = 2, bits = 16;
  fwrite("RIFF", 1, 4, f);            fwrite(&chunk_size, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f);        fwrite(&fmt, 4, 1, f);
  fwrite(&audio_fmt, 2, 1, f);        fwrite(&channels, 2, 1, f);
  fwrite(&SAMPLE_RATE, 4, 1, f);      fwrite(&byte_rate, 4, 1, f);
  fwrite(&block_align, 2, 1, f);      fwrite(&bits, 2, 1, f);
  fwrite("data", 1, 4, f);            fwrite(&data_bytes, 4, 1, f);
}

static bool sd_mount() {
  esp_vfs_fat_sdmmc_mount_config_t mc = {};
  /* NEVER reformat a card that fails to mount. Silently wiping storage
   * someone slotted in is the one destructive surprise this device must not
   * spring. It was switched on once, deliberately, to format a card known to
   * be empty, and switched straight back. Leave it false. */
  mc.format_if_mount_failed = false;
  mc.max_files = 8;
  /* 512 bytes is fine for a small card and fails on a big one: FAT32 runs out
   * of cluster numbers, so both mount and format refuse. 16KB clusters cover
   * cards up to the sizes anyone actually buys. */
  mc.allocation_unit_size = 16 * 1024;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 4;
  slot.clk = SDMMC_CLK_PIN; slot.cmd = SDMMC_CMD_PIN;
  slot.d0 = SDMMC_D0_PIN; slot.d1 = SDMMC_D1_PIN;
  slot.d2 = SDMMC_D2_PIN; slot.d3 = SDMMC_D3_PIN;
  sdmmc_card_t *card = NULL;
  if (esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &card) != ESP_OK) return false;
  mkdir(QUEUE_DIR, 0777);
  Serial.printf("SD: %s %lluMB, sector %u\n", card->cid.name,
                ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024),
                (unsigned)card->csd.sector_size);
  FATFS *fs;
  DWORD free_clusters;
  if (f_getfree("0:", &free_clusters, &fs) == FR_OK) {
    uint64_t total = (uint64_t)(fs->n_fatent - 2) * fs->csize * 512;
    uint64_t freeb = (uint64_t)free_clusters * fs->csize * 512;
    Serial.printf("SD: free %llu MB of %llu MB\n", freeb / (1024 * 1024),
                  total / (1024 * 1024));
  } else {
    Serial.println("SD: f_getfree failed");
  }
  return true;
}

static bool mic_start() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&cc, NULL, &rx_chan) != ESP_OK) return false;
  i2s_pdm_rx_config_t cfg = {
      .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {.clk = PDM_CLK_PIN, .din = PDM_DATA_PIN, .invert_flags = {.clk_inv = false}},
  };
  if (i2s_channel_init_pdm_rx_mode(rx_chan, &cfg) != ESP_OK) return false;
  return i2s_channel_enable(rx_chan) == ESP_OK;
}

/* Session-stamped names: one session's chunks sort together and in order.
 * Falls back to a persisted counter if the clock was never set, because a
 * recording must never be blocked on SNTP. */
static char session_stamp[24];
static int  chunk_index = 0;

static void new_session_stamp() {
  time_t now = time(NULL);
  struct tm tmv;
  localtime_r(&now, &tmv);
  if (tmv.tm_year > 120) {
    strftime(session_stamp, sizeof(session_stamp), "%Y-%m-%d-%H%M%S", &tmv);
  } else {
    static uint32_t fallback = 0;
    FILE *f = fopen("/sdcard/session.cnt", "r");
    if (f) { fscanf(f, "%lu", (unsigned long *)&fallback); fclose(f); }
    fallback++;
    f = fopen("/sdcard/session.cnt", "w");
    if (f) { fprintf(f, "%lu", (unsigned long)fallback); fclose(f); }
    snprintf(session_stamp, sizeof(session_stamp), "s%04lu", (unsigned long)fallback);
  }
  chunk_index = 0;
}

/* Case-insensitive suffix test. FAT can hand back names in a different case
 * than they were written, and a case-sensitive check would silently find
 * nothing while the card is full of recordings. */
static bool is_wav(const char *name) {
  size_t l = strlen(name);
  return l > 4 && !strcasecmp(name + l - 4, ".wav");
}

static void count_queue() {
  int n = 0;
  DIR *d = opendir(QUEUE_DIR);
  if (!d) {
    Serial.printf("QUEUE: cannot open %s\n", QUEUE_DIR);
    queue_depth = 0;
    return;
  }
  struct dirent *e;
  while ((e = readdir(d)) != NULL)
    if (is_wav(e->d_name)) n++;
  closedir(d);
  queue_depth = n;
}

/* The recorder owns its own task so SD latency can never stall the UI, and a
 * slow screen redraw can never drop audio. */
static void record_task(void *) {
  static int16_t buf[1024];
  FILE *f = NULL;
  uint32_t bytes = 0;
  char path[128];

  for (;;) {
    if (!recording) {
      if (f) {   // stopped mid-chunk: close and queue what we have
        fseek(f, 0, SEEK_SET);
        write_wav_header(f, bytes);
        fclose(f);
        f = NULL;
        char final_path[128];
        snprintf(final_path, sizeof(final_path), "%s", path);
        char *dot = strstr(final_path, ".wav.tmp");
        if (dot) dot[4] = 0;                       // strip ".tmp"
        int rn = rename(path, final_path);
        struct stat st;
        int sr = stat(final_path, &st);
        count_queue();
        Serial.printf("REC: chunk done %s rename=%d size=%ld queue=%d\n",
                      final_path, rn, sr == 0 ? (long)st.st_size : -1L, queue_depth);
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (!f) {
      /* Record to .tmp and rename to .wav only when the chunk is CLOSED.
       * The uploader scans this directory every 3s and only looks at .wav,
       * so a take in progress is invisible to it. Without this it found the
       * new, still-empty chunk, judged it a dud fragment and deleted it out
       * from under the recorder - every write and the close then "succeeded"
       * into a file with no directory entry, and the recording vanished. */
      snprintf(path, sizeof(path), QUEUE_DIR "/%s-%s-p%02d.wav.tmp",
               cat_slug[category], session_stamp, ++chunk_index);
      f = fopen(path, "wb");
      if (!f) {
        Serial.printf("REC: cannot open %s\n", path);
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
      Serial.printf("REC: writing %s\n", path);
      write_wav_header(f, 0);
      bytes = 0;
    }

    size_t got = 0;
    esp_err_t rr = i2s_channel_read(rx_chan, buf, sizeof(buf), &got, 500);
    if (rr == ESP_OK && got) {
      /* Count what the CARD accepted, never what the microphone produced.
       * Counting the latter meant a card refusing every write still reported
       * a perfect recording - the exact dishonesty this device must not have. */
      size_t wrote = fwrite(buf, 1, got, f);
      bytes += wrote;
      if (wrote != got) {
        static uint32_t last_werr = 0;
        write_failed = true;
        if (millis() - last_werr > 2000) {
          last_werr = millis();
          Serial.printf("REC: SD WRITE FAILED - asked %u, wrote %u\n",
                        (unsigned)got, (unsigned)wrote);
        }
      }
    } else {
      static uint32_t last_warn = 0;
      if (millis() - last_warn > 2000) {
        last_warn = millis();
        Serial.printf("REC: i2s read %s got=%u\n", esp_err_to_name(rr), (unsigned)got);
      }
    }

    if (bytes >= SAMPLE_RATE * 2 * CHUNK_SECONDS) {   // rotate
      fseek(f, 0, SEEK_SET);
      write_wav_header(f, bytes);
      fclose(f);
      f = NULL;
      char final_path[128];
      snprintf(final_path, sizeof(final_path), "%s", path);
      char *dot = strstr(final_path, ".wav.tmp");
      if (dot) dot[4] = 0;
      rename(path, final_path);
      count_queue();
    }
  }
}

/* ============================================================
 * UPLOAD QUEUE
 * ============================================================
 * Its own task, oldest first, and it keeps running while a recording is in
 * progress. A chunk is deleted ONLY after the server confirms both success
 * and the exact byte count - a 200 with a short body means something went
 * wrong in the middle, and the recording stays on the card.
 */
static bool upload_one(const char *name) {
  char path[160];
  snprintf(path, sizeof(path), QUEUE_DIR "/%s", name);
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 1000) {
    // Say so. A silent delete here is what made a real recording disappear.
    Serial.printf("upload: discarding %s, only %ld bytes\n", name, size);
    fclose(f);
    remove(path);
    return true;
  }

  // Filename carries the category prefix; strip it back off for the URL.
  char slug[40] = {0};
  const char *dash = strchr(name, '-');
  for (int i = 0; i < n_categories; i++) {
    size_t l = strlen(cat_slug[i]);
    if (!strncmp(name, cat_slug[i], l) && name[l] == '-') { snprintf(slug, sizeof(slug), "%s", cat_slug[i]); break; }
  }
  if (!slug[0]) { (void)dash; snprintf(slug, sizeof(slug), "%s", cat_slug[0]); }
  const char *upname = name + strlen(slug) + 1;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(30000);
  if (!client.connect(cfg.host, 443)) { fclose(f); return false; }

  client.printf("POST /functions/v1/transcriber-ingest/ingest?category=%s&filename=%s HTTP/1.1\r\n",
                slug, upname);
  client.printf("Host: %s\r\n", cfg.host);
  client.printf("Authorization: Bearer %s\r\n", cfg.token);
  client.print("Content-Type: audio/wav\r\n");
  client.printf("Content-Length: %ld\r\n", size);
  client.print("Connection: close\r\n\r\n");

  static uint8_t chunk[4096];
  long sent = 0;
  while (sent < size) {
    size_t got = fread(chunk, 1, sizeof(chunk), f);
    if (!got) break;
    if (client.write(chunk, got) != got) { fclose(f); client.stop(); return false; }
    sent += got;
  }
  fclose(f);

  String body;
  uint32_t t0 = millis();
  while (client.connected() && millis() - t0 < 30000) {
    while (client.available()) body += (char)client.read();
  }
  client.stop();

  // Confirmed only if the server echoes ok AND the byte count it stored.
  char expect[32];
  snprintf(expect, sizeof(expect), "\"bytes\":%ld", size);
  if (body.indexOf("\"ok\":true") >= 0 && body.indexOf(expect) >= 0) {
    remove(path);
    Serial.printf("uploaded %s (%ld bytes)\n", name, size);
    return true;
  }
  Serial.printf("upload NOT confirmed for %s - kept on card\n", name);
  return false;
}

static void upload_task(void *) {
  uint32_t backoff = 5000;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      wifi_up = false;
      WiFi.begin(cfg.ssid, cfg.pass);
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    wifi_up = true;

    char oldest[128] = {0};
    DIR *d = opendir(QUEUE_DIR);
    if (d) {
      struct dirent *e;
      while ((e = readdir(d)) != NULL) {
        if (!is_wav(e->d_name)) continue;
        if (!oldest[0] || strcmp(e->d_name, oldest) < 0)
          snprintf(oldest, sizeof(oldest), "%s", e->d_name);
      }
      closedir(d);
    }

    if (!oldest[0]) {
      uploading = false;
      oldest_wait_ms = 0;
      backoff = 5000;
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    uploading = true;
    if (upload_one(oldest)) {
      backoff = 5000;
      oldest_wait_ms = 0;
      count_queue();
    } else {
      oldest_wait_ms += backoff;
      vTaskDelay(pdMS_TO_TICKS(backoff));
      if (backoff < 300000) backoff *= 2;   // 5s, 10s, 20s ... capped at 5 min
    }
    count_queue();
  }
}

/* ============================================================
 * SCREEN
 * ============================================================ */
static const int TIMER_Y = 205, TIMER_H = 52;

static void timer_text(char *out, size_t n) {
  uint32_t s = (millis() - started_at) / 1000;
  snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static void render_timer() {
  char t[16];
  timer_text(t, sizeof(t));
  fill_rect(0, TIMER_Y, LCD_H, TIMER_H, C_RED);
  draw_text_centred(TIMER_Y + 5, t, 6, C_WHITE);
}

/* The backlog line. It says what is true: how many chunks are still on the
 * card, and whether anything is moving. Silence about a backlog would be the
 * one dishonesty that matters on this device. */
static void status_line(int y, uint16_t bg) {
  char s[40];
  if (!wifi_up) snprintf(s, sizeof(s), "OFFLINE - %d ON CARD", queue_depth);
  else if (queue_depth > 0) snprintf(s, sizeof(s), "UPLOADING %d", queue_depth);
  else snprintf(s, sizeof(s), "SYNCED");
  fill_rect(0, y - 4, LCD_H, 26, bg);
  draw_text_centred(y, s, 2, !wifi_up ? C_AMBER : (queue_depth ? C_AMBER : C_GREY));
}

static void render_list() {
  fill_rect(0, 0, LCD_H, LCD_V, C_BLACK);
  struct Row { int offset; int y; int scale; uint16_t colour; };
  static const Row ROWS[] = {
      {-2,  52, 2, C_DIM}, {-1, 102, 2, C_MID}, {0, 152, 3, C_WHITE},
      {1, 212, 2, C_MID},  {2, 262, 2, C_DIM},
  };
  for (const auto &r : ROWS) {
    int idx = (category + r.offset + n_categories * 2) % n_categories;
    draw_text_centred(r.y, cat_label[idx], r.scale, r.colour);
  }
  fill_rect(18, 158, 22, 3, C_GREY);
  fill_rect(LCD_H - 40, 158, 22, 3, C_GREY);
  status_line(315, C_BLACK);
}

static void render() {
  /* Before anything else: a device nobody has set up yet. It says what it is
   * and what to do, because the alternative is a stranger holding a blank
   * screen wondering whether they have bricked a fifty pound board. */
  if (!configured) {
    fill_rect(0, 0, LCD_H, LCD_V, C_AMBER);
    draw_text_centred(90,  "NOT SET UP", 4, C_BLACK);
    draw_text_centred(160, "CONNECT USB", 2, C_BLACK);
    draw_text_centred(190, "AND RUN", 2, C_BLACK);
    draw_text_centred(220, "SETUP ON YOUR MAC", 2, C_BLACK);
    return;
  }
  if (!sd_ok) {
    /* NO CARD is not a footnote. Tapping a device that looks ready and
     * silently records nothing is the worst failure this thing could have,
     * so it takes the whole screen and the tap is refused outright. */
    fill_rect(0, 0, LCD_H, LCD_V, C_AMBER);
    draw_text_centred(110, "NO SD CARD", 4, C_BLACK);
    draw_text_centred(180, "CANNOT RECORD", 2, C_BLACK);
    draw_text_centred(215, "INSERT CARD", 2, C_BLACK);
    draw_text_centred(245, "AND POWER CYCLE", 2, C_BLACK);
    return;
  }
  if (recording) {
    fill_rect(0, 0, LCD_H, LCD_V, C_RED);
    draw_text_centred(70, "RECORDING", 5, C_WHITE);
    draw_text_centred(150, cat_label[category], 3, C_WHITE);
    render_timer();
    status_line(300, C_RED);
  } else {
    render_list();
  }
}

/* ============================================================
 * SETUP + LOOP
 * ============================================================ */
void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n=== KNOB TRANSCRIBER ===");

  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
  haptic_init();
  knob_start();
  lcd_init();

  /* A device straight out of the box has no WiFi and no token, so there is
   * nothing useful it can do except say so and wait for the installer on the
   * other end of the cable. Stopping here rather than half-starting keeps the
   * post-setup path identical to a normal boot: SAVE reboots, and everything
   * below runs exactly once, configured. */
  cfg_load();
  configured = cfg_is_set();
  if (!configured) {
    Serial.println("NOT CONFIGURED - waiting for setup over USB");
    cfg_status();
    render();
    return;
  }

  sd_ok = sd_mount();
  Serial.printf("SD: %s\n", sd_ok ? "mounted" : "MISSING - cannot record");
  // Cache first so the device is usable instantly, then refresh from the
  // server in the background once WiFi is up.
  if (!sd_ok || !categories_load_cache()) categories_fallback();

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.pass);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  wifi_up = WiFi.status() == WL_CONNECTED;
  if (wifi_up) {
    configTzTime("GMT0BST,M3.5.0/1,M10.5.0", "pool.ntp.org");
    if (categories_fetch()) Serial.printf("categories: %d from server\n", n_categories);
  }
  Serial.printf("WiFi: %s | categories: %d | queued: %d\n",
                wifi_up ? "up" : "down", n_categories, queue_depth);

  if (sd_ok && mic_start()) {
    /* STACK SIZES MATTER HERE, and undersized ones fail SILENTLY.
     * count_queue() finds files correctly from the main task and returned 0
     * from these two - same card, same code. Directory reads carry a long
     * filename buffer on the stack (FATFS is built LFN_STACK with MAX_LFN
     * 255), and a task without room for it does not crash, it just reads
     * nothing. That cost two "lost" recordings that were on the card all
     * along, and a card replaced for no reason. Do not trim these. */
    xTaskCreatePinnedToCore(record_task, "record", 24576, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(upload_task, "upload", 24576, NULL, 3, NULL, 0);
  } else {
    Serial.println("mic or SD unavailable - recording disabled");
  }

  render();
  buzz(1);
}

void loop() {
  /* Serial does two jobs. It is the channel the installer speaks to set this
   * device up, and it is the debug trigger that exercises the recorder from
   * the Mac without anyone having to keep tapping the screen.
   *
   * Line-based, because a WiFi password arrives a byte at a time and the old
   * "any input toggles recording" would have started a take halfway through
   * one. Config commands claim their line; REC or a bare Enter toggles. */
  static char sline[256];
  static size_t slen = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c != '\n' && c != '\r') {
      if (slen < sizeof(sline) - 1) sline[slen++] = c;
      continue;
    }
    sline[slen] = 0;
    slen = 0;

    bool was_save = !strcmp(sline, "SAVE");
    if (cfg_handle_line(sline)) {
      /* Reboot on a successful save rather than reconfiguring a running
       * device. WiFi, the category fetch and the upload task all read this
       * config at start, so the honest way to apply a change is to go through
       * a normal boot once. */
      if (was_save && cfg_is_set()) {
        Serial.println("OK rebooting");
        delay(200);
        ESP.restart();
      }
      continue;
    }

    /* Empty lines are IGNORED, and that is not a detail. A terminal sending
     * CRLF ends the line on \r and then delivers \n as a second, empty line.
     * While an empty line meant "toggle recording", every single command sent
     * to this device also started or stopped a take. Found on the real board;
     * no amount of reading the code would have shown it. */
    if (!sline[0]) continue;
    if (strcmp(sline, "REC")) { Serial.printf("ERR unknown command %s\n", sline); continue; }
    if (!configured) { Serial.println("REC refused, not set up"); continue; }
    if (!sd_ok)      { Serial.println("REC refused, no SD card"); continue; }
    if (!recording) { new_session_stamp(); started_at = millis(); recording = true; }
    else recording = false;
    Serial.printf("SERIAL -> %s (%s)\n", recording ? "RECORDING" : "stopped",
                  cat_label[category]);
    render();
  }

  /* Nothing below means anything on a device that has not been set up. The
   * screen already says so and the serial handler above is the way out. */
  if (!configured) { delay(50); return; }

  static bool was_down = false;
  static int32_t last_knob = 0;
  static uint32_t last_tick = 0, last_status = 0, last_buzz = 0;
  bool dirty = false;

  uint8_t fingers;
  uint16_t x, y;
  if (read_touch(&fingers, &x, &y)) {
    bool down = fingers > 0;
    if (down && !was_down) {
      if (!sd_ok) {
        buzz(1);
        Serial.println("TAP refused - no SD card");
      } else if (!recording) {
        new_session_stamp();
        started_at = millis();
        recording = true;
        buzz(14);
      } else if (recording) {
        recording = false;
        buzz(1);
      }
      Serial.printf("TAP -> %s (%s)\n", recording ? "RECORDING" : "stopped", cat_label[category]);
      dirty = true;
    }
    was_down = down;
  }

  int32_t k = detent_steps;
  if (k != last_knob) {
    if (!recording) {            // client is fixed once a take starts
      int delta = (k > last_knob) ? 1 : -1;
      category = (category + delta + n_categories) % n_categories;
      buzz(7);
      dirty = true;
    }
    last_knob = k;
  }

  if (recording && millis() - last_tick > 1000) {
    last_tick = millis();
    if (!dirty) render_timer();
  }
  // A periodic tick you can feel: this device must never be secretly running.
  if (recording && millis() - last_buzz > 60000) { last_buzz = millis(); buzz(7); }

  // A card pushed back in should just start working. Retry while idle.
  static uint32_t last_sd_try = 0;
  if (!sd_ok && !recording && millis() - last_sd_try > 5000) {
    last_sd_try = millis();
    if (sd_mount()) {
      sd_ok = true;
      count_queue();
      Serial.println("SD: card appeared - recording enabled");
      if (mic_start()) {
        xTaskCreatePinnedToCore(record_task, "record", 24576, NULL, 5, NULL, 1);
        xTaskCreatePinnedToCore(upload_task, "upload", 24576, NULL, 3, NULL, 0);
      }
      dirty = true;
    }
  }

  if (millis() - last_status > 3000) {
    last_status = millis();
    if (!dirty) status_line(recording ? 300 : 315, recording ? C_RED : C_BLACK);
  }

  if (dirty) render();
  delay(20);
}

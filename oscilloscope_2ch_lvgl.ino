#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>

/*
  2-Channel Oscilloscope UI for ESP32 + LVGL (Agilent-style layout)
  ------------------------------------------------------------------
  Features:
  - CH1 / CH2 live waveforms
  - Trigger level + slope + source select
  - Horizontal zoom (time/div)
  - Vertical attenuation (V/div) per channel
  - Channel enable/disable
  - Probe multiplier (x1/x10)

  Notes:
  - Tune ADC pins and display setup for your Elecrow 7" hardware.
  - This sketch uses TFT_eSPI as the LVGL flush backend.
*/

// ------------------------- Hardware configuration -------------------------
static constexpr int ADC_CH1_PIN = 1;   // Update for your board
static constexpr int ADC_CH2_PIN = 2;   // Update for your board
static constexpr uint32_t SAMPLE_RATE_HZ = 25000;
static constexpr uint16_t SAMPLE_COUNT = 800;

// ----------------------------- Display/LVGL -------------------------------
TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;

static lv_obj_t *wave_canvas;
static lv_obj_t *status_label;
static lv_obj_t *softkey_bar;
static lv_color_t *canvas_buf;

// UI controls
static lv_obj_t *sw_ch1;
static lv_obj_t *sw_ch2;
static lv_obj_t *dd_probe_ch1;
static lv_obj_t *dd_probe_ch2;
static lv_obj_t *dd_vdiv_ch1;
static lv_obj_t *dd_vdiv_ch2;
static lv_obj_t *dd_time_div;
static lv_obj_t *dd_trig_src;
static lv_obj_t *dd_trig_slope;
static lv_obj_t *slider_trigger;

// -------------------------- Sampling/processing ---------------------------
static volatile uint16_t ch1_samples[SAMPLE_COUNT];
static volatile uint16_t ch2_samples[SAMPLE_COUNT];
static volatile uint16_t wr_idx = 0;
static volatile bool frame_ready = false;

static uint16_t ch1_copy[SAMPLE_COUNT];
static uint16_t ch2_copy[SAMPLE_COUNT];

hw_timer_t *sample_timer = nullptr;
portMUX_TYPE timer_mux = portMUX_INITIALIZER_UNLOCKED;

// ------------------------------- Scope state ------------------------------
enum TriggerSource : uint8_t { TRIG_CH1 = 0, TRIG_CH2 = 1 };
enum TriggerSlope : uint8_t { TRIG_RISING = 0, TRIG_FALLING = 1 };

static bool ch1_enabled = true;
static bool ch2_enabled = true;
static uint8_t probe_mul_ch1 = 1;  // 1 or 10
static uint8_t probe_mul_ch2 = 1;

static TriggerSource trigger_source = TRIG_CH1;
static TriggerSlope trigger_slope = TRIG_RISING;
static uint16_t trigger_level_adc = 2048;

// Agilent-like colors
static constexpr uint32_t COLOR_BG = 0x060B0F;
static constexpr uint32_t COLOR_GRID_MINOR = 0x1A3D4A;
static constexpr uint32_t COLOR_GRID_MAJOR = 0x2E6575;
static constexpr uint32_t COLOR_CH1 = 0xFFD400;  // yellow
static constexpr uint32_t COLOR_CH2 = 0x00D5FF;  // cyan
static constexpr uint32_t COLOR_TEXT = 0xE8EEF2;
static constexpr uint32_t COLOR_ACCENT = 0x4FC3F7;

// V/div options in mV/div at probe x1
static constexpr uint16_t VDIV_TABLE_MV[] = {100, 200, 500, 1000, 2000};
// Time/div options represented as sample stride (higher stride = zoom out)
static constexpr uint8_t TIME_STRIDE_TABLE[] = {1, 2, 4, 8};

static uint8_t idx_vdiv_ch1 = 2; // 500 mV/div
static uint8_t idx_vdiv_ch2 = 2;
static uint8_t idx_time_div = 1; // stride=2

// ------------------------------- Helpers ----------------------------------
static inline uint16_t adc_to_millivolt(uint16_t adc) {
  return static_cast<uint16_t>((adc * 3300UL) / 4095UL);
}

static inline int32_t sample_to_screen_y(uint16_t adc, int32_t h, uint8_t vdiv_idx, uint8_t probe_mul) {
  const float mv = static_cast<float>(adc_to_millivolt(adc));
  const float center_mv = 1650.0f;
  const float mv_per_div = static_cast<float>(VDIV_TABLE_MV[vdiv_idx]) * probe_mul;
  const float px_per_div = h / 8.0f; // 8 vertical divisions
  const float dy_div = (mv - center_mv) / mv_per_div;
  const float y = (h / 2.0f) - (dy_div * px_per_div);
  if (y < 2.0f) return 2;
  if (y > h - 2.0f) return h - 2;
  return static_cast<int32_t>(y);
}

void IRAM_ATTR on_sample_timer() {
  portENTER_CRITICAL_ISR(&timer_mux);
  if (frame_ready) {
    portEXIT_CRITICAL_ISR(&timer_mux);
    return;
  }

  ch1_samples[wr_idx] = analogRead(ADC_CH1_PIN);
  ch2_samples[wr_idx] = analogRead(ADC_CH2_PIN);

  wr_idx++;
  if (wr_idx >= SAMPLE_COUNT) {
    wr_idx = 0;
    frame_ready = true;
  }
  portEXIT_CRITICAL_ISR(&timer_mux);
}

void lvgl_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors(reinterpret_cast<uint16_t *>(&color_p->full), w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

static void draw_grid(uint16_t w, uint16_t h) {
  lv_canvas_fill_bg(wave_canvas, lv_color_hex(COLOR_BG), LV_OPA_COVER);

  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);
  dsc.width = 1;

  lv_point_t p1, p2;
  const uint16_t x_step = w / 10; // 10 horizontal divisions
  const uint16_t y_step = h / 8;  // 8 vertical divisions

  for (uint16_t x = 0; x <= w; x += x_step) {
    dsc.color = lv_color_hex((x % (x_step * 5) == 0) ? COLOR_GRID_MAJOR : COLOR_GRID_MINOR);
    p1.x = x; p1.y = 0;
    p2.x = x; p2.y = h;
    lv_canvas_draw_line(wave_canvas, &p1, &p2, &dsc);
  }

  for (uint16_t y = 0; y <= h; y += y_step) {
    dsc.color = lv_color_hex((y % (y_step * 4) == 0) ? COLOR_GRID_MAJOR : COLOR_GRID_MINOR);
    p1.x = 0; p1.y = y;
    p2.x = w; p2.y = y;
    lv_canvas_draw_line(wave_canvas, &p1, &p2, &dsc);
  }
}

static int find_trigger_index(const uint16_t *src, size_t n) {
  for (size_t i = 1; i < n; ++i) {
    bool cross = (trigger_slope == TRIG_RISING)
      ? (src[i - 1] < trigger_level_adc && src[i] >= trigger_level_adc)
      : (src[i - 1] > trigger_level_adc && src[i] <= trigger_level_adc);
    if (cross) return static_cast<int>(i);
  }
  return 0;
}

static void draw_trace(const uint16_t *samples, uint16_t w, uint16_t h, lv_color_t color, uint8_t vdiv_idx, uint8_t probe_mul, int trig) {
  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);
  dsc.color = color;
  dsc.width = 2;

  const uint8_t stride = TIME_STRIDE_TABLE[idx_time_div];
  lv_point_t p1, p2;

  for (uint16_t x = 1; x < w; ++x) {
    size_t i1 = (trig + (x - 1) * stride) % SAMPLE_COUNT;
    size_t i2 = (trig + x * stride) % SAMPLE_COUNT;
    p1.x = x - 1;
    p2.x = x;
    p1.y = sample_to_screen_y(samples[i1], h, vdiv_idx, probe_mul);
    p2.y = sample_to_screen_y(samples[i2], h, vdiv_idx, probe_mul);
    lv_canvas_draw_line(wave_canvas, &p1, &p2, &dsc);
  }
}

static void refresh_status_bar() {
  char txt[220];
  const char *src = trigger_source == TRIG_CH1 ? "CH1" : "CH2";
  const char *slope = trigger_slope == TRIG_RISING ? "Rising" : "Falling";
  snprintf(
    txt,
    sizeof(txt),
    "CH1 %s %umV/div x%u | CH2 %s %umV/div x%u | Time x%u | Trig %s %s %umV",
    ch1_enabled ? "ON" : "OFF", VDIV_TABLE_MV[idx_vdiv_ch1], probe_mul_ch1,
    ch2_enabled ? "ON" : "OFF", VDIV_TABLE_MV[idx_vdiv_ch2], probe_mul_ch2,
    TIME_STRIDE_TABLE[idx_time_div], src, slope, adc_to_millivolt(trigger_level_adc)
  );
  lv_label_set_text(status_label, txt);
}

static void render_scope() {
  uint16_t w = lv_obj_get_width(wave_canvas);
  uint16_t h = lv_obj_get_height(wave_canvas);

  draw_grid(w, h);

  const uint16_t *trig_buf = (trigger_source == TRIG_CH1) ? ch1_copy : ch2_copy;
  int trig = find_trigger_index(trig_buf, SAMPLE_COUNT);

  if (ch1_enabled) {
    draw_trace(ch1_copy, w, h, lv_color_hex(COLOR_CH1), idx_vdiv_ch1, probe_mul_ch1, trig);
  }
  if (ch2_enabled) {
    draw_trace(ch2_copy, w, h, lv_color_hex(COLOR_CH2), idx_vdiv_ch2, probe_mul_ch2, trig);
  }

  // Trigger level marker
  lv_draw_line_dsc_t trig_dsc;
  lv_draw_line_dsc_init(&trig_dsc);
  trig_dsc.color = lv_color_hex(COLOR_ACCENT);
  trig_dsc.width = 1;
  trig_dsc.dash_gap = 4;
  trig_dsc.dash_width = 4;

  const int y_trig = sample_to_screen_y(trigger_level_adc, h,
                                        (trigger_source == TRIG_CH1 ? idx_vdiv_ch1 : idx_vdiv_ch2),
                                        (trigger_source == TRIG_CH1 ? probe_mul_ch1 : probe_mul_ch2));
  lv_point_t p1{0, y_trig};
  lv_point_t p2{static_cast<lv_coord_t>(w), static_cast<lv_coord_t>(y_trig)};
  lv_canvas_draw_line(wave_canvas, &p1, &p2, &trig_dsc);

  refresh_status_bar();
}

// ------------------------------- UI events --------------------------------
static void on_ui_changed(lv_event_t *e) {
  LV_UNUSED(e);
  ch1_enabled = lv_obj_has_state(sw_ch1, LV_STATE_CHECKED);
  ch2_enabled = lv_obj_has_state(sw_ch2, LV_STATE_CHECKED);

  probe_mul_ch1 = lv_dropdown_get_selected(dd_probe_ch1) == 0 ? 1 : 10;
  probe_mul_ch2 = lv_dropdown_get_selected(dd_probe_ch2) == 0 ? 1 : 10;

  idx_vdiv_ch1 = lv_dropdown_get_selected(dd_vdiv_ch1);
  idx_vdiv_ch2 = lv_dropdown_get_selected(dd_vdiv_ch2);
  idx_time_div = lv_dropdown_get_selected(dd_time_div);

  trigger_source = lv_dropdown_get_selected(dd_trig_src) == 0 ? TRIG_CH1 : TRIG_CH2;
  trigger_slope = lv_dropdown_get_selected(dd_trig_slope) == 0 ? TRIG_RISING : TRIG_FALLING;
  trigger_level_adc = static_cast<uint16_t>(lv_slider_get_value(slider_trigger));

  render_scope();
}

// ------------------------------- Init code --------------------------------
static void init_lvgl() {
  lv_init();
  tft.begin();
  tft.setRotation(1);

  const uint16_t w = tft.width();
  const uint16_t h = tft.height();
  const size_t line_count = 48;
  const size_t buf_pixels = w * line_count;

  buf1 = static_cast<lv_color_t *>(heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_DMA));
  buf2 = static_cast<lv_color_t *>(heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_DMA));
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_pixels);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = w;
  disp_drv.ver_res = h;
  disp_drv.flush_cb = lvgl_flush_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);
}

static lv_obj_t *mk_dd(lv_obj_t *parent, const char *opts, int x, int y, int w = 110) {
  lv_obj_t *dd = lv_dropdown_create(parent);
  lv_dropdown_set_options(dd, opts);
  lv_obj_set_size(dd, w, 32);
  lv_obj_set_pos(dd, x, y);
  lv_obj_add_event_cb(dd, on_ui_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  return dd;
}

static void build_ui() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

  const int screen_w = tft.width();
  const int screen_h = tft.height();

  // Top status bar
  status_label = lv_label_create(scr);
  lv_obj_set_width(status_label, screen_w - 16);
  lv_obj_set_style_text_color(status_label, lv_color_hex(COLOR_TEXT), 0);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 8, 6);

  // Right control panel (Agilent-like soft menu)
  softkey_bar = lv_obj_create(scr);
  lv_obj_set_size(softkey_bar, 260, screen_h - 56);
  lv_obj_align(softkey_bar, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
  lv_obj_set_style_bg_color(softkey_bar, lv_color_hex(0x0E1A22), 0);
  lv_obj_set_style_border_color(softkey_bar, lv_color_hex(0x33505D), 0);
  lv_obj_set_style_border_width(softkey_bar, 1, 0);
  lv_obj_set_scrollbar_mode(softkey_bar, LV_SCROLLBAR_MODE_OFF);

  auto mk_lbl = [&](const char *txt, int x, int y) {
    lv_obj_t *l = lv_label_create(softkey_bar);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT), 0);
  };

  mk_lbl("CH1", 10, 10);
  sw_ch1 = lv_switch_create(softkey_bar);
  lv_obj_set_pos(sw_ch1, 60, 8);
  lv_obj_add_state(sw_ch1, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw_ch1, on_ui_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  mk_lbl("Probe", 10, 44);
  dd_probe_ch1 = mk_dd(softkey_bar, "x1\nx10", 70, 36, 80);
  mk_lbl("V/div", 160, 44);
  dd_vdiv_ch1 = mk_dd(softkey_bar, "100mV\n200mV\n500mV\n1V\n2V", 200, 36, 56);
  lv_dropdown_set_selected(dd_vdiv_ch1, idx_vdiv_ch1);

  mk_lbl("CH2", 10, 86);
  sw_ch2 = lv_switch_create(softkey_bar);
  lv_obj_set_pos(sw_ch2, 60, 84);
  lv_obj_add_state(sw_ch2, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw_ch2, on_ui_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  mk_lbl("Probe", 10, 122);
  dd_probe_ch2 = mk_dd(softkey_bar, "x1\nx10", 70, 114, 80);
  mk_lbl("V/div", 160, 122);
  dd_vdiv_ch2 = mk_dd(softkey_bar, "100mV\n200mV\n500mV\n1V\n2V", 200, 114, 56);
  lv_dropdown_set_selected(dd_vdiv_ch2, idx_vdiv_ch2);

  mk_lbl("Time/div", 10, 166);
  dd_time_div = mk_dd(softkey_bar, "x1\nx2\nx4\nx8", 100, 158, 90);
  lv_dropdown_set_selected(dd_time_div, idx_time_div);

  mk_lbl("Trigger Source", 10, 208);
  dd_trig_src = mk_dd(softkey_bar, "CH1\nCH2", 140, 200, 90);
  mk_lbl("Trigger Slope", 10, 250);
  dd_trig_slope = mk_dd(softkey_bar, "Rising\nFalling", 140, 242, 100);
  mk_lbl("Trigger Level", 10, 292);

  slider_trigger = lv_slider_create(softkey_bar);
  lv_obj_set_size(slider_trigger, 236, 24);
  lv_obj_set_pos(slider_trigger, 10, 314);
  lv_slider_set_range(slider_trigger, 0, 4095);
  lv_slider_set_value(slider_trigger, trigger_level_adc, LV_ANIM_OFF);
  lv_obj_add_event_cb(slider_trigger, on_ui_changed, LV_EVENT_VALUE_CHANGED, nullptr);

  // Waveform canvas
  wave_canvas = lv_canvas_create(scr);
  const uint16_t cw = screen_w - 280;
  const uint16_t ch = screen_h - 70;
  lv_obj_set_size(wave_canvas, cw, ch);
  lv_obj_align(wave_canvas, LV_ALIGN_BOTTOM_LEFT, 8, -8);

  canvas_buf = static_cast<lv_color_t *>(heap_caps_malloc(cw * ch * sizeof(lv_color_t), MALLOC_CAP_INTERNAL));
  lv_canvas_set_buffer(wave_canvas, canvas_buf, cw, ch, LV_IMG_CF_TRUE_COLOR);

  refresh_status_bar();
  render_scope();
}

static void setup_adc() {
  analogReadResolution(12);
  analogSetPinAttenuation(ADC_CH1_PIN, ADC_11db);
  analogSetPinAttenuation(ADC_CH2_PIN, ADC_11db);
}

static void start_sampling_timer() {
  sample_timer = timerBegin(0, 80, true); // 1 MHz timer base
  timerAttachInterrupt(sample_timer, &on_sample_timer, true);
  timerAlarmWrite(sample_timer, 1000000UL / SAMPLE_RATE_HZ, true);
  timerAlarmEnable(sample_timer);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  setup_adc();
  init_lvgl();
  build_ui();
  start_sampling_timer();
  Serial.println("Agilent-style 2CH scope UI started");
}

void loop() {
  if (frame_ready) {
    portENTER_CRITICAL(&timer_mux);
    for (uint16_t i = 0; i < SAMPLE_COUNT; ++i) {
      ch1_copy[i] = ch1_samples[i];
      ch2_copy[i] = ch2_samples[i];
    }
    frame_ready = false;
    portEXIT_CRITICAL(&timer_mux);

    render_scope();
  }

  lv_timer_handler();
  delay(5);
}

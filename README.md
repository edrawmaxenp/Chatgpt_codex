# 2-Channel Oscilloscope (ESP32 + Elecrow 7" LVGL Display)

This project provides an **Agilent-style** 2-channel oscilloscope UI for ESP32 with LVGL.

## Included features

- CH1 + CH2 waveform rendering
- Channel ON/OFF switches
- Trigger system
  - Trigger source: CH1 / CH2
  - Trigger slope: rising / falling
  - Trigger level slider
- Horizontal zoom (`Time/div` using sample stride)
- Vertical attenuation (`V/div`) per channel
- Probe setting per channel (`x1` / `x10`)
- Scope grid + trigger level line + status bar

## File

- `oscilloscope_2ch_lvgl.ino`

## Library requirements

- `lvgl`
- `TFT_eSPI`

## Setup

1. Set ADC pins in sketch:
   - `ADC_CH1_PIN`
   - `ADC_CH2_PIN`
2. Configure `TFT_eSPI` (`User_Setup.h`) for your Elecrow panel and controller.
3. Flash to ESP32 and open serial monitor (115200 baud).

## Elecrow note

Elecrow 7" products may use different display interfaces (SPI/RGB). If your board ships with a vendor BSP, keep the oscilloscope logic and UI code, and replace only the display-flush portion if needed.

## Safety

- ESP32 ADC is **0–3.3V max**.
- Use proper probe attenuation/dividers for higher voltage signals.

## UI preview

- Open `ui_preview.html` in a browser to see a static mock of the Agilent-style layout used by the sketch.
- This preview is for visual reference only; live rendering runs on LVGL on the ESP32 target.

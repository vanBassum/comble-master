# ──────────────────────────────────────────────────────────────
# Board fragment: Wireless-Tag WT-SC01 Plus
#   ESP32-S3-WROOM-1 · 3.5" 320x480 ST7796 over an 8-bit i80/8080 parallel bus
#   FT6336U capacitive touch (not yet driven) · no knob, no user LED
#
# BOARD_HAS_DISPLAY is read back in main/CMakeLists.txt to decide whether the
# display demo is compiled in. It works where a per-board REQUIRES would not,
# because BOARD_SOURCES is evaluated in the normal configure pass where BOARD
# is visible — see the note in main/CMakeLists.txt.
#
# Component deps are NOT set here: esp_lcd_st7796 is a managed dep in
# main/idf_component.yml, target-gated to esp32s3.
# ──────────────────────────────────────────────────────────────

set(BOARD_HAS_DISPLAY TRUE)

list(APPEND BOARD_SOURCES "${CMAKE_CURRENT_LIST_DIR}/BoardContext.cpp")

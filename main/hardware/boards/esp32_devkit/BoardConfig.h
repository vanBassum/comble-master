#pragma once

// ──────────────────────────────────────────────────────────────
// BoardContext configuration — generic ESP32 DevKit (ESP32-WROOM-32).
// Pin assignments and constants for this board only. Other boards live in
// sibling folders under hardware/boards/ and are selected with -DBOARD=<name>.
// ──────────────────────────────────────────────────────────────

namespace BoardConfig
{
    // LED
    // GPIO2 is the built-in LED on most ESP32 DevKit boards.
    // A board without an LED drops these constants and binds MockLed
    // in its BoardContext class instead (see hardware/interfaces/Led.h).
    static constexpr int LED_PIN = 2;
    static constexpr bool LED_ACTIVE_HIGH = true;

    // USB: this chip has no USB device peripheral at all, so it can expose no
    // Comble COM ports. UsbPortManager reads this and registers nothing.
    static constexpr int USB_COM_PORTS = 0;

    // Add project-specific pin definitions below.
    // Examples:
    //   static constexpr int MODBUS_TX_PIN = 17;
    //   static constexpr int MODBUS_RX_PIN = 16;
    //   static constexpr int SPI_MOSI_PIN  = 13;
}

#pragma once

#include "host/ble_uuid.h"
#include <cstdint>
#include <cstddef>

// ──────────────────────────────────────────────────────────────
// The BLE contract between a Comble host (WT-SC01 Plus) and a Comble slave
// (wireless UART adapter).
//
// THIS FILE MUST STAY IDENTICAL IN MEANING ON BOTH SIDES. The slave keeps its
// own copy at `Comble slave/main/app/Ble/CombleBleProtocol.h`; same UUIDs, same
// advertising layout. Change one side alone and the link stops pairing with no
// error that says why — the KC1245 gateway/thermostat pair carries the same
// warning for the same reason.
//
// This is the HOST's copy. It is a duplicate rather than a shared header
// because the two live in separate repositories; if they are ever merged into
// one workspace, this is the first thing that should become a shared component.
//
// Roles: the SLAVE is the peripheral. It advertises and hosts the GATT table;
// the host scans, picks one, pairs and subscribes. That split is deliberate —
// several slaves must be discoverable at once by whichever host is looking, and
// a peripheral is the role that scales to "a bag of adapters on a bench".
//
// Not in here yet: the UART transport itself (line config, flow control,
// sequencing). This is the discovery-and-ownership layer only, and the
// characteristics below are the seam that transport will later use.
// ──────────────────────────────────────────────────────────────
namespace comble
{
    // cb1e00xx-5f6a-4d21-9c3b-7e8f2a4d6015 — 0xCB1E reads as "CoMBLE".
    // BLE_UUID128_INIT takes the bytes LITTLE-ENDIAN, i.e. the string reversed.

    inline constexpr ble_uuid128_t SERVICE_UUID = BLE_UUID128_INIT(
        0x15, 0x60, 0x4d, 0x2a, 0x8f, 0x7e, 0x3b, 0x9c,
        0x21, 0x4d, 0x6a, 0x5f, 0x01, 0x00, 0x1e, 0xcb);

    // Slave → host. Notify only: the host subscribes and the slave pushes.
    // Will carry UART data and status once the transport exists.
    inline constexpr ble_uuid128_t CHR_INBOUND_UUID = BLE_UUID128_INIT(
        0x15, 0x60, 0x4d, 0x2a, 0x8f, 0x7e, 0x3b, 0x9c,
        0x21, 0x4d, 0x6a, 0x5f, 0x02, 0x00, 0x1e, 0xcb);

    // Host → slave. Written by the host, and the write demands an encrypted,
    // AUTHENTICATED link — so an unpaired host that merely connects cannot move
    // a single byte. That is the ownership rule enforced by the stack rather
    // than by application code remembering to check it.
    inline constexpr ble_uuid128_t CHR_OUTBOUND_UUID = BLE_UUID128_INIT(
        0x15, 0x60, 0x4d, 0x2a, 0x8f, 0x7e, 0x3b, 0x9c,
        0x21, 0x4d, 0x6a, 0x5f, 0x03, 0x00, 0x1e, 0xcb);

    // ── Advertising payload ────────────────────────────────────
    // An advertising packet holds 31 bytes. Flags cost 3 and a 128-bit service
    // UUID costs 18, leaving 10 — which the manufacturer block below fills.
    // The NAME does not fit alongside them and travels in the scan response
    // instead, which is why a scanning host must ask for scan responses to see
    // it (active scanning, not passive).
    //
    // The passkey is NEVER advertised. It is never transmitted at all: that is
    // what a passkey is for.

    inline constexpr uint16_t COMPANY_ID = 0xFFFF;      // SIG test id; we hold none
    inline constexpr uint8_t  MFG_TYPE_SLAVE_V1 = 0x01;

    // [ company:u16 LE ][ type:u8 ][ flags:u8 ]
    inline constexpr size_t MFG_DATA_LEN  = 2 + 1 + 1;
    inline constexpr size_t MFG_OFF_TYPE  = 2;
    inline constexpr size_t MFG_OFF_FLAGS = 3;

    // Advertised state, so a host can tell at a glance — before connecting —
    // whether an adapter is free to take or already answers to somebody else.
    // A slave that is bonded to a host keeps advertising: it must stay
    // discoverable to ITS host, and a temporary disconnect is not a release.
    inline constexpr uint8_t FLAG_BONDED   = 1 << 0;   // has at least one bond
    inline constexpr uint8_t FLAG_CONNECTED = 1 << 1;  // a host is connected now
}

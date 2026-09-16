#pragma once

#include "AppProvider.h"
#include "BoardConfig.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"

// ──────────────────────────────────────────────────────────────
// Which paired slave is on which USB COM port.
//
// This is a MAP, and nothing else. It stores an address per port and never asks the
// BLE layer a single question — not whether that slave is bonded, not whether it is in
// range, not whether the link is up. That separation is the point: a port assignment is
// a decision the user made about the host computer's side of the device, and it has to
// survive the slave being switched off, walking out of range, or being released and
// paired again. The panel joins the two at draw time (address -> name, dot, RSSI) and
// is the only place the two ideas meet.
//
// So an assignment can name a slave this host no longer owns. That is not a bug to
// defend against here; it is a row that says so.
//
// HOW MANY PORTS IS THE BOARD'S ANSWER, NOT A SETTING. It was a setting — `usb.ports`,
// 1..4, with a stepper on the panel — and that was a category error: a COM port is a
// USB endpoint budget, so the number of them is fixed by the silicon before the device
// boots and cannot be chosen at runtime by anybody. It now comes from
// BoardConfig::USB_COM_PORTS, which is 1 on the WT-SC01 Plus and says there why.
// Everything below is written for N, and the S3 simply is a board where N is one; a
// future host on a chip with a bigger endpoint budget raises its own constant and
// changes nothing here, in BleHostManager, or in the slave model.
//
// Storage is one setting per port, which buys three things for free: the assignments
// survive a reboot, they show up in the web settings UI, and `settings list` can be
// used to inspect them without a command of our own. NVS keys are capped at 15
// characters (asserted at RUNTIME in Register — an over-long key boot-loops the
// device), so "usb.port1" and friends are deliberately short.
//
// USB itself is NOT here. No TinyUSB, no CDC, no descriptor rebuild — the device does
// not expose a single port yet. What this owns is the model that such a thing would
// read, and the answer to "how many can this host have".
// ──────────────────────────────────────────────────────────────
class UsbPortManager
{
    static constexpr const char* TAG = "UsbPortManager";

public:
    /// How many Comble COM ports this host exposes. A hardware capability, read from
    /// the selected board — zero on a board whose chip has no USB device peripheral we
    /// can drive, in which case this manager registers nothing and refuses every port.
    static constexpr int kMaxPorts = BoardConfig::USB_COM_PORTS;

    /// How many assignment slots are declared. Storage capacity, NOT a capability:
    /// the keys and labels below have to be string literals with static storage, so
    /// they cannot be generated for a count only known per board. Only the first
    /// kMaxPorts of them are ever registered or read; the rest are inert. A board that
    /// wants more than this raises it here and adds the rows.
    static constexpr int kSlotCapacity = 4;
    static_assert(kMaxPorts >= 0 && kMaxPorts <= kSlotCapacity,
                  "BoardConfig::USB_COM_PORTS is outside the declared slot capacity");

    /// A slave address as hex, plus its terminator: "A1B2C3D4E5F6".
    static constexpr size_t kAddrTextLen = 13;

    explicit UsbPortManager(AppProvider& app) : app_(app) {}

    UsbPortManager(const UsbPortManager&) = delete;
    UsbPortManager& operator=(const UsbPortManager&) = delete;
    UsbPortManager(UsbPortManager&&) = delete;
    UsbPortManager& operator=(UsbPortManager&&) = delete;

    void Init();

    /// The slave on `port` (1-based). False when the port is unassigned or out of
    /// range, in which case `addrOut` is untouched.
    bool Assignment(int port, uint8_t addrOut[6]) const;

    /// Put a slave on a port. A slave lives on at most ONE port, so assigning one that
    /// is already somewhere else MOVES it — the alternative is a device where two COM
    /// ports quietly claim the same radio link.
    void Assign(int port, const uint8_t addr[6]);
    void Unassign(int port);

    /// Which port a slave is on, or 0 for none. 1-based, so 0 can mean "nowhere".
    int PortOf(const uint8_t addr[6]) const;

private:
    AppProvider& app_;
    InitState initState_;

    /// One key per port, holding the slave's address as hex, or "" for unassigned.
    inline static StringSetting slot_[kSlotCapacity] = {
        { "usb.port1", "COM1 Slave", "" },
        { "usb.port2", "COM2 Slave", "" },
        { "usb.port3", "COM3 Slave", "" },
        { "usb.port4", "COM4 Slave", "" },
    };

    static bool InRange(int port) { return port >= 1 && port <= kMaxPorts; }

    void Save();

    // ── Commands ──
    RequestError Cmd_Get(CommandContext& ctx);
    RequestError Cmd_Assign(CommandContext& ctx);

    inline static CommandEntry commands_[] = {
        { "usb", "get",    &InvokeCommand<&UsbPortManager::Cmd_Get> },
        { "usb", "assign", &InvokeCommand<&UsbPortManager::Cmd_Assign> },
    };
};

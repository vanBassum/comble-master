#include "UsbPortManager.h"

#include "StruxProvider.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>

namespace
{
    /// "A1B2C3D4E5F6". Upper case, no separators — it is an identifier here, not
    /// something anybody reads off the screen.
    void FormatAddr(const uint8_t addr[6], char* out, size_t cap)
    {
        snprintf(out, cap, "%02X%02X%02X%02X%02X%02X",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    }

    /// The reverse, strictly: 12 hex digits or nothing. A half-parsed address would
    /// silently point a port at the wrong slave, so anything else is a refusal.
    bool ParseAddr(const char* text, uint8_t out[6])
    {
        if (text == nullptr || strlen(text) != 12) return false;

        for (int i = 0; i < 6; ++i)
        {
            char pair[3] = { text[i * 2], text[i * 2 + 1], 0 };
            char* end = nullptr;
            const unsigned long v = strtoul(pair, &end, 16);
            if (end != pair + 2) return false;
            out[i] = static_cast<uint8_t>(v);
        }
        return true;
    }
}

void UsbPortManager::Init()
{
    auto init = initState_.TryBeginInit();
    if (!init)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    // A board with no COM ports registers nothing — no settings a user could edit to no
    // effect, and no `usb` category in `help list` promising a surface that is not
    // there. The manager still exists and still answers, it just answers "no".
    if (kMaxPorts == 0)
    {
        init.SetReady();
        ESP_LOGI(TAG, "Initialized — this board exposes no USB COM ports");
        return;
    }

    StruxProvider& strux = app_.getStrux();
    for (int p = 0; p < kMaxPorts; ++p)
        strux.getSettingsManager().Register({ &slot_[p] });
    strux.getCommandManager().Register(this, commands_);

    init.SetReady();
    ESP_LOGI(TAG, "Initialized — %d COM port(s)", kMaxPorts);
}

void UsbPortManager::Save()
{
    app_.getStrux().getSettingsManager().Save();
}

bool UsbPortManager::Assignment(int port, uint8_t addrOut[6]) const
{
    if (!InRange(port)) return false;

    char text[kAddrTextLen] = {};
    slot_[port - 1].Get(text, sizeof(text));
    return ParseAddr(text, addrOut);
}

int UsbPortManager::PortOf(const uint8_t addr[6]) const
{
    for (int p = 1; p <= kMaxPorts; ++p)
    {
        uint8_t found[6] = {};
        if (Assignment(p, found) && memcmp(found, addr, 6) == 0) return p;
    }
    return 0;
}

void UsbPortManager::Assign(int port, const uint8_t addr[6])
{
    if (!InRange(port)) return;

    // One slave, one port. Clearing the old seat first is what makes a re-assignment a
    // MOVE rather than a duplicate — two ports naming one slave would be a promise the
    // radio cannot keep, and the panel would show the same link twice. Inert while the
    // host has a single port, and deliberately kept: the rule belongs to the model, not
    // to the number of ports the board happens to have.
    const int previous = PortOf(addr);
    if (previous != 0 && previous != port)
        slot_[previous - 1].Set("");

    char text[kAddrTextLen] = {};
    FormatAddr(addr, text, sizeof(text));
    slot_[port - 1].Set(text);
    Save();

    ESP_LOGI(TAG, "COM%d assigned to %s", port, text);
}

void UsbPortManager::Unassign(int port)
{
    if (!InRange(port)) return;
    slot_[port - 1].Set("");
    Save();
    ESP_LOGI(TAG, "COM%d unassigned", port);
}

// ──────────────────────────────────────────────────────────────
// Commands. The same model the panel drives, on the wire — which is how this page
// gets verified without a finger on the glass.
// ──────────────────────────────────────────────────────────────

RequestError UsbPortManager::Cmd_Get(CommandContext& ctx)
{
    RETURN_IF_ERROR(ctx.readArgs());

    auto resp = ctx.reply.object();

    // `ports` is what this host HAS, not what it was told to have. A caller that wants
    // to know whether a second port could ever appear is asking about the hardware, and
    // this is the hardware answering.
    resp.field("ports", static_cast<uint32_t>(kMaxPorts));

    auto list = resp.array("assignments");
    for (int p = 1; p <= kMaxPorts; ++p)
    {
        auto entry = list.object();
        entry.field("port", static_cast<uint32_t>(p));

        uint8_t addr[6] = {};
        if (Assignment(p, addr))
        {
            char text[kAddrTextLen] = {};
            FormatAddr(addr, text, sizeof(text));
            entry.field("slave", text);
        }
        else
        {
            entry.field("slave", "");
        }
    }
    return RequestError::Ok;
}

RequestError UsbPortManager::Cmd_Assign(CommandContext& ctx)
{
    // Optional, defaulting to the first port: on a one-port host naming it every time
    // is noise, and the argument stays so the command does not have to change shape on
    // a host that has more than one.
    uint32_t port = 1;
    char slave[kAddrTextLen] = {};
    RETURN_IF_ERROR(ctx.readArgs(Optional("port", port), Optional("slave", slave)));

    if (!InRange(static_cast<int>(port)))
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "no such port on this host");
        resp.field("ports", static_cast<uint32_t>(kMaxPorts));
        return RequestError::Ok;
    }

    // An empty (or absent) slave clears the port. That is the whole "unassign" verb —
    // a second command for it would be the same write with a different name.
    if (slave[0] == '\0')
    {
        Unassign(static_cast<int>(port));
        auto resp = ctx.reply.object();
        resp.field("ok", true);
        resp.field("port", port);
        resp.field("slave", "");
        return RequestError::Ok;
    }

    uint8_t addr[6] = {};
    if (!ParseAddr(slave, addr))
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "slave must be 12 hex digits, or empty to clear");
        return RequestError::Ok;
    }

    Assign(static_cast<int>(port), addr);

    auto resp = ctx.reply.object();
    resp.field("ok", true);
    resp.field("port", port);
    resp.field("slave", slave);
    return RequestError::Ok;
}

#pragma once

#include "StruxProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"

class Stream;

// ──────────────────────────────────────────────────────────────
// SystemManager owns device identity and lifecycle — and nothing
// else (no timers, no watchdogs; those belong elsewhere):
//   - device.name: exposed to other managers via GetDeviceName()
//     (settings are private to their owner — nobody else reads the
//     key)
//   - the generic system commands: ping / info / reboot
// ──────────────────────────────────────────────────────────────
class SystemManager
{
    static constexpr const char* TAG = "SystemManager";

public:
    explicit SystemManager(StruxProvider& strux);

    SystemManager(const SystemManager&) = delete;
    SystemManager& operator=(const SystemManager&) = delete;
    SystemManager(SystemManager&&) = delete;
    SystemManager& operator=(SystemManager&&) = delete;

    void Init();

    /// Copies the device name into `out`; falls back to the firmware's project name if the
    /// stored value is empty.
    void GetDeviceName(char* out, size_t maxLen);

    /// How this chip is clocked, and whether frequency scaling is on — e.g.
    /// "160 MHz, scaling down to 40 MHz". Reported by `system info` and logged at
    /// boot, because DFS is configured by startup code (CONFIG_PM_DFS_INIT_AUTO in
    /// sdkconfig.defaults) and there is otherwise no evidence that it took.
    void DescribeCpu(char* out, size_t maxLen);

private:
    StruxProvider& strux_;
    InitState initState_;

    // ── Settings (registered with SettingsManager in Init) ──
    /// Empty by default ON PURPOSE: GetDeviceName() falls back to the firmware's
    /// project name, so a fork gets its own identity from project() in the root
    /// CMakeLists and never has to edit this framework file. The name reaches the
    /// AP SSID, the DHCP hostname and the mDNS record, and mdns_hostname_set() is
    /// given it RAW — so whatever a product stores here must be hostname-safe
    /// (no spaces). Backport candidate for the template.
    inline static StringSetting name_{ "device.name", "Device Name", "" };

    // ── WebSocket commands (registered with CommandManager in Init) ──
    RequestError Cmd_Ping(CommandContext& ctx);
    RequestError Cmd_Info(CommandContext& ctx);
    RequestError Cmd_Reboot(CommandContext& ctx);

    inline static CommandEntry commands_[] = {
        { "system", "ping",   &InvokeCommand<&SystemManager::Cmd_Ping> },
        { "system", "info",   &InvokeCommand<&SystemManager::Cmd_Info> },
        { "system", "reboot", &InvokeCommand<&SystemManager::Cmd_Reboot> },
    };
};

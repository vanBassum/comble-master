#include "BleHostManager.h"
#include "CombleBleProtocol.h"

#include "StruxProvider.h"
#include "SettingsManager.h"

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" void ble_store_config_init(void);

static BleHostManager *s_instance = nullptr;

namespace
{
    /// Is this address one we already hold a bond for? Asked of the store rather
    /// than cached: the bond list lives in NVS and the stack may change it
    /// (a repeat-pairing, a store eviction) without telling us.
    bool AddressIsBonded(const uint8_t addr[6])
    {
        ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
        int count = 0;
        if (ble_store_util_bonded_peers(peers, &count,
                                        sizeof(peers) / sizeof(peers[0])) != 0)
            return false;
        for (int i = 0; i < count; ++i)
            if (memcmp(peers[i].val, addr, 6) == 0) return true;
        return false;
    }

    bool HasCombleService(const struct ble_hs_adv_fields &f)
    {
        for (int i = 0; i < f.num_uuids128; ++i)
            if (ble_uuid_cmp(&f.uuids128[i].u, &comble::SERVICE_UUID.u) == 0)
                return true;
        return false;
    }
}

// ──────────────────────────────────────────────────────────────

void BleHostManager::Init()
{
    auto init = initState_.TryBeginInit();
    if (!init)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    s_instance = this;

    // BLE only: hand the Classic-BT controller's RAM back to the heap. On this
    // board that is real internal DRAM, and LVGL wants all of it it can get.
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.sync_cb = &BleHostManager::OnSyncStatic;
    ble_hs_cfg.reset_cb = &BleHostManager::OnResetStatic;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // The mirror image of the slave: it displays the code, we type it. Anything
    // else and the pair silently degrades to "just works", which would hand a
    // shared adapter to whichever host connected first — the exact failure the
    // whole ownership model exists to prevent.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_store_config_init();
    ble_svc_gap_init();

    nimble_port_freertos_init(&BleHostManager::HostTaskStatic);

    LoadRoster();

    init.SetReady();
    ESP_LOGI(TAG, "Initialized — %d slave(s) already owned, %d named",
             BondedCount(), rosterCount_);
}

void BleHostManager::HostTaskStatic(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void BleHostManager::OnSyncStatic()
{
    if (s_instance) s_instance->OnSync();
}

void BleHostManager::OnResetStatic(int reason)
{
    ESP_LOGW("BleHost", "NimBLE host reset, reason=%d", reason);
}

void BleHostManager::OnSync()
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &ownAddrType_);
    ESP_LOGI(TAG, "Radio ready");
    // Deliberately does NOT start scanning. Scanning is a thing the user asked
    // for on a screen, not something the device does forever in the background —
    // it costs radio time that the Wi-Fi side shares an antenna with.
}

// ──────────────────────────────────────────────────────────────

namespace
{
    /// Milliseconds since boot. Wraps after 49 days; the only arithmetic done on it
    /// is a difference between two recent stamps, which wraps correctly.
    inline uint32_t NowMs()
    {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }
}

void BleHostManager::ExpireResults()
{
    const uint32_t now = NowMs();

    LOCK(lock_);
    int kept = 0;
    for (int i = 0; i < resultCount_; ++i)
    {
        if (now - results_[i].lastSeenMs >= kResultHoldMs) continue;
        if (kept != i) results_[kept] = results_[i];
        ++kept;
    }
    resultCount_ = kept;
}

void BleHostManager::StartScan(uint32_t durationMs)
{
    // Aged, NOT cleared. Clearing here is what made a row vanish and come back
    // every few seconds: the list emptied at the top of every window and refilled
    // only when each slave next advertised, which is its own business and can be
    // a second or more away.
    ExpireResults();
    state_ = State::Scanning;

    struct ble_gap_disc_params params = {};
    // ACTIVE: the slave's name lives in its scan response, because the name and
    // a 128-bit service UUID will not both fit in one 31-byte advertisement.
    // A passive scan would find every slave and be able to name none of them.
    params.passive = 0;
    params.filter_duplicates = 0;   // we merge adv + scan-rsp ourselves, by address
    params.itvl = 0;
    params.window = 0;
    params.filter_policy = 0;
    params.limited = 0;

    const int rc = ble_gap_disc(ownAddrType_, durationMs, &params,
                                &BleHostManager::GapEventStatic, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY)
    {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        state_ = State::Failed;
        return;
    }
    // DEBUG, not INFO: a scan starts every few seconds for as long as the device is
    // powered, so at INFO this line and its DISC_COMPLETE partner ARE the console —
    // they push everything that means something off the top. What a scan actually
    // changes is how many slaves are on the air, and that is logged where it changes.
    ESP_LOGD(TAG, "Scanning for %lums", (unsigned long)durationMs);
}

void BleHostManager::StopScan()
{
    ble_gap_disc_cancel();
    if (state_ == State::Scanning) state_ = State::Idle;
}


// ──────────────────────────────────────────────────────────────
// The roster: which slaves this host owns, by name.
//
// A NimBLE bond records an address and the keys, and nothing else — no name. So
// a paired adapter that is switched off or out of range has nothing to show on
// a screen but six hex bytes, which is useless for the one job the display has:
// telling you which of the adapters in the bag this is. Hence a small table of
// our own, written when a pairing succeeds.
// ──────────────────────────────────────────────────────────────

namespace
{
    constexpr const char *kRosterKeyFmt = "ble.slave%d";

    void FormatEntry(char *out, size_t cap, const uint8_t addr[6],
                     uint8_t addrType, const char *name)
    {
        snprintf(out, cap, "%02x%02x%02x%02x%02x%02x:%u:%s",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
                 (unsigned)addrType, name);
    }

    bool ParseEntry(const char *in, uint8_t addr[6], uint8_t &addrType, char *name,
                    size_t nameCap)
    {
        if (in == nullptr || strlen(in) < 15) return false;

        for (int i = 0; i < 6; ++i)
        {
            char byteText[3] = { in[i * 2], in[i * 2 + 1], '\0' };
            char *end = nullptr;
            const unsigned long v = strtoul(byteText, &end, 16);
            if (end != byteText + 2) return false;
            addr[i] = (uint8_t)v;
        }
        if (in[12] != ':') return false;

        addrType = (uint8_t)(in[13] - '0');
        if (in[14] != ':') return false;

        snprintf(name, nameCap, "%s", in + 15);
        return true;
    }
}

void BleHostManager::LoadRoster()
{
    auto &settings = app_.getStrux().getSettingsManager();

    LOCK(lock_);
    rosterCount_ = 0;
    for (int i = 0; i < kMaxPaired; ++i)
    {
        char key[16];
        snprintf(key, sizeof(key), kRosterKeyFmt, i);

        char value[64] = {};
        if (!settings.ReadString(key, value, sizeof(value))) continue;
        if (value[0] == '\0') continue;

        PairedSlave s = {};
        if (!ParseEntry(value, s.addr, s.addrType, s.name, sizeof(s.name)))
        {
            ESP_LOGW(TAG, "Roster slot %d is unreadable — ignoring", i);
            continue;
        }
        roster_[rosterCount_++] = s;
    }
    if (rosterCount_ > 0)
        ESP_LOGI(TAG, "Roster: %d slave(s)", rosterCount_);
}

void BleHostManager::SaveRoster()
{
    auto &settings = app_.getStrux().getSettingsManager();

    // Snapshot under the lock, then write NVS without holding it: a flash write
    // is slow, and this runs on the NimBLE host task where blocking the radio
    // for the duration of an erase is not a trade worth making.
    PairedSlave snapshot[kMaxPaired];
    int count;
    {
        LOCK(lock_);
        count = rosterCount_;
        memcpy(snapshot, roster_, sizeof(PairedSlave) * count);
    }

    for (int i = 0; i < kMaxPaired; ++i)
    {
        char key[16];
        snprintf(key, sizeof(key), kRosterKeyFmt, i);

        if (i < count)
        {
            char value[64];
            FormatEntry(value, sizeof(value), snapshot[i].addr, snapshot[i].addrType,
                        snapshot[i].name);
            settings.WriteString(key, value);
        }
        else
        {
            // Blank the tail rather than leaving it: a shorter roster must not
            // resurrect a released slave from a stale slot on the next boot.
            settings.WriteString(key, "");
        }
    }
    settings.Save();
}

void BleHostManager::RememberPairing()
{
    bool dirty = false;
    {
        LOCK(lock_);

        int slot = -1;
        for (int i = 0; i < rosterCount_; ++i)
            if (memcmp(roster_[i].addr, pendingAddr_, 6) == 0) { slot = i; break; }

        if (slot < 0 && rosterCount_ < kMaxPaired)
            slot = rosterCount_++;      // a new one

        if (slot < 0)
        {
            ESP_LOGW(TAG, "Roster is full (%d) — pairing succeeded but this slave "
                          "will have no name after a reboot", kMaxPaired);
        }
        else
        {
            // Re-pairing with one we already knew just refreshes the name.
            PairedSlave &s = roster_[slot];
            memset(&s, 0, sizeof(s));
            memcpy(s.addr, pendingAddr_, 6);
            s.addrType = pendingAddrType_;
            snprintf(s.name, sizeof(s.name), "%s", lastName_);
            dirty = true;
        }
    }

    // Outside the lock: SaveRoster takes it, and this mutex is not recursive.
    if (dirty) SaveRoster();
}

void BleHostManager::PruneRoster()
{
    // Drop anything the stack no longer has a bond for. The bond list is the
    // authority on ownership; this table only decorates it with names.
    LOCK(lock_);
    int kept = 0;
    for (int i = 0; i < rosterCount_; ++i)
        if (AddressIsBonded(roster_[i].addr))
            roster_[kept++] = roster_[i];
    rosterCount_ = kept;
}

int BleHostManager::GetPaired(PairedSlave *out, int max) const
{
    // Driven by the BOND LIST, which is the authority on ownership — the roster
    // only supplies names. Getting this the wrong way round meant a slave paired
    // by an older firmware (bonded, but never rostered) was owned, filtered out
    // of the "found" list as already-ours, and absent from "paired" for want of a
    // roster row: present on the air and invisible on both lists.
    ble_addr_t peers[kMaxPaired];
    int count = 0;
    if (ble_store_util_bonded_peers(peers, &count, kMaxPaired) != 0) return 0;

    LOCK(lock_);

    int n = 0;
    for (int i = 0; i < count && n < max; ++i)
    {
        PairedSlave &s = out[n];
        memset(&s, 0, sizeof(s));
        memcpy(s.addr, peers[i].val, 6);
        s.addrType = peers[i].type;

        // Name, best effort: what we stored, else what it is calling itself on
        // the air right now, else its address — which is ugly but honest, and
        // self-heals into a real name the first time it is seen advertising.
        const char *name = nullptr;
        for (int j = 0; j < rosterCount_; ++j)
            if (memcmp(roster_[j].addr, s.addr, 6) == 0) { name = roster_[j].name; break; }
        if (name == nullptr || name[0] == 0)
            for (int j = 0; j < resultCount_; ++j)
                if (memcmp(results_[j].addr, s.addr, 6) == 0) { name = results_[j].name; break; }

        if (name != nullptr && name[0] != 0)
            snprintf(s.name, sizeof(s.name), "%s", name);
        else
            snprintf(s.name, sizeof(s.name), "%02X%02X%02X%02X%02X%02X",
                     s.addr[0], s.addr[1], s.addr[2], s.addr[3], s.addr[4], s.addr[5]);

        for (int j = 0; j < resultCount_; ++j)
        {
            if (memcmp(results_[j].addr, s.addr, 6) == 0)
            {
                s.inRange = true;
                s.rssi = results_[j].rssi;
                break;
            }
        }
        ++n;
    }
    return n;
}

void BleHostManager::ReconcileRoster()
{
    // Called when a scan finishes: adopt names for anything we are bonded to but
    // have no name for. That is what turns a bond inherited from an older
    // firmware — or from a roster slot that was lost — back into a named row,
    // without anyone having to re-pair.
    bool dirty = false;
    {
        LOCK(lock_);
        for (int i = 0; i < resultCount_; ++i)
        {
            const Slave &seen = results_[i];
            if (!seen.bonded || seen.name[0] == 0) continue;

            int slot = -1;
            for (int j = 0; j < rosterCount_; ++j)
                if (memcmp(roster_[j].addr, seen.addr, 6) == 0) { slot = j; break; }

            if (slot >= 0)
            {
                if (strcmp(roster_[slot].name, seen.name) == 0) continue;   // already right
            }
            else
            {
                if (rosterCount_ >= kMaxPaired) continue;
                slot = rosterCount_++;
                memset(&roster_[slot], 0, sizeof(PairedSlave));
                memcpy(roster_[slot].addr, seen.addr, 6);
                roster_[slot].addrType = seen.addrType;
            }
            snprintf(roster_[slot].name, sizeof(roster_[slot].name), "%s", seen.name);
            dirty = true;
        }
    }
    // Outside the lock: SaveRoster takes it, and this mutex is not recursive.
    // Only on a real change — this runs after every scan, and NVS is flash.
    if (dirty)
    {
        ESP_LOGI(TAG, "Roster updated from scan");
        SaveRoster();
    }
}

int BleHostManager::GetResults(Slave *out, int max) const
{
    LOCK(lock_);
    const int n = (resultCount_ < max) ? resultCount_ : max;
    memcpy(out, results_, sizeof(Slave) * n);
    return n;
}

int BleHostManager::BondedCount() const
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0;
    if (ble_store_util_bonded_peers(peers, &count,
                                    sizeof(peers) / sizeof(peers[0])) != 0)
        return 0;
    return count;
}

void BleHostManager::AddOrUpdate(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields = {};
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0)
        return;

    const bool ours = HasCombleService(fields);

    LOCK(lock_);

    // Find an existing entry by address. A scan RESPONSE carries the name but
    // not the service UUID, so it only matches something already on the list —
    // which is why entries are keyed by address and merged rather than appended.
    int idx = -1;
    for (int i = 0; i < resultCount_; ++i)
        if (memcmp(results_[i].addr, disc->addr.val, 6) == 0) { idx = i; break; }

    if (idx < 0)
    {
        if (!ours) return;                        // not a Comble slave, and not one we know
        if (resultCount_ >= kMaxResults) return;  // full; it drains as entries age out
        idx = resultCount_++;
        results_[idx] = {};
        memcpy(results_[idx].addr, disc->addr.val, 6);
        results_[idx].addrType = disc->addr.type;
        snprintf(results_[idx].name, sizeof(results_[idx].name), "(unnamed)");
    }

    Slave &s = results_[idx];
    s.rssi = disc->rssi;
    s.lastSeenMs = NowMs();   // the stamp ExpireResults ages against

    if (fields.name != nullptr && fields.name_len > 0)
    {
        const size_t n = (fields.name_len < sizeof(s.name) - 1)
                             ? fields.name_len : sizeof(s.name) - 1;
        memcpy(s.name, fields.name, n);
        s.name[n] = '\0';
    }

    if (fields.mfg_data != nullptr && fields.mfg_data_len >= comble::MFG_DATA_LEN)
    {
        const uint16_t company =
            (uint16_t)fields.mfg_data[0] | ((uint16_t)fields.mfg_data[1] << 8);
        if (company == comble::COMPANY_ID &&
            fields.mfg_data[comble::MFG_OFF_TYPE] == comble::MFG_TYPE_SLAVE_V1)
        {
            const uint8_t f = fields.mfg_data[comble::MFG_OFF_FLAGS];
            s.advBonded = (f & comble::FLAG_BONDED) != 0;
            s.advConnected = (f & comble::FLAG_CONNECTED) != 0;
        }
    }

    s.bonded = AddressIsBonded(s.addr);
}

// ──────────────────────────────────────────────────────────────

bool BleHostManager::BeginPairing(const Slave &slave, uint32_t passkey)
{
    ble_gap_disc_cancel();

    pendingPasskey_ = passkey;
    snprintf(lastName_, sizeof(lastName_), "%s", slave.name);
    memcpy(pendingAddr_, slave.addr, 6);
    pendingAddrType_ = slave.addrType;
    state_ = State::Connecting;

    ble_addr_t addr = {};
    addr.type = slave.addrType;
    memcpy(addr.val, slave.addr, 6);

    const int rc = ble_gap_connect(ownAddrType_, &addr, 10000, nullptr,
                                   &BleHostManager::GapEventStatic, nullptr);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ble_gap_connect failed: %d", rc);
        state_ = State::Failed;
        return false;
    }
    ESP_LOGI(TAG, "Connecting to '%s'", slave.name);
    return true;
}

bool BleHostManager::ReleaseBond(const uint8_t addr[6])
{
    // Both address types, because a bond is stored under whichever the peer
    // used and the caller holds a roster entry that may predate a change.
    int rc = -1;
    for (uint8_t type = 0; type <= 1; ++type)
    {
        ble_addr_t a = {};
        a.type = type;
        memcpy(a.val, addr, 6);
        if (ble_store_util_delete_peer(&a) == 0) rc = 0;
    }

    PruneRoster();
    SaveRoster();
    ESP_LOGW(TAG, "Released a slave (rc=%d) — it is free to pair again", rc);
    return rc == 0;
}

int BleHostManager::GapEventStatic(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    return s_instance ? s_instance->OnGapEvent(event) : 0;
}

int BleHostManager::OnGapEvent(struct ble_gap_event *event)
{
    switch (event->type)
    {
    case BLE_GAP_EVENT_DISC:
        AddOrUpdate(&event->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (state_ == State::Scanning) state_ = State::Idle;
        ESP_LOGD(TAG, "Scan complete: %d slave(s)", resultCount_);

        // The transition, not the poll. resultCount_ is cleared at the top of every
        // scan (see StartScan), so an empty room stays silent after the first line and
        // a slave appearing or leaving still says so exactly once.
        if (resultCount_ != lastLoggedResults_)
        {
            ESP_LOGI(TAG, "Slaves on the air: %d", resultCount_);
            lastLoggedResults_ = resultCount_;
        }

        ReconcileRoster();
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0)
        {
            connHandle_ = event->connect.conn_handle;
            state_ = State::Pairing;
            ESP_LOGI(TAG, "Connected; starting pairing");
            // Ask for encryption explicitly. The slave's write characteristic
            // demands an authenticated link, so without this the first write
            // would fail rather than triggering a pair.
            const int rc = ble_gap_security_initiate(connHandle_);
            if (rc != 0 && rc != BLE_HS_EALREADY)
            {
                ESP_LOGE(TAG, "ble_gap_security_initiate failed: %d", rc);
                state_ = State::Failed;
            }
        }
        else
        {
            ESP_LOGW(TAG, "Connect failed (status %d)", event->connect.status);
            state_ = State::Failed;
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected (reason %d)", event->disconnect.reason);
        // A disconnect during pairing is a failure; after a successful pair it
        // is ordinary — the bond is already in NVS and outlives the link.
        if (state_ == State::Pairing || state_ == State::Connecting)
            state_ = State::Failed;
        else
            state_ = State::Idle;
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0)
        {
            state_ = State::Paired;
            RememberPairing();
            ESP_LOGI(TAG, "Paired with '%s' — bond stored", lastName_);
        }
        else
        {
            // Almost always the wrong six digits.
            state_ = State::Failed;
            ESP_LOGW(TAG, "Pairing failed (status %d) — wrong code?",
                     event->enc_change.status);
            ble_gap_terminate(event->enc_change.conn_handle,
                              BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        // We are KeyboardOnly, so the stack asks us to supply the number the
        // slave is displaying. It was typed on the touchscreen before this.
        if (event->passkey.params.action == BLE_SM_IOACT_INPUT)
        {
            struct ble_sm_io io = {};
            io.action = BLE_SM_IOACT_INPUT;
            io.passkey = pendingPasskey_;
            const int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            ESP_LOGI(TAG, "Supplied pairing code (rc=%d)", rc);
        }
        else
        {
            ESP_LOGW(TAG, "Unexpected passkey action %d",
                     event->passkey.params.action);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
                ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        ESP_LOGW(TAG, "Re-pairing — old bond deleted");
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negotiated: %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

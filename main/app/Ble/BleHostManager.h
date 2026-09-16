#pragma once

#include "AppProvider.h"
#include "InitState.h"
#include "Mutex.h"
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// The host's BLE half: it scans for Comble slaves, pairs with them, and
// remembers which ones it owns.
//
// We are the CENTRAL. Slaves advertise; this scans, filters on our service
// UUID, and presents what it found. Pairing uses the six digits the slave holds
// (and shows on its OLED) — this side is KeyboardOnly, the slave is
// DisplayOnly, and the user carries the number from one to the other. That is
// what makes ownership a deliberate physical act rather than whoever-scanned-first.
//
// Two lists, and they answer different questions:
//
//   the ROSTER    slaves this host owns. Persisted by us, because a NimBLE bond
//                 stores an ADDRESS and nothing else — so a paired adapter that
//                 is out of range or switched off would otherwise have no name
//                 to show, and the home screen would be a column of MACs.
//   the SCAN      what is on the air right now. Transient, cleared per scan.
//
// A slave is normally in both. It is in the roster alone when it is off or out
// of range, and in the scan alone when it belongs to somebody else — or to
// nobody yet.
//
// Threading: every callback below runs on the NimBLE host task, while the UI
// reads from the LVGL task. Both lists are guarded, and the UI is handed a
// COPY — holding a lock across LVGL drawing would couple the radio's timing to
// the screen's.
// ──────────────────────────────────────────────────────────────

class BleHostManager
{
    static constexpr const char *TAG = "BleHost";

public:
    static constexpr int kMaxResults = 16;

    /// How long a slave stays on the list after its last advertisement. Three
    /// scan cycles: the home screen runs 4 s windows with up to a poll's gap
    /// between them, so anything shorter can expire a slave that is merely
    /// between adverts, and the list blinks again. A slave that has actually
    /// left is gone within this plus a cycle, which nobody is waiting on.
    static constexpr uint32_t kResultHoldMs = 12000;
    static constexpr int kMaxPaired = CONFIG_BT_NIMBLE_MAX_BONDS;

    /// One slave as seen on the air. `bonded` is what THIS host knows; `advBonded`
    /// is what the slave advertises about itself — they differ when a slave is
    /// owned by somebody else's host, which is exactly the case worth showing.
    struct Slave
    {
        uint8_t addr[6];
        uint8_t addrType;
        char    name[32];
        int8_t  rssi;
        bool    advBonded;     // the slave says it has an owner
        bool    advConnected;  // the slave says a host is on its link
        bool    bonded;        // WE have a bond with it

        /// Milliseconds since boot when this slave was last heard from. What makes
        /// the list survive a scan boundary: entries are AGED, never cleared.
        uint32_t lastSeenMs;
    };

    /// One slave this host owns. Survives reboots; `inRange` and `rssi` do not,
    /// and are filled in from the most recent scan.
    struct PairedSlave
    {
        uint8_t addr[6];
        uint8_t addrType;
        char    name[32];
        bool    inRange;
        int8_t  rssi;
    };

    enum class State : uint8_t
    {
        Idle,
        Scanning,
        Connecting,
        Pairing,
        Paired,
        Failed,
    };

    explicit BleHostManager(AppProvider &app) : app_(app) {}

    BleHostManager(const BleHostManager &) = delete;
    BleHostManager &operator=(const BleHostManager &) = delete;
    BleHostManager(BleHostManager &&) = delete;
    BleHostManager &operator=(BleHostManager &&) = delete;

    void Init();

    /// Begin (or restart) a discovery window. Results are AGED, not cleared: an
    /// entry survives until it has gone kResultHoldMs without being heard, so a
    /// slave that misses a packet is not dropped mid-window and a slave that is
    /// simply quiet is not dropped at the boundary between windows either.
    ///
    /// The distinction used to be invisible, because a window was something the
    /// user asked for. The home screen now restarts one every time the radio
    /// falls idle, so windows run back to back and clearing at the top of each
    /// one is exactly the flicker this list was built to avoid.
    void StartScan(uint32_t durationMs = 6000);
    void StopScan();

    /// What is on the air. Returns how many were written.
    int GetResults(Slave *out, int max) const;

    /// What this host owns, with liveness from the last scan folded in.
    int GetPaired(PairedSlave *out, int max) const;

    State GetState() const { return state_; }
    const char *LastName() const { return lastName_; }

    /// Connect to a discovered slave and pair with it using `passkey`.
    /// Asynchronous: watch GetState(). The bond is persisted by NimBLE, the
    /// roster entry by us.
    bool BeginPairing(const Slave &slave, uint32_t passkey);

    /// Forget a slave — the deliberate "release" half of the ownership model.
    /// Drops the NimBLE bond AND the roster entry; leaving either behind is how
    /// a released adapter comes back as a ghost.
    bool ReleaseBond(const uint8_t addr[6]);

    int BondedCount() const;

private:
    AppProvider &app_;
    InitState initState_;

    mutable Mutex lock_;
    Slave results_[kMaxResults] = {};
    int resultCount_ = 0;

    /// Drop results older than kResultHoldMs, compacting in place. Caller holds
    /// no lock; this takes it.
    void ExpireResults();

    /// What the last INFO line said, so a scan that finds the same room as the one
    /// before it says nothing. -1 because 0 is a real answer worth logging once.
    int lastLoggedResults_ = -1;

    PairedSlave roster_[kMaxPaired] = {};
    int rosterCount_ = 0;

    volatile State state_ = State::Idle;
    char lastName_[32] = {};
    uint8_t pendingAddr_[6] = {};
    uint8_t pendingAddrType_ = 0;

    uint8_t ownAddrType_ = 0;
    uint16_t connHandle_ = 0;
    uint32_t pendingPasskey_ = 0;

    void OnSync();
    int OnGapEvent(struct ble_gap_event *event);
    void AddOrUpdate(const struct ble_gap_disc_desc *disc);

    // ── Roster persistence ──
    // Stored as one string per slot under `ble.slaveN`, not as a registered
    // setting: this is internal state, not something a user should be editing in
    // the settings UI. Format is "aabbccddeeff:T:Name".
    void LoadRoster();
    void SaveRoster();
    void RememberPairing();
    void PruneRoster();

    /// Adopt names for slaves we are bonded to but have no name for.
    /// Runs after each scan; writes NVS only when something changed.
    void ReconcileRoster();

    static void HostTaskStatic(void *param);
    static void OnSyncStatic();
    static void OnResetStatic(int reason);
    static int GapEventStatic(struct ble_gap_event *event, void *arg);
};

#pragma once

#include "AppProvider.h"
#include "BoardContext.h"
#include "StruxProvider.h"
#include "LedManager/LedManager.h"
#include "Ble/BleHostManager.h"
#include "Ui/UiManager.h"

// The application layer's context: owns this product's managers and answers AppProvider.
//
// This is the file a fork edits. Strux's own managers, their init order and their wiring
// all live one layer down in StruxContext, so pulling an improvement from the template
// does not touch anything here — which is the whole reason the layers were split.
//
// Adding an application manager means: create the class taking AppProvider&, add an
// accessor to AppProvider, add the member here, and call its Init() below. The framework
// does not need to be told it exists; the manager registers its own commands and
// settings into Strux from its Init().
class AppContext : public AppProvider
{
public:
    AppContext(BoardContext& board, StruxProvider& strux)
        : board_(board), strux_(strux) {}

    ~AppContext() = default;
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    /// Bring the application up. Called last: every manager here registers into the
    /// framework, so the framework has to be ready before any of this runs.
    void Init()
    {
        ledManager_.Init();
        // Before the UI: the first thing the slaves screen does is ask
        // this how many slaves are already owned.
        bleHost_.Init();
        ui_.Init();
    }

    StruxProvider& getStrux() override { return strux_; }
    BoardContext& getBoard() override { return board_; }
    LedManager& getLedManager() override { return ledManager_; }
    BleHostManager& getBleHost() override { return bleHost_; }

private:
    BoardContext& board_;
    StruxProvider& strux_;

    LedManager ledManager_{*this};

    // The touchscreen UI. Deliberately absent from AppProvider: nothing else
    // calls into it yet, and a manager earns its accessor when a peer needs it.
    BleHostManager bleHost_{*this};
    UiManager ui_{*this};
};

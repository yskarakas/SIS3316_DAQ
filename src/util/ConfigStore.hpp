// ===========================================================================
//  ConfigStore.hpp — process-wide "active board configuration" (thread-safe).
//
//  Single source of truth shared between the Config tab (which edits it) and the
//  Live DAQ tab (whose "Configure on Start" applies it). Without this the two
//  tabs each kept a private BoardConfig and Live DAQ silently overwrote the
//  Config tab's trigger/threshold settings on every START.
// ===========================================================================
#pragma once
#include <mutex>
#include <atomic>
#include "../hw/Sis3316Config.hpp"

namespace sis {

class ConfigStore {
public:
    static ConfigStore& instance() { static ConfigStore s; return s; }

    BoardConfig get() const { std::lock_guard<std::mutex> lk(fMx); return fCfg; }
    void set(const BoardConfig& c) { std::lock_guard<std::mutex> lk(fMx); fCfg = c; }

    // Whether the Live DAQ worker currently holds the board's single link grant.
    // The Config tab checks this before opening its own connection so it cannot
    // steal the grant mid-run (which floods the log with "lost grant" errors).
    bool liveActive() const { return fLive.load(); }
    void setLiveActive(bool on) { fLive.store(on); }

private:
    ConfigStore() = default;
    mutable std::mutex fMx;
    BoardConfig fCfg;              // defaults from BoardConfig()
    std::atomic<bool> fLive{false};
};

} // namespace sis

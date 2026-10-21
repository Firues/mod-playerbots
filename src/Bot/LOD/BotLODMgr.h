/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTLODMGR_H
#define PLAYERBOTS_BOTLODMGR_H

#include "BotRecord.h"

#include <ctime>
#include <unordered_map>

class ChatHandler;
class Player;

class BotLODMgr
{
public:
    static BotLODMgr& Instance();

    void Initialize();
    void Shutdown();
    void Update(uint32 diff);

    void OnBotLogin(Player* bot);
    void OnBotLogout(Player* bot);

    [[nodiscard]] bool IsEnabled() const { return _enabled; }
    [[nodiscard]] uint32 GetPopulation() const { return static_cast<uint32>(_records.size()); }
    void PrintStats(ChatHandler* handler) const;

private:
    BotLODMgr() = default;
    ~BotLODMgr() = default;

    BotLODMgr(BotLODMgr const&) = delete;
    BotLODMgr& operator=(BotLODMgr const&) = delete;
    BotLODMgr(BotLODMgr&&) = delete;
    BotLODMgr& operator=(BotLODMgr&&) = delete;

    void LoadRecords();
    void RefreshHotSet(time_t now);
    void UpdateCold(uint32 diff);
    void ClearStalePending(time_t now);
    bool Materialize(BotRecord& record, time_t now);
    void Dematerialize(BotRecord& record, time_t now);
    [[nodiscard]] bool CanDematerialize(Player* bot) const;

    std::unordered_map<ObjectGuid::LowType, BotRecord> _records;
    std::unordered_map<ObjectGuid::LowType, time_t> _pendingSince;
    bool _enabled = false;
    uint32 _lastUpdateMs = 0;
    uint32 _lastColdMs = 0;
};

#define sBotLODMgr BotLODMgr::Instance()

#endif

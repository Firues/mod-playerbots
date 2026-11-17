/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTLODMGR_H
#define PLAYERBOTS_BOTLODMGR_H

#include "BotRecord.h"

#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

class ChatHandler;
class Player;
class WhoListPlayerInfo;

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

    // Who list extension: appends one entry per cold (non-materialized) record so the
    // in-game who window reflects the full population, not only live bots near players.
    void AppendWhoEntries(std::vector<WhoListPlayerInfo>& storage) const;

    // Group invitation targeting a cold record: materialize the named bot and deliver the
    // invitation after it logs in. Returns false when the name is not a cold record.
    bool TryInviteColdBot(Player* inviter, std::string const& memberName);

private:
    BotLODMgr() = default;
    ~BotLODMgr() = default;

    BotLODMgr(BotLODMgr const&) = delete;
    BotLODMgr& operator=(BotLODMgr const&) = delete;
    BotLODMgr(BotLODMgr&&) = delete;
    BotLODMgr& operator=(BotLODMgr&&) = delete;

    struct WhoBotIdentity
    {
        std::wstring name;
        std::wstring guild;
        std::string nameNarrow;
        std::string guildNarrow;
        uint8 gender = 0;
    };

    struct PendingInvite
    {
        ObjectGuid inviterGuid;
        time_t readyAt = 0;
    };

    void LoadRecords();
    void LoadWhoNames();
    void ProcessPendingInvites(time_t now);
    [[nodiscard]] uint32 StableLevelForRecord(BotRecord const& record, uint32 ceiling) const;
    void RefreshHotSet(time_t now);
    void ConcentrateRecords(time_t now);
    void ReviveDead(time_t now);
    void UpdateCold(uint32 diff);
    void ClearStalePending(time_t now);
    bool Materialize(BotRecord& record, time_t now);
    void Dematerialize(BotRecord& record, time_t now);
    [[nodiscard]] bool CanDematerialize(Player* bot) const;

    // Snapshot of the real (non-headless) players LOD steers the crowd around, refreshed
    // by RefreshHotSet each LOD tick.
    struct RealPlayerPosition
    {
        uint32 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint32 zoneId = 0;
        uint32 level = 0;
    };

    std::unordered_map<ObjectGuid::LowType, BotRecord> _records;
    std::unordered_map<ObjectGuid::LowType, time_t> _pendingSince;
    std::unordered_map<ObjectGuid::LowType, time_t> _deadSince;
    std::unordered_map<ObjectGuid::LowType, WhoBotIdentity> _whoData;
    std::unordered_map<std::string, ObjectGuid::LowType> _whoNameIndex;
    std::unordered_map<ObjectGuid::LowType, PendingInvite> _pendingInvites;
    std::vector<RealPlayerPosition> _players;
    bool _enabled = false;
    uint32 _lastUpdateMs = 0;
    uint32 _lastColdMs = 0;
};

#define sBotLODMgr BotLODMgr::Instance()

#endif

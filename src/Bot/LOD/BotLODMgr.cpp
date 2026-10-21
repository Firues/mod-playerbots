/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotLODMgr.h"
#include "BotRecord.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotAI.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldSession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr uint32 LOD_ACCOUNT_QUERY_CHUNK = 500;
constexpr uint32 LOD_PENDING_TIMEOUT = 30;
constexpr float LOD_TWO_PI = 6.2831853f;

struct RealPlayerPosition
{
    uint32 mapId;
    float x;
    float y;
};
}

BotLODMgr& BotLODMgr::Instance()
{
    static BotLODMgr instance;
    return instance;
}

void BotLODMgr::Initialize()
{
    _enabled = sPlayerbotAIConfig.LODEnabled && sPlayerbotAIConfig.Enabled;
    _records.clear();
    _pendingSince.clear();
    _lastUpdateMs = getMSTime();
    _lastColdMs = _lastUpdateMs;

    if (!_enabled)
    {
        LOG_INFO("playerbots", "Bot LOD: disabled");
        return;
    }

    if (sPlayerbotAIConfig.RandomBotAccounts.empty())
    {
        LOG_ERROR("playerbots", "Bot LOD: no random bot accounts configured; LOD disabled");
        _enabled = false;
        return;
    }

    LoadRecords();
    if (_records.empty())
    {
        LOG_ERROR("playerbots", "Bot LOD: no bot records loaded; LOD disabled");
        _enabled = false;
        return;
    }

    LOG_INFO("playerbots", "Bot LOD: loaded {} records (population {}, max active {})",
             _records.size(), sPlayerbotAIConfig.LODPopulation, sPlayerbotAIConfig.LODMaxActiveBots);
}

void BotLODMgr::LoadRecords()
{
    std::vector<BotRecord> loaded;
    auto const& accounts = sPlayerbotAIConfig.RandomBotAccounts;
    loaded.reserve(accounts.size() * 10);

    for (size_t offset = 0; offset < accounts.size(); offset += LOD_ACCOUNT_QUERY_CHUNK)
    {
        std::string accountList;
        size_t const end = std::min(accounts.size(), offset + LOD_ACCOUNT_QUERY_CHUNK);
        for (size_t i = offset; i < end; ++i)
        {
            if (!accountList.empty())
                accountList += ',';

            accountList += std::to_string(accounts[i]);
        }

        std::string const query = Acore::StringFormat(
            "SELECT guid, account, level, xp, money, race, class, map, zone, "
            "position_x, position_y, position_z, orientation "
            "FROM characters WHERE deleteDate IS NULL AND account IN ({})",
            accountList);

        QueryResult result = CharacterDatabase.Query(query);
        if (!result)
            continue;

        do
        {
            Field* fields = result->Fetch();

            BotRecord record;
            record.guid = fields[0].Get<uint32>();
            record.accountId = fields[1].Get<uint32>();
            record.level = fields[2].Get<uint32>();
            record.xp = fields[3].Get<uint32>();
            record.money = fields[4].Get<uint32>();
            record.race = fields[5].Get<uint8>();
            record.classId = fields[6].Get<uint8>();
            record.mapId = fields[7].Get<uint32>();
            record.zoneId = fields[8].Get<uint32>();
            record.x = fields[9].Get<float>();
            record.y = fields[10].Get<float>();
            record.z = fields[11].Get<float>();
            record.orientation = fields[12].Get<float>();

            if (record.guid)
                loaded.push_back(record);
        } while (result->NextRow());
    }

    std::mt19937 rng(static_cast<uint32>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::shuffle(loaded.begin(), loaded.end(), rng);

    uint32 const population = sPlayerbotAIConfig.LODPopulation;
    if (population && loaded.size() > population)
    {
        LOG_INFO("playerbots", "Bot LOD: truncating {} records to configured population {}",
                 loaded.size(), population);
        loaded.resize(population);
    }

    _records.reserve(loaded.size());
    for (BotRecord const& record : loaded)
    {
        bool const inserted = _records.emplace(record.guid, record).second;
        if (inserted)
            sRandomPlayerbotMgr.RegisterRandomBot(record.guid);
    }
}

void BotLODMgr::Shutdown()
{
    if (_enabled)
        LOG_INFO("playerbots", "Bot LOD: shutting down with {} records", _records.size());

    _records.clear();
    _pendingSince.clear();
    _enabled = false;
}

void BotLODMgr::Update(uint32 /*diff*/)
{
    if (!_enabled)
        return;

    uint32 const nowMs = getMSTime();
    uint32 const sinceUpdate = nowMs - _lastUpdateMs;
    if (sinceUpdate < sPlayerbotAIConfig.LODUpdateInterval)
        return;

    _lastUpdateMs = nowMs;

    uint32 const sinceCold = nowMs - _lastColdMs;
    if (sinceCold >= sPlayerbotAIConfig.LODColdTickInterval * 1000)
    {
        UpdateCold(sinceCold);
        _lastColdMs = nowMs;
    }

    time_t const now = time(nullptr);
    ClearStalePending(now);
    RefreshHotSet(now);
}

void BotLODMgr::RefreshHotSet(time_t now)
{
    std::vector<RealPlayerPosition> players;
    for (auto const& entry : ObjectAccessor::GetPlayers())
    {
        Player* player = entry.second;
        if (!player || !player->IsInWorld() || !player->GetSession() || player->GetSession()->IsHeadless())
            continue;

        if (!IsRealPlayer(player))
            continue;

        Map* map = player->GetMap();
        if (!map || map->IsDungeon() || map->IsBattlegroundOrArena())
            continue;

        if (sObjectMgr->IsTransportMap(map->GetId()))
            continue;

        auto const& allowedMaps = sPlayerbotAIConfig.RandomBotMaps;
        if (!allowedMaps.empty() &&
            std::find(allowedMaps.begin(), allowedMaps.end(), map->GetId()) == allowedMaps.end())
            continue;

        players.push_back({map->GetId(), player->GetPositionX(), player->GetPositionY()});
    }

    float const activeSq = sPlayerbotAIConfig.LODActiveRadius * sPlayerbotAIConfig.LODActiveRadius;
    float const deactivateSq =
        sPlayerbotAIConfig.LODDeactivateRadius * sPlayerbotAIConfig.LODDeactivateRadius;

    struct Candidate
    {
        float distanceSq;
        ObjectGuid::LowType guid;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(_records.size());
    uint32 logoutBudget = sPlayerbotAIConfig.LODMaxLogoutsPerTick;

    for (auto& entry : _records)
    {
        BotRecord& record = entry.second;
        record.nearestPlayerSq = std::numeric_limits<float>::max();

        for (RealPlayerPosition const& player : players)
        {
            if (player.mapId != record.mapId)
                continue;

            float const dx = player.x - record.x;
            float const dy = player.y - record.y;
            float const distanceSq = dx * dx + dy * dy;
            if (distanceSq < record.nearestPlayerSq)
                record.nearestPlayerSq = distanceSq;
        }

        if (record.online)
        {
            if (record.nearestPlayerSq <= deactivateSq)
                record.hotDeadline = now + sPlayerbotAIConfig.LODDematerializeDelay;
            else if (now >= record.hotDeadline && logoutBudget)
            {
                Dematerialize(record, now);
                --logoutBudget;
            }
        }
        else if (record.nearestPlayerSq <= activeSq)
        {
            candidates.push_back({record.nearestPlayerSq, record.guid});
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](Candidate const& left, Candidate const& right) { return left.distanceSq < right.distanceSq; });

    uint32 hotCount = static_cast<uint32>(_pendingSince.size());
    for (auto const& entry : _records)
        if (entry.second.online)
            ++hotCount;

    uint32 loginBudget = sPlayerbotAIConfig.LODMaxLoginsPerTick;
    for (Candidate const& candidate : candidates)
    {
        if (hotCount >= sPlayerbotAIConfig.LODMaxActiveBots || !loginBudget)
            break;

        auto it = _records.find(candidate.guid);
        if (it == _records.end() || it->second.online || _pendingSince.contains(candidate.guid))
            continue;

        if (!Materialize(it->second, now))
            continue;

        --loginBudget;
        ++hotCount;
    }
}

bool BotLODMgr::Materialize(BotRecord& record, time_t now)
{
    if (record.online || _pendingSince.contains(record.guid))
        return false;

    MapEntry const* mapEntry = sMapStore.LookupEntry(record.mapId);
    if (!mapEntry || mapEntry->IsDungeon() || mapEntry->IsBattlegroundOrArena() ||
        sObjectMgr->IsTransportMap(record.mapId))
        return false;

    auto const& allowedMaps = sPlayerbotAIConfig.RandomBotMaps;
    if (!allowedMaps.empty() &&
        std::find(allowedMaps.begin(), allowedMaps.end(), record.mapId) == allowedMaps.end())
        return false;

    _pendingSince[record.guid] = now;
    record.hot = true;
    record.hotDeadline = now + sPlayerbotAIConfig.LODDematerializeDelay;

    sRandomPlayerbotMgr.AddPlayerBot(ObjectGuid::Create<HighGuid::Player>(record.guid), 0);
    return true;
}

void BotLODMgr::Dematerialize(BotRecord& record, time_t now)
{
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(record.guid);
    if (!bot)
    {
        record.online = false;
        record.hot = false;
        return;
    }

    if (!CanDematerialize(bot))
    {
        record.hotDeadline = now + sPlayerbotAIConfig.LODDematerializeDelay;
        return;
    }

    sRandomPlayerbotMgr.LogoutPlayerBot(bot->GetGUID());
    record.online = false;
    record.hot = false;
}

bool BotLODMgr::CanDematerialize(Player* bot) const
{
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->GetMap())
        return false;

    if (bot->IsInCombat() || bot->isDead() || bot->GetGroup())
        return false;

    if (bot->InBattleground() || bot->InBattlegroundQueue())
        return false;

    if (bot->GetMap()->IsDungeon() || bot->GetMap()->IsBattlegroundOrArena())
        return false;

    if (bot->GetMap()->GetInstanceId() != 0 || sObjectMgr->IsTransportMap(bot->GetMapId()))
        return false;

    if (bot->IsInFlight())
        return false;

    WorldSession* session = bot->GetSession();
    return session && !session->IsLoggingOut();
}

void BotLODMgr::OnBotLogin(Player* bot)
{
    if (!_enabled || !bot)
        return;

    ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
    auto it = _records.find(guid);
    if (it == _records.end())
        return;

    BotRecord& record = it->second;
    time_t const now = time(nullptr);
    record.online = true;
    record.hot = true;
    record.hotDeadline = now + sPlayerbotAIConfig.LODDematerializeDelay;
    _pendingSince.erase(guid);

    if (bot->GetMapId() != record.mapId)
    {
        LOG_WARN("playerbots", "Bot LOD: bot {} logged in on map {} but record says {}; keeping DB position",
                 bot->GetName(), bot->GetMapId(), record.mapId);
        return;
    }

    Map* map = bot->GetMap();
    if (!map)
        return;

    float z = map->GetHeight(bot->GetPhaseMask(), record.x, record.y, record.z, true, MAX_HEIGHT);
    if (z <= INVALID_HEIGHT)
        z = record.z;

    record.z = z;
    if (!bot->TeleportTo(bot->GetMapId(), record.x, record.y, z, record.orientation))
    {
        LOG_WARN("playerbots", "Bot LOD: bot {} could not be moved to its cold position on map {}",
                 bot->GetName(), record.mapId);
    }
}

void BotLODMgr::OnBotLogout(Player* bot)
{
    if (!_enabled || !bot)
        return;

    ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
    auto it = _records.find(guid);
    if (it == _records.end())
        return;

    BotRecord& record = it->second;
    record.level = bot->GetLevel();
    record.xp = bot->GetUInt32Value(PLAYER_XP);
    record.money = bot->GetMoney();
    record.online = false;
    record.hot = false;
    record.hasTarget = false;
    _pendingSince.erase(guid);

    Map* map = bot->GetMap();
    if (map && !map->IsDungeon() && !map->IsBattlegroundOrArena() &&
        !sObjectMgr->IsTransportMap(map->GetId()))
    {
        record.mapId = bot->GetMapId();
        record.zoneId = bot->GetZoneId();
        record.x = bot->GetPositionX();
        record.y = bot->GetPositionY();
        record.z = bot->GetPositionZ();
        record.orientation = bot->GetOrientation();
    }
}

void BotLODMgr::UpdateCold(uint32 diff)
{
    float const seconds = static_cast<float>(diff) / 1000.0f;
    if (seconds <= 0.0f)
        return;

    for (auto& entry : _records)
    {
        BotRecord& record = entry.second;
        if (record.online)
            continue;

        if (!record.hasTarget)
        {
            float const angle = frand(0.0f, LOD_TWO_PI);
            float const distance = frand(0.0f, sPlayerbotAIConfig.LODColdWanderRadius);
            record.targetX = record.x + std::cos(angle) * distance;
            record.targetY = record.y + std::sin(angle) * distance;
            record.targetZ = record.z;
            record.hasTarget = true;
        }

        float const dx = record.targetX - record.x;
        float const dy = record.targetY - record.y;
        float const distance = std::sqrt(dx * dx + dy * dy);
        if (distance <= 0.1f)
        {
            record.x = record.targetX;
            record.y = record.targetY;
            record.z = record.targetZ;
            record.hasTarget = false;
            continue;
        }

        float const step = sPlayerbotAIConfig.LODColdMoveSpeed * seconds;
        if (step >= distance)
        {
            record.x = record.targetX;
            record.y = record.targetY;
            record.z = record.targetZ;
            record.hasTarget = false;
        }
        else
        {
            record.x += dx / distance * step;
            record.y += dy / distance * step;
        }

        record.orientation = std::atan2(record.targetY - record.y, record.targetX - record.x);
    }
}

void BotLODMgr::ClearStalePending(time_t now)
{
    for (auto it = _pendingSince.begin(); it != _pendingSince.end();)
    {
        if (now - it->second <= LOD_PENDING_TIMEOUT)
        {
            ++it;
            continue;
        }

        LOG_WARN("playerbots", "Bot LOD: login timed out for bot {}", it->first);
        it = _pendingSince.erase(it);
    }
}

void BotLODMgr::PrintStats(ChatHandler* handler) const
{
    uint32 online = 0;
    uint32 hot = 0;
    for (auto const& entry : _records)
    {
        BotRecord const& record = entry.second;
        if (record.online)
            ++online;
        if (record.hot)
            ++hot;
    }

    std::string const message = Acore::StringFormat(
        "Bot LOD: enabled={} population={} online={} hot={} pending={} cold={} recordSize={}",
        _enabled, _records.size(), online, hot, _pendingSince.size(), _records.size() - online,
        sizeof(BotRecord));

    LOG_INFO("playerbots", "{}", message);
    if (handler)
        handler->SendSysMessage(message.c_str());
}

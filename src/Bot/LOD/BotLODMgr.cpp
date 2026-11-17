/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotLODMgr.h"
#include "BotRecord.h"
#include "Cache/WhoListCacheMgr.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotFactory.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotAI.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "Util.h"
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

// AreaTableEntry::team: 2 = Alliance-controlled, 4 = Horde-controlled, anything else is shared.
bool IsAllianceRace(uint8 race)
{
    return race == RACE_HUMAN || race == RACE_DWARF || race == RACE_NIGHTELF || race == RACE_GNOME ||
           race == RACE_DRAENEI;
}

bool RaceAllowedInZone(uint8 race, int32 zoneTeam)
{
    if (zoneTeam == 4)
        return !IsAllianceRace(race);
    if (zoneTeam == 2)
        return IsAllianceRace(race);
    return true;
}
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
    _deadSince.clear();
    _players.clear();
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

    LoadWhoNames();

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

// Names, genders and guild names of the whole bot population, used to render cold
// records in the who list (the who cache has no Player object to pull them from).
void BotLODMgr::LoadWhoNames()
{
    _whoData.clear();

    auto const& accounts = sPlayerbotAIConfig.RandomBotAccounts;
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

        QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
            "SELECT c.guid, c.name, c.gender FROM characters c WHERE c.deleteDate IS NULL AND c.account IN ({})",
            accountList));
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                ObjectGuid::LowType const guid = fields[0].Get<uint32>();
                std::string const name = fields[1].Get<std::string>();
                uint8 const gender = fields[2].Get<uint8>();

                WhoBotIdentity& data = _whoData[guid];
                std::wstring wide;
                if (Utf8toWStr(name, wide))
                {
                    data.name = wide;
                    wstrToLower(data.name);
                }
                data.nameNarrow = name;
                data.gender = gender;
            } while (result->NextRow());
        }
        else
        {
            LOG_ERROR("playerbots", "Bot LOD: failed to load bot character names (chunk {}-{})", offset, end);
            return;
        }

        QueryResult guilds = CharacterDatabase.Query(Acore::StringFormat(
            "SELECT c.guid, g.name FROM characters c "
            "JOIN guild_member gm ON gm.guid = c.guid "
            "JOIN guild g ON g.guildid = gm.guildid "
            "WHERE c.deleteDate IS NULL AND c.account IN ({})",
            accountList));
        if (guilds)
        {
            do
            {
                Field* fields = guilds->Fetch();
                ObjectGuid::LowType const guid = fields[0].Get<uint32>();
                std::string const guildName = fields[1].Get<std::string>();

                auto it = _whoData.find(guid);
                if (it == _whoData.end())
                    continue;

                std::wstring wide;
                if (Utf8toWStr(guildName, wide))
                {
                    it->second.guild = wide;
                    wstrToLower(it->second.guild);
                    it->second.guildNarrow = guildName;
                }
            } while (guilds->NextRow());
        }
    }

    LOG_INFO("playerbots", "Bot LOD: loaded who-list identities for {} bots", _whoData.size());

    // Lowercase name -> guid index for offline invitations (the client sends the typed name).
    _whoNameIndex.clear();
    for (auto const& [guid, identity] : _whoData)
        _whoNameIndex[identity.nameNarrow] = guid;
}

void BotLODMgr::Shutdown()
{
    if (_enabled)
        LOG_INFO("playerbots", "Bot LOD: shutting down with {} records", _records.size());

    _records.clear();
    _pendingSince.clear();
    _deadSince.clear();
    _players.clear();
    _whoData.clear();
    _whoNameIndex.clear();
    _pendingInvites.clear();
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
    ReviveDead(now);
    RefreshHotSet(now);
    ConcentrateRecords(now);
    ProcessPendingInvites(now);
}

void BotLODMgr::RefreshHotSet(time_t now)
{
    _players.clear();
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

        _players.push_back({map->GetId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                            map->GetZoneId(PHASEMASK_NORMAL, player->GetPositionX(), player->GetPositionY(),
                                           player->GetPositionZ()),
                            player->GetLevel()});
    }

    float const activeSq = sPlayerbotAIConfig.LODActiveRadius * sPlayerbotAIConfig.LODActiveRadius;
    float const minSpawnSq = sPlayerbotAIConfig.LODMinSpawnDistance * sPlayerbotAIConfig.LODMinSpawnDistance;
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

        for (RealPlayerPosition const& player : _players)
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
            // Materialize only beyond the client render distance: a bot appearing
            // a few yards in front of a running player breaks the illusion. Closer
            // shells stay cold until the player moves away, then go live unseen.
            if (record.nearestPlayerSq < minSpawnSq)
                continue;

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

// Crowd steering: keep a minimum number of records around each real player so every zone they
// visit feels populated. Cold records are cheap to move - only their position fields change -
// and they materialize into full bots on a following tick once inside the active radius.
void BotLODMgr::ConcentrateRecords(time_t /*now*/)
{
    if (!sPlayerbotAIConfig.LODConcentrateInPlayerZone || !sPlayerbotAIConfig.LODBotsNearPlayerTarget ||
        _players.empty())
        return;

    float const activeSq = sPlayerbotAIConfig.LODActiveRadius * sPlayerbotAIConfig.LODActiveRadius;
    // Place inside the materialization radius (with margin) so relocated records qualify as
    // hot on the next pass, but beyond MinSpawnDistance so they never appear right next to
    // a player. LODConcentrateRadius is clamped to LODActiveRadius at config load.
    float const spawnFloor = std::max(20.0f, sPlayerbotAIConfig.LODMinSpawnDistance * 1.1f);
    float const spread = std::max(spawnFloor, sPlayerbotAIConfig.LODConcentrateRadius * 0.9f);
    uint32 budget = sPlayerbotAIConfig.LODMaxLoginsPerTick;

    for (RealPlayerPosition const& player : _players)
    {
        Map* map = sMapMgr->FindBaseMap(player.mapId);
        if (!map)
            continue;

        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(player.zoneId);

        // Count every record near this player, online or not: materialized bots keep counting
        // once hot, otherwise the deficit would keep pulling fresh records every tick.
        uint32 nearCount = 0;
        for (auto const& entry : _records)
        {
            BotRecord const& record = entry.second;
            if (record.mapId != player.mapId)
                continue;

            if (record.online)
            {
                Player* bot = sRandomPlayerbotMgr.GetPlayerBot(record.guid);
                if (!bot || bot->GetMapId() != player.mapId)
                    continue;

                float const bdx = bot->GetPositionX() - player.x;
                float const bdy = bot->GetPositionY() - player.y;
                if (bdx * bdx + bdy * bdy <= activeSq)
                    ++nearCount;
            }
            else
            {
                float const rdx = record.x - player.x;
                float const rdy = record.y - player.y;
                if (rdx * rdx + rdy * rdy <= activeSq)
                    ++nearCount;
            }
        }

        if (nearCount >= sPlayerbotAIConfig.LODBotsNearPlayerTarget)
            continue;

        uint32 deficit = sPlayerbotAIConfig.LODBotsNearPlayerTarget - nearCount;
        if (deficit > budget)
            deficit = budget;

        std::vector<ObjectGuid::LowType> eligible;
        for (auto const& entry : _records)
        {
            BotRecord const& record = entry.second;
            if (record.online || _pendingSince.contains(record.guid))
                continue;

            float const dx = record.x - player.x;
            float const dy = record.y - player.y;
            if (record.mapId == player.mapId && dx * dx + dy * dy <= activeSq)
                continue;

            // Respect faction-controlled zones, matching the teleport checks in RandomTeleport.
            if (zone && !RaceAllowedInZone(record.race, zone->team))
                continue;

            eligible.push_back(record.guid);
        }

        while (deficit && !eligible.empty())
        {
            size_t const idx = urand(0, eligible.size() - 1);
            auto it = _records.find(eligible[idx]);
            eligible[idx] = eligible.back();
            eligible.pop_back();

            if (it == _records.end())
                continue;

            BotRecord& record = it->second;
            float const angle = frand(0.0f, LOD_TWO_PI);
            float const distance = frand(spawnFloor, spread);
            float const x = player.x + std::cos(angle) * distance;
            float const y = player.y + std::sin(angle) * distance;

            // The grid may not be loaded here; materialization re-checks the height on login
            // (OnBotLogin) and falls back to the stored z, so an approximate z is fine.
            float z = map->GetHeight(PHASEMASK_NORMAL, x, y, player.z, true, MAX_HEIGHT);
            if (z <= INVALID_HEIGHT)
                z = player.z;

            record.mapId = player.mapId;
            record.zoneId = player.zoneId;
            record.x = x;
            record.y = y;
            record.z = z;
            record.orientation = frand(0.0f, LOD_TWO_PI);
            record.hasTarget = false;
            record.targetX = x;
            record.targetY = y;
            record.targetZ = z;

            --deficit;
            --budget;
        }

        if (!budget)
        {
            LOG_DEBUG("playerbots", "Bot LOD: concentration relocation budget exhausted this tick");
            break;
        }
    }
}

// Bring killed bots back after a delay. CanDematerialize rejects the dead, so without this a
// bot killed by a player would stay materialized forever, occupying a MaxActiveBots slot.
void BotLODMgr::ReviveDead(time_t now)
{
    if (!sPlayerbotAIConfig.LODReviveDelay)
        return;

    for (auto& entry : _records)
    {
        BotRecord& record = entry.second;
        if (!record.online)
        {
            _deadSince.erase(entry.first);
            continue;
        }

        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(record.guid);
        if (!bot || !bot->IsInWorld() || !bot->isDead())
        {
            _deadSince.erase(entry.first);
            continue;
        }

        if (bot->IsInCombat())
            continue;

        auto since = _deadSince.find(entry.first);
        if (since == _deadSince.end())
        {
            _deadSince.emplace(entry.first, now);
            continue;
        }

        if (now - since->second < sPlayerbotAIConfig.LODReviveDelay)
            continue;

        // Resurrect in place and let the manager refresh gear/health; the bot stays where it
        // fell and dematerializes normally once the player moves on.
        bot->ResurrectPlayer(1.0f, false);
        sRandomPlayerbotMgr.Refresh(bot);
        _deadSince.erase(entry.first);
        LOG_DEBUG("playerbots", "Bot LOD: revived dead bot {}", bot->GetName());
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

    // Factory-created characters are level-1 shells: the standard random-bot login flow
    // levels them via factory init (rolled in [RandomBotMinLevel .. player max level + 3]
    // when SyncLevelWithPlayers is on) and equips them for the rolled level, but that flow
    // is bypassed in LOD mode. Run it here for uninitialized bots so materialized crowds
    // match the population design instead of spawning as level-1 shells. The stable level
    // also updates record.level below so cold records stop being re-rolled after this.
    if (bot->GetLevel() < 3 && sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        // Feed the live player ceiling before rolling: CheckPlayers' 60s sync loop is
        // bypassed in LOD mode, so the roll would otherwise cap at a stale playersLevel
        // of 0 and every shell would materialize at level 1. Same margin (+3) as
        // CheckPlayers uses.
        uint32 maxRealLevel = 0;
        for (RealPlayerPosition const& player : _players)
            maxRealLevel = std::max(maxRealLevel, player.level);
        uint32 const ceiling = maxRealLevel ? maxRealLevel + 3 : sPlayerbotAIConfig.RandomBotMaxLevel;
        sRandomPlayerbotMgr.SetPlayersLevel(ceiling);

        uint32 const targetLevel = StableLevelForRecord(record, ceiling);
        PlayerbotFactory factory(bot, targetLevel);
        factory.Randomize(false);
        record.level = bot->GetLevel();
    }

    // Records move between maps (ConcentrateRecords relocates them toward players), so the
    // DB position at login can be anywhere; always settle the bot onto the record position.
    Map* map = sMapMgr->FindBaseMap(record.mapId);
    if (!map)
        return;

    float z = map->GetHeight(bot->GetPhaseMask(), record.x, record.y, record.z, true, MAX_HEIGHT);
    if (z <= INVALID_HEIGHT)
        z = record.z;

    record.z = z;
    if (bot->GetMapId() != record.mapId)
        LOG_DEBUG("playerbots", "Bot LOD: bot {} materializing from map {} to record map {}",
                  bot->GetName(), bot->GetMapId(), record.mapId);

    if (!bot->TeleportTo(record.mapId, record.x, record.y, z, record.orientation))
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

// Group invitation targeting a cold record (e.g. from the who list): materialize the
// named shell and defer the invitation until it logs in and settles.
bool BotLODMgr::TryInviteColdBot(Player* inviter, std::string const& memberName)
{
    if (!_enabled || !inviter)
        return false;

    std::string name = memberName;
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);

    auto idx = _whoNameIndex.find(name);
    if (idx == _whoNameIndex.end())
        return false;

    auto recordIt = _records.find(idx->second);
    if (recordIt == _records.end())
        return false;

    BotRecord& record = recordIt->second;
    if (record.online || _pendingSince.contains(idx->second) || _pendingInvites.contains(idx->second))
        return false;

    time_t const now = time(nullptr);
    if (!Materialize(record, now))
        return false;

    _pendingInvites[idx->second] = {inviter->GetGUID(), now + 5};
    LOG_INFO("playerbots", "Bot LOD: materializing bot {} for a group invitation", record.guid);
    return true;
}

// Deliver deferred invitations once the invited bot is logged in and settled.
void BotLODMgr::ProcessPendingInvites(time_t now)
{
    for (auto it = _pendingInvites.begin(); it != _pendingInvites.end();)
    {
        if (now < it->second.readyAt)
        {
            ++it;
            continue;
        }

        Player* inviter = ObjectAccessor::FindConnectedPlayer(it->second.inviterGuid);
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(ObjectGuid::Create<HighGuid::Player>(it->first));

        if (!inviter || !bot || !bot->IsInWorld() || bot->IsBeingTeleported())
        {
            // The inviter left - nobody to deliver to; a teleporting bot just needs time.
            it = _pendingInvites.erase(it);
            continue;
        }

        WorldPacket packet(CMSG_GROUP_INVITE, bot->GetName().size() + 10);
        packet << bot->GetName();
        inviter->GetSession()->HandleGroupInviteOpcode(packet);
        LOG_INFO("playerbots", "Bot LOD: delivering deferred group invitation to {}", bot->GetName());
        it = _pendingInvites.erase(it);
    }
}

// Who list extension: render cold records as who entries so the in-game who window
// reflects the whole population, mirroring how a full realm's /who returns a sample of
// all online players. Levels: a materialized-once record shows its real level; a never
// materialized shell shows a stable per-guid roll inside the live player ceiling.
void BotLODMgr::AppendWhoEntries(std::vector<WhoListPlayerInfo>& storage) const
{
    if (!_enabled)
        return;

    uint32 ceiling = sPlayerbotAIConfig.RandomBotMaxLevel;
    for (RealPlayerPosition const& player : _players)
        ceiling = std::max(ceiling, player.level + 3);

    for (auto const& entry : _records)
    {
        BotRecord const& record = entry.second;
        if (record.online)
            continue;

        auto identity = _whoData.find(entry.first);
        if (identity == _whoData.end() || identity->second.name.empty())
            continue;

        // Unknown location (factory shell that never materialized) - hide from the who
        // list until it gains a real zone at its first materialization.
        if (!record.zoneId)
            continue;

        uint32 const level = StableLevelForRecord(record, ceiling);

        storage.emplace_back(ObjectGuid::Create<HighGuid::Player>(entry.first),
                             Player::TeamIdForRace(record.race), SEC_PLAYER, level, record.classId, record.race,
                             record.zoneId, identity->second.gender, true, identity->second.name,
                             identity->second.guild, identity->second.nameNarrow, identity->second.guildNarrow);
    }
}

uint32 BotLODMgr::StableLevelForRecord(BotRecord const& record, uint32 ceiling) const
{
    if (record.level > 1)
        return record.level;

    uint32 const minLevel = std::max(2u, sPlayerbotAIConfig.RandomBotMinLevel);
    uint32 hash = (record.guid * 2654435761u) >> 13;
    uint32 level;
    if (hash % 100 < 10)
        level = ceiling;
    else
        level = minLevel + (hash % std::max(1u, ceiling - minLevel + 1));
    return std::min(level, ceiling);
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

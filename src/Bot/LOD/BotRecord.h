/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTRECORD_H
#define PLAYERBOTS_BOTRECORD_H

#include "ObjectGuid.h"

#include <ctime>

// Compact off-screen representation of a random bot. This struct is intentionally
// free of strings and owning pointers; full state lives in the character database
// and is loaded only when the bot is materialized.
struct BotRecord
{
    ObjectGuid::LowType guid = 0;
    uint32 accountId = 0;
    uint32 level = 1;
    uint32 xp = 0;
    uint32 money = 0;
    uint32 mapId = 0;
    uint32 zoneId = 0;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float orientation = 0.0f;

    float targetX = 0.0f;
    float targetY = 0.0f;
    float targetZ = 0.0f;

    uint8 race = 0;
    uint8 classId = 0;
    bool online = false;
    bool hot = false;
    bool hasTarget = false;
    float nearestPlayerSq = 0.0f;

    time_t hotDeadline = 0;
};

static_assert(sizeof(BotRecord) <= 128, "BotRecord must stay compact");

#endif

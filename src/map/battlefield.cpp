﻿/*
===========================================================================

  Copyright (c) 2010-2015 Darkstar Dev Teams

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see http://www.gnu.org/licenses/

  This file is part of DarkStar-server source code.

===========================================================================
*/

#include "battlefield.h"

#include "../common/timer.h"

#include "ai/ai_container.h"
#include "ai/states/death_state.h"

#include "enmity_container.h"

#include "entities/baseentity.h"
#include "entities/battleentity.h"
#include "entities/charentity.h"
#include "entities/mobentity.h"
#include "entities/npcentity.h"

#include "lua/luautils.h"

#include "packets/entity_animation.h"
#include "packets/entity_update.h"
#include "packets/message_basic.h"
#include "packets/position.h"

#include "status_effect_container.h"
#include "treasure_pool.h"

#include "utils/itemutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

CBattlefield::CBattlefield(uint16 id, CZone* PZone, uint8 area, CCharEntity* PInitiator)
{
    m_ID = id;
    m_ZoneID = PZone->GetID();
    m_Area = area;
    m_Initiator.id = PInitiator->id;
    m_Initiator.name = PInitiator->name;

    m_StartTime = server_clock::now();
    m_Tick = m_StartTime;
}

CBattlefield::~CBattlefield()
{
    Cleanup();
}

uint16 CBattlefield::GetID()
{
    return m_ID;
}

CZone* CBattlefield::GetZone()
{
    return zoneutils::GetZone(m_ZoneID);
}

uint16 CBattlefield::GetZoneID()
{
    return m_ZoneID;
}

string_t CBattlefield::GetName()
{
    return m_Name;
}

BattlefieldInitiator_t CBattlefield::GetInitiator()
{
    return m_Initiator;
}

uint8 CBattlefield::GetArea()
{
    return m_Area;
}

BattlefieldRecord_t CBattlefield::GetRecord()
{
    return m_Record;
}

uint8 CBattlefield::GetStatus()
{
    return m_Status;
}

uint16 CBattlefield::GetRuleMask()
{
    return m_Rules;
}

time_point CBattlefield::GetStartTime()
{
    return m_StartTime;
}

duration CBattlefield::GetTimeInside()
{
    return m_Tick - m_StartTime;
}

time_point CBattlefield::GetFightTime()
{
    return m_FightTick;
}

duration CBattlefield::GetTimeLimit()
{
    return m_TimeLimit;
}

time_point CBattlefield::GetWipeTime()
{
    return m_WipeTime;
}

duration CBattlefield::GetFinishTime()
{
    return m_FinishTime;
}

duration CBattlefield::GetRemainingTime()
{
    return GetTimeLimit() - GetTimeInside();
}

uint8 CBattlefield::GetMaxParticipants()
{
    return m_MaxParticipants;
}

uint8 CBattlefield::GetPlayerCount()
{
    return m_PlayerList.size();
}

uint8 CBattlefield::GetLevelCap()
{
    return m_LevelCap;
}

uint16 CBattlefield::GetLootID()
{
    return m_LootID;
}

void CBattlefield::SetName(int8* name)
{
    m_Name.clear();
    m_Name.insert(0, name);
}

void CBattlefield::SetInitiator(int8* name)
{
    m_Initiator.name.clear();
    m_Initiator.name.insert(0, name);
}

void CBattlefield::SetTimeLimit(duration time)
{
    m_TimeLimit = time;
}

void CBattlefield::SetWipeTime(time_point time)
{
    m_WipeTime = time;
}

void CBattlefield::SetArea(uint8 area)
{
    m_Area = area;
}

void CBattlefield::SetRecord(int8* name, duration time)
{
    time = std::chrono::duration_cast<std::chrono::seconds>(time);

    m_Record.name = name ? name : "your mum";
    m_Record.time = time.count() ? time : 3600s;

    const int8* query = "UPDATE battlefield_info SET fastestName = '%s', fastestTime = '%u' WHERE battlefieldId = '%u' AND zoneId = '%u'";
    if (Sql_Query(SqlHandle, query, name, time.count(), this->GetID(), GetZoneID()) == SQL_ERROR)
    {
        ShowError("Battlefield::SetRecord() unable to find battlefield %u \n", this->GetID());
    }
}

void CBattlefield::SetStatus(uint8 status)
{
    m_Status = status;
}

void CBattlefield::SetRuleMask(uint16 rulemask)
{
    m_Rules = rulemask;
}

void CBattlefield::SetMaxParticipants(uint8 max)
{
    m_MaxParticipants = max;
}

void CBattlefield::SetLevelCap(uint8 cap)
{
    m_LevelCap = cap;
}

void CBattlefield::SetLootID(uint16 id)
{
    m_LootID = id;
}

void CBattlefield::ApplyLevelCap(CCharEntity* PChar)
{
    //adjust player's level to the appropriate cap and remove buffs

    auto cap = GetLevelCap();
    cap += map_config.Battle_cap_tweak;

    if (cap)
    {
        PChar->StatusEffectContainer->DelStatusEffectsByFlag(EFFECTFLAG_DEATH);
        PChar->StatusEffectContainer->AddStatusEffect(new CStatusEffect(EFFECT_LEVEL_RESTRICTION, EFFECT_LEVEL_RESTRICTION, cap, 0, 0));
    }
}

void CBattlefield::PushMessageToAllInBcnm(uint16 msg, uint16 param)
{
    // todo: handle this properly
    ForEachPlayer([msg, param](CCharEntity* PChar)
    {
        if (PChar->m_lastBcnmTimePrompt != param)
        {
            PChar->pushPacket(new CMessageBasicPacket(PChar, PChar, param, 0, msg));
            PChar->m_lastBcnmTimePrompt = param;
        }
    });
}

bool CBattlefield::AllPlayersDead()
{
    ForEachPlayer([](CCharEntity* PChar)
    {
        if (!PChar->PAI->IsCurrentState<CDeathState>())
            return false;
    });
    SetWipeTime(server_clock::now());
    return true;
}

bool CBattlefield::AllEnemiesDefeated()
{
    ForEachRequiredEnemy([](CMobEntity* PMob)
    {
        if (PMob->PAI->IsCurrentState<CDeathState>())
            return false;
    });
    return true;
}

bool CBattlefield::IsOccupied()
{
    return m_PlayerList.size() > 0;
}

bool CBattlefield::InsertEntity(CBaseEntity* PEntity, bool inBattlefield, BATTLEFIELDMOBCONDITION conditions, bool ally)
{
    DSP_DEBUG_BREAK_IF(PEntity == nullptr);

    if (PEntity->objtype == TYPE_PC)
    {
        if (GetPlayerCount() < GetMaxParticipants())
        {
            ApplyLevelCap(static_cast<CCharEntity*>(PEntity));
            m_PlayerList.push_back(PEntity->id);
        }
        else
        {
            return false;
        }
    }
    else if (PEntity->objtype == TYPE_NPC)
    {
        m_NpcList.push_back(PEntity->targid);
    }
    else if (PEntity->objtype == TYPE_MOB)
    {
        // mobs
        if (!ally)
        {
            auto entity = dynamic_cast<CPetEntity*>(PEntity);

            // dont enter player pet
            if (!(entity && entity->PMaster && entity->PMaster->objtype == TYPE_PC))
            {
                // only apply conditions to mobs spawning by default
                BattlefieldMob_t mob;
                mob.targid = PEntity->targid;
                mob.condition = conditions;

                if (mob.condition & CONDITION_WIN_REQUIREMENT)
                    m_RequiredEnemyList.push_back(mob);
                else
                    m_AdditionalEnemyList.push_back(mob);
            }
        }
        // ally
        else
        {
            m_AllyList.push_back(PEntity->targid);
        }
    }

    auto entity = dynamic_cast<CBattleEntity*>(PEntity);

    // set their battlefield to this as they're now physically inside that battlefield
    if (inBattlefield)
        PEntity->PBattlefield = std::unique_ptr<CBattlefield>(this);

    // mob, initiator or ally
    if (entity && !entity->StatusEffectContainer->GetStatusEffect(EFFECT_BATTLEFIELD))
        entity->StatusEffectContainer->AddStatusEffect(new CStatusEffect(EFFECT_BATTLEFIELD, EFFECT_BATTLEFIELD, this->GetID(),
            0, 0, m_Initiator.id, this->GetArea()));

    return true;
}

CBaseEntity* CBattlefield::GetEntity(CBaseEntity* PEntity)
{
    if (PEntity->objtype == TYPE_PC)
        ForEachPlayer([&](CCharEntity* PChar) { if (PChar == PEntity) return PEntity; });
    else if (PEntity->objtype == TYPE_MOB && PEntity->allegiance == ALLEGIANCE_MOB)
        ForEachEnemy([&](CMobEntity* PMob) { if (PMob == PEntity) return PEntity; });
    else if (PEntity->objtype == TYPE_MOB && PEntity->allegiance == ALLEGIANCE_PLAYER)
        ForEachAlly([&](CMobEntity* PAlly) { if (PAlly == PEntity) return PEntity; });
    else if (PEntity->objtype == TYPE_PET && static_cast<CBattleEntity*>(PEntity)->PMaster->objtype == TYPE_PC)
        ForEachPlayer([&](CCharEntity* PChar) { if (PChar == static_cast<CBattleEntity*>(PEntity)->PMaster) return PEntity; });
    else if (PEntity->objtype == TYPE_NPC)
        ForEachNpc([&](CNpcEntity* PNpc) { if (PNpc == PEntity) return PEntity; });

    return nullptr;
}

bool CBattlefield::RemoveEntity(CBaseEntity* PEntity, uint8 leavecode)
{
    DSP_DEBUG_BREAK_IF(PEntity == nullptr);

    auto found = false;
    auto check = [PEntity, &found](auto entity) { if (PEntity->targid == entity) { found = true; return found; } return false; };

    if (PEntity->objtype == TYPE_PC)
    {
        auto i = 0;
        for (; i < m_PlayerList.size(); ++i)
        {
            if (m_PlayerList[i] == PEntity->id)
            {
                found = true;
                break;
            }
        }
        if (found)
            m_PlayerList.erase(m_PlayerList.begin() + i);

        if (leavecode != 255)
        {
            if (leavecode == 2)
                OpenChest();

            luautils::OnBattlefieldLeave(static_cast<CCharEntity*>(PEntity), this, leavecode);
        }
    }
    else if (PEntity->objtype == TYPE_NPC)
    {
        PEntity->loc.zone->PushPacket(PEntity, CHAR_INRANGE, new CEntityAnimationPacket(PEntity, CEntityAnimationPacket::Fade_Out));
        PEntity->PAI->Despawn();
        m_NpcList.erase(std::remove_if(m_NpcList.begin(), m_NpcList.end(), check), m_NpcList.end());
    }
    else if (PEntity->objtype == TYPE_MOB)
    {
        PEntity->PAI->Despawn();
        PEntity->status = STATUS_DISAPPEAR;

        // allies targid >= 0x700
        if (PEntity->targid >= 0x700)
        {
            m_AllyList.erase(std::remove_if(m_AllyList.begin(), m_AllyList.end(), check), m_AllyList.end());
            GetZone()->DeletePET(PEntity);
            delete PEntity;
            return found;
        }
        else
        {
            auto check = [PEntity, &found](auto entity) { if (entity.targid == PEntity->targid) { found = true; return found; } return false; };
            m_RequiredEnemyList.erase(std::remove_if(m_RequiredEnemyList.begin(), m_RequiredEnemyList.end(), check), m_RequiredEnemyList.end());
            m_AdditionalEnemyList.erase(std::remove_if(m_AdditionalEnemyList.begin(), m_AdditionalEnemyList.end(), check), m_AdditionalEnemyList.end());
        }
    }
    // assume its either a player or ally and remove any enmity
    auto entity = dynamic_cast<CBattleEntity*>(PEntity);
    if (entity)
    {
        entity->StatusEffectContainer->DelStatusEffectsByFlag(EFFECTFLAG_CONFRONTATION);
        entity->StatusEffectContainer->DelStatusEffect(EFFECT_LEVEL_RESTRICTION);
        ClearEnmityForEntity(entity);
    }
    PEntity->PBattlefield.release();
    return found;
}

void CBattlefield::DoTick(time_point time)
{
    if (m_Tick + 1s < time)
    {
        //todo : bcnm - update tick, fight tick, end if time is up
        m_Tick = time;
        m_FightTick = m_Status == BATTLEFIELD_STATUS_LOCKED ? time : m_FightTick;

        // remove the char if they zone out
        for (auto charid : m_PlayerList)
        {
            auto PChar = zoneutils::GetChar(charid);
            if (!PChar || PChar->getZone() != GetZoneID())
            {
                RemoveEntity(PChar, -1);
            }
        }
        luautils::OnBattlefieldTick(this);
    }
}

bool CBattlefield::CanCleanup(bool cleanup)
{
    if (cleanup)
        m_Cleanup = cleanup;

    return m_Cleanup;
}

void CBattlefield::Cleanup()
{
    // wipe enmity from all mobs in list if needed
    ForEachEnemy([&](CMobEntity* PMob)
    {
        RemoveEntity(PMob);
    });

    ForEachPlayer([&](CCharEntity* PChar)
    {
        RemoveEntity(PChar, -1);
    });

    ForEachAlly([&](CMobEntity* PAlly)
    {
        RemoveEntity(PAlly);
    });

    //make chest vanish (if any)
    ForEachNpc([&](CNpcEntity* PNpc)
    {
        RemoveEntity(PNpc);
    });

    if (std::chrono::duration_cast<std::chrono::seconds>(GetRecord().time) > std::chrono::duration_cast<std::chrono::seconds>(m_FightTick - m_StartTime))
    {
        SetRecord((int8*)m_Initiator.name.c_str(), std::chrono::duration_cast<std::chrono::seconds>(m_FightTick - m_StartTime));
    }
}

bool CBattlefield::LoadMobs()
{
    //get ids from DB
    const int8* fmtQuery = "SELECT monsterId, conditions \
						    FROM battlefield_mobs \
							WHERE battlefieldId = %u AND area = %u";

    int32 ret = Sql_Query(SqlHandle, fmtQuery, this->GetID(), this->GetArea());

    if (ret == SQL_ERROR ||
        Sql_NumRows(SqlHandle) == 0)
    {
        ShowError("Battlefield::LoadMobs() : Cannot find any monster IDs for battlefield %i area %i \n",
            this->GetID(), this->GetArea());
    }
    else
    {
        while (Sql_NextRow(SqlHandle) == SQL_SUCCESS)
        {
            uint32 mobid = Sql_GetUIntData(SqlHandle, 0);
            uint8 condition = Sql_GetUIntData(SqlHandle, 1);
            CMobEntity* PMob = (CMobEntity*)zoneutils::GetEntity(mobid, TYPE_MOB);

            if (PMob != nullptr)
            {
                if (condition & CONDITION_SPAWNED_AT_START)
                {
                    if (!PMob->PAI->IsSpawned())
                    {
                        PMob->Spawn();
                        //ShowDebug("Spawned %s (%u) id %i inst %i \n",PMob->GetName(),PMob->id,battlefield->GetID(),battlefield->GetArea());
                        this->InsertEntity(PMob, true, (BATTLEFIELDMOBCONDITION)condition);
                    }
                    else
                    {
                        this->InsertEntity(PMob, true);
                        ShowDebug(CL_CYAN"Battlefield::LoadMobs() <%s> (%u) is already spawned\n" CL_RESET, PMob->GetName(), PMob->id);
                    }
                }
                else
                {
                    this->InsertEntity(PMob, true, (BATTLEFIELDMOBCONDITION)condition);
                }
            }
            else
            {
                ShowDebug("Battlefield::LoadMobs() mob %u not found\n", mobid);
                return false;
            }
        }
    }
    return true;
}

bool CBattlefield::CanSpawnTreasure()
{
    return !m_SeenBooty;
}

bool CBattlefield::SpawnTreasureChest()
{
    DSP_DEBUG_BREAK_IF(m_SeenBooty);

    //get ids from DB
    const int8* fmtQuery = "SELECT npcId \
						    FROM battlefield_treasure_chests \
							WHERE battlefieldId = %u AND area = %u";

    int32 ret = Sql_Query(SqlHandle, fmtQuery, this->GetID(), this->GetArea());

    if (ret == SQL_ERROR || Sql_NumRows(SqlHandle) == 0)
    {
        ShowError("Battlefield::SpawnTreasureChest() Cannot find any npc IDs for battlefieldId %i area %i \n",
            this->GetID(), this->GetArea());
    }
    else
    {
        while (Sql_NextRow(SqlHandle) == SQL_SUCCESS)
        {
            uint32 npcid = Sql_GetUIntData(SqlHandle, 0);
            CBaseEntity* PNpc = (CBaseEntity*)zoneutils::GetEntity(npcid, TYPE_NPC);
            if (PNpc)
            {
                PNpc->Spawn();
                this->InsertEntity(PNpc, true);
            }
            else
            {
                ShowDebug(CL_CYAN"Battlefield::SpawnTreasureChest: <%s> is already spawned\n" CL_RESET, PNpc->GetName());
            }
        }
        m_SeenBooty = true;
        return true;
    }
    return false;
}

void CBattlefield::OpenChest()
{
    DSP_DEBUG_BREAK_IF(m_GotBooty);

    auto LootList = itemutils::GetLootList(GetLootID());

    if (LootList)
    {
        for (auto i = 0; i < LootList->size(); ++i)
        {
            // todo: handle loot
        }
    }
    m_GotBooty = true;
}

void CBattlefield::ClearEnmityForEntity(CBattleEntity* PEntity)
{
    ForEachEnemy([PEntity](CMobEntity* PMob)
    {
        if (PEntity->PPet)
            PMob->PEnmityContainer->Clear(PEntity->PPet->id);

        PMob->PEnmityContainer->Clear(PEntity->id);
    });
}

bool CBattlefield::InProgress()
{
    ForEachEnemy([&](CMobEntity* PMob)
    {
        if (PMob->PEnmityContainer->GetEnmityList()->size())
        {
            if (m_Status == BATTLEFIELD_STATUS_OPEN)
                SetStatus(BATTLEFIELD_STATUS_LOCKED);

            return true;
        }
    });

    // mobs might have 0 enmity but we wont allow anymore players to enter
    return m_Status != BATTLEFIELD_STATUS_OPEN;
}

void CBattlefield::ForEachPlayer(std::function<void(CCharEntity*)> func)
{
    for (auto player : m_PlayerList)
    {
        func((CCharEntity*)GetZone()->GetCharByID(player));
    }
}

void CBattlefield::ForEachEnemy(std::function<void(CMobEntity*)> func)
{
    ForEachRequiredEnemy(func);
    ForEachAdditionalEnemy(func);
}

void CBattlefield::ForEachRequiredEnemy(std::function<void(CMobEntity*)> func)
{
    for (auto mob : m_RequiredEnemyList)
    {
        func((CMobEntity*)GetZone()->GetEntity(mob.targid, TYPE_MOB | TYPE_PET));
    }
}

void CBattlefield::ForEachAdditionalEnemy(std::function<void(CMobEntity*)> func)
{
    for (auto mob : m_AdditionalEnemyList)
    {
        func((CMobEntity*)GetZone()->GetEntity(mob.targid, TYPE_MOB | TYPE_PET));
    }
}

void CBattlefield::ForEachNpc(std::function<void(CNpcEntity*)> func)
{
    for (auto npc : m_NpcList)
    {
        func((CNpcEntity*)GetZone()->GetEntity(npc, TYPE_NPC));
    }
}

void CBattlefield::ForEachAlly(std::function<void(CMobEntity*)> func)
{
    for (auto ally : m_AllyList)
    {
        func((CMobEntity*)GetZone()->GetEntity(ally, TYPE_PET));
    }
}
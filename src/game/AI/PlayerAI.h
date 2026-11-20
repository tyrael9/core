/*
 * Copyright (C) 2005-2010 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2010 MaNGOSZero <http://github.com/mangoszero/mangoszero/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MANGOS_PLAYERAI_H
#define MANGOS_PLAYERAI_H

#include "Common.h"
#include "ObjectGuid.h"

class WorldObject;
class Unit;
class Creature;
class Player;
class Spell;
class SpellEntry;

enum class PlayerPointMovementResult : uint8
{
    Reached,
    Interrupted,
    Failed
};

class PlayerAI
{
    public:
        explicit PlayerAI(Player* pPlayer) : me(pPlayer), enablePositiveSpells(false) {}
        virtual ~PlayerAI();
        void SetPlayer(Player* player) { me = player; }
        virtual void Remove();

        // Called at World update tick
        virtual void UpdateAI(uint32 const /*diff*/);
        virtual void MovementInform(uint32 MovementType, uint32 Data = 0) {}
        // Engine-only result channel; legacy MovementInform remains unchanged.
        virtual void OnPointMovementResult(uint64 /*actionId*/, uint64 /*encounterId*/,
                                           uint32 /*pointId*/,
                                           PlayerPointMovementResult /*result*/,
                                           float /*finalDistance*/) {}

        // Optional observer hooks for player-owned spell requests and their final Core outcome.
        // Returning zero from the prepare hook opts out of lifecycle correlation.
        virtual uint64 OnSpellCastPreparing(Spell const* /*spell*/, uint32 /*spellId*/,
                                            uint64 /*coreTargetGuid*/, uint64& encounterId,
                                            uint64& traceTargetGuid, bool& engineAction)
        {
            encounterId = 0;
            traceTargetGuid = 0;
            engineAction = false;
            return 0;
        }
        virtual void OnSpellCastFinished(Spell const* /*spell*/, uint64 /*actionId*/,
                                         uint64 /*encounterId*/, uint32 /*spellId*/,
                                         uint64 /*traceTargetGuid*/, bool /*success*/,
                                         bool /*interrupted*/, bool /*engineAction*/) {}
        // Item-use roots aggregate the complete ON_USE fan-out; each child Spell is
        // registered before prepare() so synchronous finishes cannot race registration.
        virtual uint64 OnItemUsePreparing(uint64 /*itemGuid*/, uint32 /*itemEntry*/,
            uint32 /*itemCount*/, uint8 /*bagIndex*/, uint8 /*itemSlot*/,
            uint8 /*requestedSpellSlot*/) { return 0; }
        virtual uint64 OnItemUseChildPreparing(uint64 /*itemUseId*/, uint8 /*slotIndex*/,
            uint32 /*spellId*/, bool /*triggered*/) { return 0; }
        virtual void OnItemUseSlotSkipped(uint64 /*itemUseId*/, uint8 /*slotIndex*/) {}
        virtual void OnItemUseDispatchComplete(uint64 /*itemUseId*/) {}
        virtual void OnItemUseChildFinished(uint64 /*itemUseId*/, uint64 /*childId*/,
            uint32 /*spellId*/, bool /*success*/, bool /*interrupted*/) {}
        virtual void OnItemUseMutation(uint64 /*itemUseId*/, uint64 /*childId*/,
            uint64 /*itemGuid*/, uint32 /*itemEntry*/, uint8 /*kind*/, uint8 /*phase*/,
            uint8 /*slotIndex*/, uint32 /*beforeCount*/, uint32 /*afterCount*/,
            int32 /*beforeCharges*/, int32 /*afterCharges*/, bool /*itemExistsAfter*/) {}
        // Notify observers only when the Core changes a Player between alive and dead.
        virtual void OnAliveStateChanged(bool /*wasAlive*/, bool /*isAlive*/) {}
        // Called after Core applies one hit to a Player; values are actual health before/after.
        virtual void OnDamageTaken(uint64 /*attackerGuid*/, uint32 /*damage*/,
                                   uint32 /*healthBefore*/, uint32 /*healthAfter*/,
                                   uint32 /*spellId*/, uint8 /*damageType*/) {}
        // Called once after world updates stop and before bot/player teardown.
        virtual void OnServerShutdown() {}
        // Called on the source Map thread before this Player is detached from it.
        virtual void OnMapLeave(uint32 /*mapId*/, uint32 /*instanceId*/) {}

        // == Helpers =====================================
        bool CanCastSpell(Unit* pTarget, SpellEntry const* pSpell, bool isTriggered, bool checkControlled = true);

        // == Fields =======================================

        // Pointer to controlled by AI player
        Player* me;
        bool enablePositiveSpells;
};

class PlayerControlledAI: public PlayerAI
{
    public:
        explicit PlayerControlledAI(Player* pPlayer, Unit* caster = nullptr);

        ~PlayerControlledAI() override;

        // Called at World update tick
        void UpdateAI(uint32 const /*diff*/) override;
        Unit* FindController();
        void UpdateTarget(Unit* victim);

        // == Fields =======================================
        ObjectGuid controllerGuid;
        uint32 uiGlobalCD;
        std::vector<uint32> usableSpells;
        bool bIsMelee;
        bool isHealer;
        
};

#endif

/*
 * PartyBot decision tracing support.
 *
 * This file is part of the VMaNGOS project and is distributed under
 * the GNU General Public License, version 2 or (at your option) any later version.
 */

#ifndef MANGOS_PARTY_BOT_DECISION_TRACE_H
#define MANGOS_PARTY_BOT_DECISION_TRACE_H

#include "Common.h"

class Player;
class Unit;
struct SpellEntry;

namespace PartyBotEngine
{

class DecisionTrace
{
public:
    DecisionTrace();

    // Begin a bounded trace window for one PartyBot AI update.
    void BeginTick() const;

    // Record one eligibility rejection or spell execution result.
    void Record(Player const* bot, char const* event, Unit const* target,
                SpellEntry const* spell, char const* reason, uint32 detail = 0) const;

private:
    mutable uint32 m_recordsThisTick;
    mutable bool m_enabled;
};

} // namespace PartyBotEngine

#endif

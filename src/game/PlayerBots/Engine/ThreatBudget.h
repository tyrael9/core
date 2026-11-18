#ifndef MANGOS_PARTYBOT_THREAT_BUDGET_H
#define MANGOS_PARTYBOT_THREAT_BUDGET_H

#include "PartySnapshot.h"
#include <cmath>

namespace PartyBotEngine {

enum class ThreatBudgetStatus : std::uint8_t {
    Unknown,
    CurrentVictim,
    BudgetAvailable,
    AtOrOverBudget
};

enum class ThreatBudgetUnknownReason : std::uint8_t {
    None,
    MissingOrDuplicateHostile,
    HostileNotAlive,
    MissingOrDuplicateMember,
    MemberNotOnlineOrAlive,
    MissingCurrentVictim,
    MissingOrDuplicateVictimThreat,
    VictimThreatOffline,
    InvalidVictimThreat,
    MissingOrDuplicateMemberThreat,
    MemberThreatOffline,
    InvalidMemberThreat
};

struct ThreatBudgetAssessment {
    ThreatBudgetStatus status = ThreatBudgetStatus::Unknown;
    ThreatBudgetUnknownReason unknownReason = ThreatBudgetUnknownReason::None;
    Guid hostileGuid = 0;
    Guid memberGuid = 0;
    Guid currentVictimGuid = 0;
    float currentVictimThreat = 0.0f;
    float memberThreat = 0.0f;
    float thresholdMultiplier = 0.0f;
    float thresholdThreat = 0.0f;
    float rawHeadroom = 0.0f;
    bool usesMeleeThreshold = false;
};

namespace detail {

inline HostileSnapshot const* FindUniqueHostile(PartySnapshot const& snapshot,
                                                Guid hostileGuid, bool& duplicate)
{
    HostileSnapshot const* found = nullptr;
    duplicate = false;
    for (HostileSnapshot const& hostile : snapshot.hostiles) {
        if (hostile.guid != hostileGuid)
            continue;
        if (found) {
            duplicate = true;
            return nullptr;
        }
        found = &hostile;
    }
    return found;
}

inline PartyMemberSnapshot const* FindUniqueMember(PartySnapshot const& snapshot,
                                                   Guid memberGuid, bool& duplicate)
{
    PartyMemberSnapshot const* found = nullptr;
    duplicate = false;
    for (PartyMemberSnapshot const& member : snapshot.members) {
        if (member.guid != memberGuid)
            continue;
        if (found) {
            duplicate = true;
            return nullptr;
        }
        found = &member;
    }
    return found;
}

inline ThreatEntrySnapshot const* FindUniqueThreatEntry(HostileSnapshot const& hostile,
                                                         Guid unitGuid, bool& duplicate)
{
    ThreatEntrySnapshot const* found = nullptr;
    duplicate = false;
    for (ThreatEntrySnapshot const& entry : hostile.threat) {
        if (entry.unitGuid != unitGuid)
            continue;
        if (found) {
            duplicate = true;
            return nullptr;
        }
        found = &entry;
    }
    return found;
}

} // namespace detail

// A read-only projection of Core's current-victim aggro threshold. This is not
// a replacement for ThreatContainer::selectNextVictim: it does not model
// eligibility passes, threat-area checks, taunt, immunity or future threat.
inline ThreatBudgetAssessment AssessThreatBudget(PartySnapshot const& snapshot,
                                                  Guid hostileGuid, Guid memberGuid)
{
    ThreatBudgetAssessment result;
    result.hostileGuid = hostileGuid;
    result.memberGuid = memberGuid;

    bool duplicate = false;
    HostileSnapshot const* hostile =
        detail::FindUniqueHostile(snapshot, hostileGuid, duplicate);
    if (!hostile || duplicate) {
        result.unknownReason = ThreatBudgetUnknownReason::MissingOrDuplicateHostile;
        return result;
    }
    if (!hostile->alive) {
        result.unknownReason = ThreatBudgetUnknownReason::HostileNotAlive;
        return result;
    }

    PartyMemberSnapshot const* member =
        detail::FindUniqueMember(snapshot, memberGuid, duplicate);
    if (!member || duplicate) {
        result.unknownReason = ThreatBudgetUnknownReason::MissingOrDuplicateMember;
        return result;
    }
    if (!member->online || !member->alive) {
        result.unknownReason = ThreatBudgetUnknownReason::MemberNotOnlineOrAlive;
        return result;
    }
    if (!hostile->victimGuid) {
        result.unknownReason = ThreatBudgetUnknownReason::MissingCurrentVictim;
        return result;
    }
    result.currentVictimGuid = hostile->victimGuid;

    ThreatEntrySnapshot const* victimThreat =
        detail::FindUniqueThreatEntry(*hostile, hostile->victimGuid, duplicate);
    if (!victimThreat || duplicate) {
        result.unknownReason = ThreatBudgetUnknownReason::MissingOrDuplicateVictimThreat;
        return result;
    }
    if (!victimThreat->online) {
        result.unknownReason = ThreatBudgetUnknownReason::VictimThreatOffline;
        return result;
    }
    if (!std::isfinite(victimThreat->threat) || victimThreat->threat < 0.0f) {
        result.unknownReason = ThreatBudgetUnknownReason::InvalidVictimThreat;
        return result;
    }
    result.currentVictimThreat = victimThreat->threat;

    ThreatEntrySnapshot const* memberThreat =
        detail::FindUniqueThreatEntry(*hostile, memberGuid, duplicate);
    if (!memberThreat || duplicate) {
        result.unknownReason = ThreatBudgetUnknownReason::MissingOrDuplicateMemberThreat;
        return result;
    }
    if (!memberThreat->online) {
        result.unknownReason = ThreatBudgetUnknownReason::MemberThreatOffline;
        return result;
    }
    if (!std::isfinite(memberThreat->threat) || memberThreat->threat < 0.0f) {
        result.unknownReason = ThreatBudgetUnknownReason::InvalidMemberThreat;
        return result;
    }
    result.memberThreat = memberThreat->threat;

    if (memberGuid == hostile->victimGuid) {
        result.status = ThreatBudgetStatus::CurrentVictim;
        return result;
    }

    result.usesMeleeThreshold = memberThreat->meleeReachable;
    result.thresholdMultiplier = result.usesMeleeThreshold ? 1.1f : 1.3f;
    result.thresholdThreat = result.currentVictimThreat * result.thresholdMultiplier;
    result.rawHeadroom = result.thresholdThreat - result.memberThreat;
    result.status = result.rawHeadroom > 0.0f
        ? ThreatBudgetStatus::BudgetAvailable
        : ThreatBudgetStatus::AtOrOverBudget;
    return result;
}

} // namespace PartyBotEngine

#endif

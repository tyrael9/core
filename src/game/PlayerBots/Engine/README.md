# PartyBot Engine migration

The architecture, lifecycle policy, rollout phases and validation requirements are maintained in [DESIGN.md](DESIGN.md). Update that document in the same change whenever implementation decisions or phase status change.

## Baseline

This work starts from the exact Core revision used by the reference deployment:

- Core: `0e3ff01e76d4758e8a7c3108b2717cc785ed56fa`
- Deploy wrapper: `ebaa560951459419c35140e20d787ec5db2905b8`
- Client variant: `5875` (Vanilla 1.12.1)
- Build language level: C++14

The fork branch is intentionally based on that Core commit rather than the moving
upstream `development` head. Do not rebase the work onto a newer upstream commit
without first reviewing its PartyBot and database changes.

## Phase 0: observability without changing decisions

The first implementation adds an opt-in `DecisionTrace` seam. It records:

- why a spell candidate failed the common readiness, GCD, resource, immunity,
  shapeshift, aura, or range checks;
- why a PartyBot AoE spell was rejected by the existing crowd-control or
  threat-safety guard;
- why a party member was excluded from heal-target consideration (health
  threshold, friendly-target validation, line of sight, scan range, an existing
  periodic heal, or the legacy duplicate-target filter);
- the result returned by the core spell-casting path;
- when a healer-class bot is assigned a non-healer role;
- when a same-map party member is in combat while the healer itself is not;
- when a Warrior tank has no party attacker to assist, or cannot use a taunt;
- when a Mage skips Arcane Explosion because too few enemies are near the caster;
- when the original owner disconnects and PartyBots enter their reconnect grace
  period, or when that grace period expires.

The healer lifecycle now enters the existing class-specific combat routine when
another member in the same map/instance is fighting. In that state it does not
start another food/drink cycle or run out-of-combat buff logic, but it retains
the existing follow/teleport behaviour. This is a targeted behaviour fix; it
does not replace the per-class rotations.

PartyBots now remain in their existing group while the original owner is
temporarily disconnected and their GUID is still a member of that group. The
default reconnect grace period is 10 minutes; configure it with
`PlayerBot.OwnerOfflineGraceMs`. A returning owner resets the timer. Explicit
group removal is still handled as a removal, rather than an owner disconnect.

Tracing is disabled by default. Add this line to the `mangosd.conf` used by a
test server to enable it:

```ini
PlayerBot.DecisionTrace = 1
```

Each PartyBot emits at most 12 trace records per one-second AI update. The
final slot reports that further records were suppressed. Normal bot behaviour
is unchanged while tracing is disabled, and the trace code does not write to
the database.

## Next phases

1. Add a snapshot of party members, attackers, threat and relevant spell state.
2. Add a read-only decision evaluation layer alongside the current per-class
   methods, retaining the legacy AI as the execution fallback.
3. Migrate healer selection and reservations first, then Protection Warrior
   target/threat decisions.
4. Add encounter-specific strategies only after core group combat is stable.

Do not remove the existing `PartyBotAI::UpdateAI` lifecycle or change database
schemas as part of the first engine milestones.

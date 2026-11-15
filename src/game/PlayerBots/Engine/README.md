# PartyBot Engine migration

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
  threshold, friendly-target validation, line of sight, or scan range);
- the result returned by the core spell-casting path.

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

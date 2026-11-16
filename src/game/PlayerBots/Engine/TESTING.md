# PartyBot Core build and regression testing

## Automated compile CI

The workflow at .github/workflows/partybot-core-build.yml builds this branch on a GitHub-hosted Linux ARM64 runner, matching the Oracle A1 deployment architecture.

It configures and compiles the Vanilla 1.12.1 / 5875 C++14 Core default targets, including game, mangosd and realmd. It uses bundled third-party sources, disables optional cpptrace/PCH for deterministic resource use, and uses ccache.

A successful run proves configure/compile/link on that CI environment. It does not prove runtime behavior, instance persistence, database compatibility beyond compilation, or successful operation in the production deployment. It is not permission to deploy.

## Required in-game regression checks

Run only on a disposable/test realm unless the user explicitly approves otherwise.

### Owner disconnect and reconnect

1. Form a party with owner and PartyBots; enter an instance.
2. Disconnect owner while bots remain in the instance.
3. Confirm bots remain in the original group and do not trigger immediate cleanup/reset.
4. Wait through normal group-leader offline handling; confirm bots remain.
5. Reconnect before grace expiry; confirm owner returns as leader and bots remain in same group/instance.
6. Repeat beyond grace expiry; confirm cleanup occurs only after expiry.
7. Verify intentional owner leave/disband and explicit bot removal still clean up.

### Healer lifecycle and pre-heal

1. Test Druid, Shaman and Priest bots while another same-map party member is fighting.
2. Confirm healer enters its class combat routine and does not start food/drink or out-of-combat buffs.
3. Confirm it heals eligible injured members when it has sufficient mana.
4. Confirm pre-heal reports success only if the cast is accepted.
5. Enable PlayerBot.DecisionTrace = 1 on the test realm if it does not act; capture rejection reasons.

### Warrior and Mage diagnostics

1. Confirm Warrior assists a party member's attacker and uses a valid taunt when appropriate.
2. Confirm trace explains missing assist targets or unusable taunts.
3. Confirm Mage Arcane Explosion is skipped when too few enemies are near the caster.

## New engine tests (planned)

- Snapshot consistency and GUID resolution within an update epoch.
- Trigger cadence and cached value invalidation.
- Action IsPossible/IsUseful/Execute result distinctions.
- Deterministic candidate priority and hard safety vetoes.
- Heal/CC/interrupt reservation ownership, expiry and invalidation.
- No duplicate GCD execution between Legacy and Strategy modes.
- Shadow mode never invokes the action executor.
- Encounter strategy activation/deactivation and safe movement arbitration.

## Validation vocabulary

- Committed: source and documentation are present in the branch.
- CI configured: workflow exists; no successful run implied.
- CI passed: latest relevant workflow run completed successfully.
- In-game tested: scenario was exercised on a test realm.
- Deployed: user explicitly approved the intended server update.

A CI pass must never be described as an in-game test.

## Pure decision-engine tests

Run the standalone C++14 harness from the repository root:

    bash src/game/PlayerBots/Engine/tests/run_tests.sh

The harness has no live Map/Player dependency. It currently covers:
- deterministic priority ordering and one action selected per channel;
- possible/useful rejection and missing action registration;
- reservation owner conflict and expiry;
- non-finite priority rejection and multiplier veto.

These tests validate the pure decision layer only. They do not test PartyBotAI integration, map-thread ownership, vMaNGOS spell execution or in-game behavior.

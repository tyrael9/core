# PartyBot Core build and regression testing

## Automated compile CI
The workflow at .github/workflows/partybot-core-build.yml builds this branch on a GitHub-hosted Linux ARM64 runner, matching the Oracle A1 deployment architecture.

It configures and compiles the Vanilla 1.12.1 / 5875 C++14 Core default targets, including game, mangosd and realmd. It uses bundled third-party sources, disables optional cpptrace/PCH for deterministic resource use, and uses ccache.

A successful run proves configure/compile/link on that CI environment. The first post-reactivation run (head `f3179084158804429f47f7a55f7cbbbf04f83ea9`, run #38) passed: standalone tests, CMake configure, and full `mangosd` / `realmd` build. It does not prove runtime behavior, instance persistence, database compatibility beyond compilation, or successful operation in the production deployment. It is not permission to deploy.

## Publish a test container to GHCR

The manual workflow `.github/workflows/publish-partybot-image.yml` builds a native `linux/arm64` image on GitHub's ARM64 runner and pushes it to GHCR. It is deliberately `workflow_dispatch` only: normal pushes and PR updates do not publish images.

For the current Shadow integration, the workflow pins:
- Core: `9af09a71cc8d2f37211acc4b7fd1ac64ba487492`
- Deployment wrapper: `tyrael9/vmangos-deploy@ebaa560951459419c35140e20d787ec5db2905b8`

Before publishing it runs the standalone PartyBot engine tests. The Docker build then compiles the pinned Core with the pinned deployment wrapper and its Docker patches. The image tag is:

```text
ghcr.io/tyrael9/vmangos-server:partybot-shadow-9af09a7
```

To publish: open the repository Actions tab, select **Publish PartyBot ARM64 container**, choose **Run workflow**, and run it on the working branch. A successful run means the image was pushed; it does not deploy anything to an Oracle instance and does not prove in-game behavior.

GHCR may initially create the package as private. If Oracle cannot pull it anonymously, change the package visibility to Public in the package settings, or authenticate on Oracle with a GitHub token that has `read:packages`.

When the publish workflow is deliberately retargeted to another Core SHA, update this section's SHA and image tag in the same commit.

## Deploying the image on the Oracle test instance

Do not replace the database image or touch database volumes. The server image
contains the Core binaries; it does not contain the extracted client map data
or the database.

The deployment fork provides a repeatable helper:

- Script: `tyrael9/vmangos-deploy/scripts/partybot-deploy.sh`
- Guide: [PartyBot test deployment](https://github.com/tyrael9/vmangos-deploy/blob/master/docs/partybot-testing.md)

After installing the helper on the Oracle host, deploy the tag published by the
successful image workflow:

```sh
./scripts/partybot-deploy.sh deploy partybot-daba395
```

The helper verifies the image architecture, saves the previous `mangosd` and
`realmd` image references, recreates only those two services with
`--no-deps`, and waits for their health checks. It does not restart or recreate
database services, make a database dump, or modify database backups, volumes,
configuration files, or extracted client data. Continue using the configured
database-backup service for routine backups.

Use the helper's `status` and `logs` commands to inspect the effective
configuration and service output. Use `rollback` to restore the recorded
previous server images. Once its private `.partybot/active.override.yaml` is
active, use the helper for further server-image changes: a plain
`docker compose up -d` does not include that override and can recreate the
server containers with the image references in the base Compose file.

Players should log out before deploying because replacing `mangosd`
disconnects active clients. A successful health check proves startup only;
in-game behavior must still be validated separately.

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

#### Confirmed Oracle owner-disconnect test (2026-11-15)

**Runtime-tested.** The ARM64 image
`ghcr.io/tyrael9/vmangos-server:partybot-daba395`, built from Core revision
`daba3955d67a40d84915c45537a3bed9445953ba`, was deployed to the Oracle A1
test realm. The operator interrupted the owner's connection and reconnected
within the configured grace period. On reconnect, the operator confirmed:

- the PartyBots remained in the original party;
- all PartyBots remained in the same instance at their previous locations;
- the instance had not reset;
- PartyBots were still present and able to act normally; and
- the owner was again the party leader.

The operator also tested an intentional owner leave. The PartyBots disappeared
from the owner's vicinity as expected, confirming that the grace protection does
not retain them after an intentional leave. The operator then waited beyond ten
minutes after an unexpected disconnect and confirmed that the PartyBots were
cleaned up as expected.

This validates the short-disconnect reconnect path, intentional owner-leave
cleanup and post-grace cleanup. It does not yet validate explicit per-bot removal
commands or reconnect after the grace period has already expired. The code default for
`PlayerBot.OwnerOfflineGraceMs` is 600000 ms (10 minutes), but the effective
value can be overridden in the server configuration.

## Healer lifecycle and pre-heal

1. Test Druid, Shaman and Priest bots while another same-map party member is fighting.
2. Confirm healer enters its class combat routine and does not start food/drink or out-of-combat buffs.
3. Confirm it heals eligible injured members when it has sufficient mana.
4. Confirm pre-heal reports success only if the cast is accepted.
5. Enable PlayerBot.DecisionTrace = 1 on the test realm if it does not act; capture rejection reasons.

### Warrior and Mage diagnostics

1. Confirm Warrior assists a party member's attacker and uses a valid taunt when appropriate.
2. Confirm trace explains missing assist targets or unusable taunts.
3. Confirm Mage Arcane Explosion is skipped when too few enemies are near the caster.

## Shadow runtime smoke test

Shadow mode is opt-in and must be enabled only on a test realm:

1. Set `PlayerBot.EngineShadow = 1`.
2. Set `PlayerBot.DecisionTrace = 1` to emit bounded summary records.
3. Restart only the game server through the deployment helper after publishing and deploying the intended test image.
4. Form a normal party and keep PartyBots on the same map/instance.
5. Inspect `[PartyBotShadow]` records. Group/map/instance IDs should match the game session, member count should include the bot and same-map group members, and snapshot epoch should advance when the cache refreshes.
6. Confirm detailed `[PartyBotShadowDecision]` records contain the expected role, strategy, target GUID, priority and selection status. Healers should propose injured members; tanks should propose hostiles attacking non-tanks; DPS should favor the Main Tank's target.
7. Verify Legacy behavior is unchanged. Shadow mode must never call `ExecuteSelected`.

Battleground groups (group ID zero), missing group state and unavailable snapshots are skipped. Cross-map party members are intentionally not resolved into the current map snapshot.

## Automated live diagnostics

The deployment fork now provides a read-only collector:
`tyrael9/vmangos-deploy/scripts/partybot-diagnostics.sh`.

Run it on the Oracle host from the `vmangos-deploy` checkout:

```sh
bash scripts/partybot-diagnostics.sh
```

It reads the effective `mangosd`/`realmd` image references and container
status through `partybot-deploy.sh`, reads only the two relevant PartyBot
configuration flags, and parses timestamped `[PartyBotShadow]` summaries.
It writes `.partybot/diagnostics/latest.json` and reports heartbeat freshness,
observed bot/group/map/instance identities, member/hostile counts, candidate
totals, and rate-limited `[PartyBotShadowStatus]` counts for
`no_supported_group`, `coordinator_unavailable`, `snapshot_unavailable`,
and `bot_missing_from_snapshot`. It is read-only: no restart, SQL, DB/volume
access or config changes.

The report deliberately labels absent heartbeats as WARN because that can mean
Shadow is disabled, no eligible party bot is active, the log window is too
short, or snapshot construction did not complete. It does not claim to detect
internal coordinator leaks or prove lifecycle correctness; those still need
dedicated instrumentation/tests. Zero candidates/selected is expected until
runtime strategies/actions are registered.

## New engine tests (planned)

- Trigger cadence and cached Value invalidation.
- Action-specific spell/target/range/line-of-sight validation.
- Heal/CC/interrupt reservation ownership, expiry and invalidation under real role strategies.
- No duplicate GCD execution when a domain transitions from Shadow to Strategy mode.
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

## Snapshot/coordinator tests

The standalone harness additionally checks:
- heal reservations can be split across multiple owners and summed;
- snapshot cache freshness and epoch progression;
- coordinator registry reuses a shard for the same group/map/instance key and separates different instances;
- group ID zero does not create a coordinator (battleground path remains Legacy).

The full Core compile includes PartySnapshotBuilder.cpp. This validates adapter API compatibility at compile time, not live thread safety or runtime data correctness.

## CI scope for the PartyBot validation branch

The `work/partybot-engine-foundation` branch intentionally uses the dedicated
`.github/workflows/partybot-core-build.yml` workflow only. That workflow runs
the pure decision-engine tests and a full Vanilla 5875 Linux ARM64 Core build,
matching the target Oracle A1 architecture.

The general `CI Build` workflow retains its full supported-client/compiler/
platform matrix for other branches, but skips this focused validation branch.
Both workflows use concurrency controls, and the dedicated workflow is triggered
by PR updates (not both branch pushes and PR updates). This avoids duplicate
matrix spending while still validating the deployment-relevant target.

A successful focused run is compile/test evidence only; it is not an in-game
test and does not authorize deployment.


## Role-aware Shadow strategy tests

The standalone harness additionally validates:
- the healer triage candidate prefers a critically injured party member over a healthier tank;
- a tank proposes a hostile attacking a healer and does not pull a hostile already attacking the designated Main Tank;
- a DPS bot prefers the Main Tank's current target over a hostile attacking the healer.

These tests exercise pure snapshot-to-candidate logic. They do not test the Core role adapter, hostile collection against live maps, or whether suggestions are tactically correct in an encounter. For live comparison, inspect bounded `[PartyBotShadowDecision]` records alongside Legacy behavior.

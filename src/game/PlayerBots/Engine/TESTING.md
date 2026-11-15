# PartyBot Core build and regression testing

## Automated compile CI

The workflow at `.github/workflows/partybot-core-build.yml` builds this
branch on a GitHub-hosted Linux ARM64 runner. ARM64 is intentional: it matches
the architecture of the Oracle A1 deployment.

The workflow is triggered by:

- pushes to `work/partybot-engine-foundation`;
- pull requests targeting `validation/partybot-engine-base-0e3ff01`;
- manual `workflow_dispatch`.

It configures the project for the Vanilla 1.12.1 / 5875 client and C++14,
then builds all default targets, including `game`, `mangosd` and `realmd`.
The build uses the repository's bundled third-party sources and disables
optional cpptrace and PCH to keep the CI path deterministic and reduce
resource use. Compiler warnings are not promoted to errors because this
legacy codebase has existing warnings unrelated to the PartyBot changes.

A successful run proves that the checked-out source configures, compiles and
links on the CI ARM64 Linux environment. It does not prove runtime behavior,
database compatibility beyond compilation, or successful operation in an
existing deployment. It is not permission to deploy.

The workflow uses GitHub Actions cache for ccache. The cache key is scoped by
runner architecture and branch, with fallback keys for reuse across commits.

## Required in-game regression checks

Run these only on a disposable/test realm, not against the live deployment
without explicit approval.

### Owner disconnect and reconnect

1. Form a party with the owner and PartyBots, then enter an instance.
2. Disconnect the owner while the PartyBots remain in the instance.
3. Confirm the bots remain in the original group and do not trigger immediate
   cleanup or instance reset.
4. Wait through the normal group-leader offline handling and confirm the bots
   continue to exist if a bot is promoted to leader.
5. Reconnect the owner before the configured grace period expires.
6. Confirm the owner is restored as group leader and the existing bots remain
   in the same instance/group.
7. Repeat with a disconnect longer than the configured grace period and
   confirm cleanup occurs only after expiry.
8. Separately verify that intentional owner group-leave/disband and explicit
   bot removal still clean up normally.

### Healer lifecycle

The pre-heal path must report success only when a heal cast is accepted. A
selected target or a failed/unsuitable pre-heal must not suppress the normal
no-action diagnostics or make the caller believe a cast occurred.

1. Test Druid, Shaman and Priest PartyBots with a party member in combat while
   the healer itself is initially out of combat.
2. Confirm the healer enters its existing class combat routine.
3. Confirm it heals eligible injured party members when it has sufficient mana.
4. Enable `PlayerBot.DecisionTrace = 1` on the test realm if it does not act;
   capture the rejection reason rather than guessing at the cause.
5. Confirm out-of-combat food/drink and buff behavior is not started merely
   because a party member is fighting.

### Warrior and Mage diagnostics

1. Confirm the Warrior assists a party member's attacker and uses a valid
   taunt when appropriate.
2. Confirm the trace explains cases where no assist target or usable taunt is
   found.
3. Confirm Mage Arcane Explosion is skipped when fewer than two enemies are
   near the caster, and that the trace reports the skip.

## Validation status vocabulary

Keep these states distinct in PR updates:

- **Committed:** source and documentation are present in the branch.
- **CI configured:** workflow exists; no successful run is implied.
- **CI passed:** the latest relevant workflow run completed successfully.
- **In-game tested:** the scenario above was exercised on a test realm.
- **Deployed:** the user explicitly approved and the intended server was
  updated.

A CI pass must never be described as an in-game test.

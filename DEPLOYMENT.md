# DEPLOYMENT — OTA release runbook

This file is a runbook for **Claude** to execute, not a tutorial for humans.
When the user says one of the trigger phrases below, follow this file step by
step. Always confirm before any irreversible action.

## Trigger phrases (any of these → read this file → execute the runbook)

- "deploy this for the OTA updates"
- "ship a new OTA release"
- "cut a release"
- "publish vX.Y.Z"

If unsure whether a request is a deploy ask: "You want me to cut a new OTA
release (tag + build + publish to GitHub)?" Don't execute on a maybe.

## The contract

The runbook does **all** the steps in one shot, but it asks the user to
confirm ONCE up front with the full plan (version + notes). After that one
"yes," it runs through without further questions unless something fails.

The user **never** flashes the device as part of a deploy — orbs in the field
pick up the new release on their next reboot. (User's standing rule: build to
verify, never flash from Claude.)

---

## Phase 0 — Preflight (silent unless something is wrong)

Run these checks. If anything fails, surface the failure to the user and
STOP — do not try to "fix" it without asking.

```bash
cd /Users/lynch/Documents/Futurity/futuresGarden_ESP32

# 1. Are we in a git repo?
git rev-parse --is-inside-work-tree
# Expected: true
# If "fatal: not a git repository" → jump to Phase 0b (first-time bootstrap)

# 2. Is the working tree clean?
git status --porcelain
# Expected: empty output
# If anything → ask user: "There are uncommitted changes. Commit them as part
# of this release, or stash and release without them?"

# 3. Are we on the branch they want to release?
git branch --show-current
# Most often: main / master. If a feature branch, ask user whether to release
# from this branch or switch first.

# 4. Is the remote reachable + up to date?
git fetch origin
git log HEAD..origin/$(git branch --show-current) --oneline
# If non-empty → ask: "Remote has commits we don't. Pull first?"

# 5. Is the GitHub CLI installed and authenticated?
gh --version
gh auth status
# If "command not found" → STOP and tell user:
#   "I need the `gh` CLI to publish releases. Install it once with:
#       brew install gh && gh auth login
#    Then ask me to deploy again."
# If "not authenticated" → tell user to run `gh auth login` once.

# 6. Does the repo have the OTA macros wired?
grep -E "OTA_GITHUB_OWNER|OTA_GITHUB_REPO" main/secrets.h
# Verify the owner/repo match the actual GitHub remote returned by:
gh repo view --json owner,name
# If mismatched → STOP and ask user to confirm which is right.

# 7. Is the repo public? (OTA fetches anonymously.)
gh repo view --json visibility
# Expected: "PUBLIC". If "PRIVATE" → STOP. Ask user to make it public or to
# add token auth (out of current scope).
```

### Phase 0b — First-time bootstrap (only if step 1 failed)

This happens **once, ever**. After this, every deploy uses Phase 1+.

Tell the user: "This project isn't in git yet. Before I can publish releases
I need to initialize a repo and push to GitHub. Want me to do that now? It'll
create a public repo at `<OTA_GITHUB_OWNER>/<OTA_GITHUB_REPO>` from
main/secrets.h."

On yes:

```bash
cd /Users/lynch/Documents/Futurity/futuresGarden_ESP32

# Make sure secrets.h is NOT going to be committed — the file holds WiFi
# passwords and ElevenLabs key. Verify it's ignored before the first commit.
grep -E "^secrets\.h$|^main/secrets\.h$" .gitignore || \
    echo "main/secrets.h" >> .gitignore

git init -b main
git add .
git commit -m "Initial commit"

# Pull owner/repo from secrets.h (already verified to be set in step 6 above).
OWNER=$(awk -F'"' '/OTA_GITHUB_OWNER/ {print $2}' main/secrets.h)
REPO=$(awk -F'"' '/OTA_GITHUB_REPO/  {print $2}' main/secrets.h)

gh repo create "$OWNER/$REPO" --public --source=. --remote=origin --push
```

Confirm with the user that the repo is visible at
`https://github.com/$OWNER/$REPO` before moving on.

---

## Phase 1 — Determine the next version

The version baked into the binary comes from `git describe`. The release
tag on GitHub is what the orb compares against. They must match.

```bash
# What's the most recent tag?
PREV=$(git tag --sort=-v:refname | head -1)
# If there are no tags yet, default PREV to "v0.0.0" (first release is v0.0.1).

# What changed since the last tag?
git log "$PREV"..HEAD --oneline
# Use these commit subjects as the seed for release notes.
```

Suggest the next version using **semver bump rules**:
- `fix:` or bug-fix-flavored commits only → patch (`v0.0.1` → `v0.0.2`)
- `feat:` or new functionality → minor (`v0.0.1` → `v0.1.0`)
- Anything labeled `BREAKING` or that materially changes behavior the user
  flagged as a "big change" → major (`v0.0.1` → `v1.0.0`)

Default to **patch** if the diff is small and the user didn't say otherwise.

---

## Phase 2 — The ONE confirmation question

Present everything in ONE AskUserQuestion call. This is the only gate; after
"yes" the runbook runs to completion. Format:

```
Cut a new OTA release?

  From: <PREV>  →  To: <NEXT>
  Branch: <current>  (HEAD = <short sha>)
  Changes (<N> commits since <PREV>):
    - <commit subject 1>
    - <commit subject 2>
    - <commit subject 3>
    ...
  After publishing, every orb in the field will pick this up on its next reboot.
```

Options:
- "Yes, publish <NEXT>" (recommended)
- "Yes, but bump to <alternative>" (offer one alternative — usually the next semver tier up)
- "Edit the release notes first"
- (the Other option lets them say "no" or change anything else)

If they pick "edit notes," let them dictate the notes, then re-confirm with
the same question.

If they pick a different version, use that one.

DO NOT proceed past this gate without an affirmative answer.

---

## Phase 3 — Execute (no more questions, unless something errors)

After confirmation:

```bash
cd /Users/lynch/Documents/Futurity/futuresGarden_ESP32
NEXT="<the version from Phase 2>"

# 1. Tag the current commit.
git tag "$NEXT"

# 2. Push the tag (+ any pending commits if user opted in at preflight).
git push
git push origin "$NEXT"

# 3. Build with the new tag in place. git describe will bake $NEXT into the
#    binary's esp_app_get_description()->version field.
source /Users/lynch/.espressif/v5.5/esp-idf/export.sh >/dev/null
idf.py build

# 4. Verify the binary picked up the right version string. If grep doesn't
#    find it, abort the release (tag is local-only at this point; we already
#    pushed it in step 2, but we have not published the GitHub release yet).
strings build/ESP32-S3-Touch-LCD-2.8C-Test.elf | grep -F "$NEXT" | head -3
# If empty → STOP and tell user. The tag is pushed but no release was cut, so
# the orb won't try to update yet. They can delete the tag with:
#   git push --delete origin "$NEXT" && git tag -d "$NEXT"

# 5. Publish the GitHub release with the .bin attached.
gh release create "$NEXT" \
    build/ESP32-S3-Touch-LCD-2.8C-Test.bin \
    --title "$NEXT" \
    --notes "$RELEASE_NOTES"
# RELEASE_NOTES is the bullet list from Phase 2, joined with newlines.
```

---

## Phase 4 — Verify the release is live

```bash
# Confirm GitHub shows it.
gh release view "$NEXT" --json tagName,assets,isLatest

# Should print:
# - tagName: matches $NEXT
# - assets: contains ESP32-S3-Touch-LCD-2.8C-Test.bin with non-zero size
# - isLatest: true

# Sanity check the URL the orb will actually hit:
OWNER=$(awk -F'"' '/OTA_GITHUB_OWNER/ {print $2}' main/secrets.h)
REPO=$(awk -F'"' '/OTA_GITHUB_REPO/  {print $2}' main/secrets.h)
curl -sI "https://api.github.com/repos/$OWNER/$REPO/releases/latest" | head -1
# Expect HTTP/2 200.
```

Report to user: "Released $NEXT. Live at
https://github.com/$OWNER/$REPO/releases/tag/$NEXT . Power-cycle any orb to
pull it."

---

## Phase 5 — Failure recovery

The runbook is structured so the GitHub release is the LAST step. If
anything before it fails, no orb has tried to update yet — recovery is local.

| What broke | What to do |
|---|---|
| `idf.py build` fails | Tag is already pushed. Fix the build, OR delete the tag (`git push --delete origin <tag> && git tag -d <tag>`) and start over. Tell the user which. |
| Version string didn't bake into binary | Same as above. `git describe` probably didn't see the new tag — make sure `git tag` ran before `idf.py build`. |
| `gh release create` fails | Tag is published but no asset. Two options: rerun `gh release create` (it'll fail because tag exists; use `gh release upload <tag> <file>` to attach the asset to the existing release), or delete the release + tag and retry. |
| Released a broken build that bricks the WSS connect | Don't panic — the orb's rollback validation will revert it on the next power cycle (see `orb_ota_mark_running_valid` in [main/Convai/convai.c](main/Convai/convai.c)). Cut a fixed release fast; orbs that did update successfully will pull the fix on their next reboot. |
| Need to recall a release | `gh release delete <tag>` removes it from GitHub; next-newest release becomes "latest" and orbs will downgrade to it on next boot. |

---

## Testing OTA on a dev build — the dev-guard toggle

OTA normally **skips dev builds** (versions containing `-g`/`-dirty`/`NOTAG`) so a
locally flashed image is never auto-downgraded to the latest release. To verify the
OTA path itself on a bench orb, disable that guard:

- Kconfig `CONFIG_ORB_OTA_UPDATE_DEV_BUILDS` (`main/Kconfig.projbuild`), default `n`.
- Enable via `idf.py menuconfig` → *Example Configuration* → *Allow OTA auto-update on
  dev builds*. It lands in the **gitignored `sdkconfig`** — never put it in
  `sdkconfig.defaults`, so it can't leak into a release.
- With it on, a dev image logs `dev-guard DISABLED …` and takes OTA like a release.
- **No-op for clean tagged releases** (their version has no `-g`/`-dirty`/`NOTAG`), so
  the toggle only ever affects dev images on your bench.

Test flow: **publish the target release first.** OTA triggers on any version
*difference*, not "newer" — a guard-off dev build grabs whatever is *latest*, even an
older tag. Then flash the guard-off dev build, power-cycle, and watch the `ota:` lines
over UDP (`nc -ul 6666`). Turn the toggle back off for normal dev so your dev flashes
stay put.

---

## Per-orb identity & provisioning (NVS `device_id`)

Each orb fetches its Supabase config keyed on a `device_id`. **Identity lives in NVS,
not the firmware image** (`main/Wireless/config_fetch.c::orb_device_id`) — OTA ships
ONE binary to every orb, so a baked-in id would repaint the whole fleet on update.

Resolution order in `orb_device_id()`:
1. `DEVICE_ID` set **and** `DEVICE_ID_OVERWRITE=1` → force-rewrite NVS from the
   compile-time id (deliberate re-provision; **never ship a release with this `=1`**).
2. NVS holds a `device_id` → use it. The normal, OTA-safe path.
3. NVS empty → seed once from `DEVICE_ID` (or a `orb-XXXXXX` MAC id if undefined),
   persist, use it.

So `DEVICE_ID` in `secrets.h` is a **one-time provisioning seed**, not the live id.

**Provision a new orb (one cable flash each, all from a clean release tag):**
1. Set `#define DEVICE_ID "<color>"` in `secrets.h`, build, cable-flash.
2. First boot logs `device_id=<color> (seeded into NVS on first boot)` and stamps NVS.
   The orb keeps that id across all future OTA updates.
3. `secrets.h` is gitignored, so swapping the color between orbs doesn't change the
   version — every provisioning build reports the same clean tag, so all orbs stay on
   the OTA track. (Provision from a *release tag*, not a dev build — the dev-guard
   would take those orbs off the OTA track.)

**Published release assets MUST be neutral.** Build the binary you upload to GitHub with
`DEVICE_ID` **commented out**, so any unprovisioned orb that auto-pulls it seeds a
harmless MAC id instead of a color. Verify: `strings build/…bin | grep -iE
'^(black|purple|…)$'` → expect no match.

**Re-provision** an orb to a new color: either `idf.py erase-flash` (wipes NVS → next
boot re-seeds from `DEVICE_ID`) then flash, or set `DEVICE_ID_OVERWRITE 1` for a
no-erase rewrite (flash once, set back to `0`).

**Migration trap**: you can't roll identity out via one shared OTA release — every
unprovisioned orb would seed from that release's baked id at once. Provision each orb by
cable (or accept MAC ids and map them in Supabase). A live orb already on the OTA track
auto-pulls a new neutral release and seeds a MAC id, losing its friendly config until
you cable-provision it — do so promptly (or `erase-flash` + reflash if it beat you).

---

## Changing WiFi (or any `secrets.h` value) via OTA

WiFi networks are a compile-time **priority list** — `WIFI_CREDS` in `secrets.h`, applied
by `wifi_connect_best()` in `main/Wireless/Wireless.c` (scans on boot + each disconnect,
connects to the first listed network in range). So changing/adding networks = a new
binary. Over OTA this works **only if the orb can still reach the internet on a network
it *currently* has listed** to download the release that carries the new list:
- At least one already-listed network still reachable → safe over OTA (rollback covers a
  bad switch *if that network stays up*). The multi-network list makes this easier: any
  one of the listed APs being up is enough to deliver the update.
- None of the currently-listed networks reachable → **cable flash**; there's no path to
  deliver the fix once the orb can't connect to anything it knows.

Practical rollout for a network change: add the NEW network to `WIFI_CREDS` *alongside*
the old one, cut a release, let every orb OTA over the old network, confirm they're
online, then retire the old network (and optionally drop it from the list in a later
release). Because the orb tries the whole list, the switch is seamless.

Hand-flashed `secrets.h` changes (WiFi, `device_id`, etc.) **stick across OTA only if the
flashed build reports the same version as the latest release** — OTA compares version
*strings* (`strcmp`), so a build still reading `vX.Y.Z` is "up to date" and won't
re-download. `secrets.h` is gitignored, so editing it keeps the tree clean and the version
unchanged. Build from the release tag and confirm with
`strings build/…elf | grep -Fx vX.Y.Z` before flashing. A build that reads any other
version would OTA back to the release binary on next boot, discarding your local change.

---

## Notes for me (Claude) when running this

- This runbook does NOT flash the device. The user does that separately if
  they want to test on a tethered orb. Deployment = publish to GitHub.
- The user is fine with you executing — don't second-guess after the Phase 2
  confirmation. No surprise extra questions.
- If `gh` or git is missing entirely, that's the only acceptable place to
  bail out and ask the user to install something. Everything else, just do it.
- Memory note `project_ota_deploy_runbook` points here. Keep them in sync if
  you change the trigger phrases or the high-level shape of the workflow.

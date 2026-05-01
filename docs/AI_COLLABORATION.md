# AI Collaboration Playbook

This project is built collaboratively between **two AI coders** working as
peers, with the user (Mathieu) as the final arbiter. This document is the
contract between them. **Read it at the start of every scheduled session.**

The two collaborators:

- **Claude (you, when reading this)** — runs on the Mac, has the tight
  feedback loop with the codebase, hardware test rig, build/test
  pipeline, git, log analysis, background task scheduling. Inside Claude
  Code with full tool access (Read/Edit/Bash/Agent/Monitor/etc.).
- **ChatGPT 5.5 / Codex CLI** — installed locally at
  `/Users/mathieuvachon/.nvm/versions/node/v24.12.0/bin/codex`,
  authenticated, runs as `gpt-5.5` with `xhigh` reasoning effort and
  `workspace-write` sandbox access. Strong on careful patches with
  thorough tests, edge-case analysis, and second-opinion reviews.

Each AI has weaknesses the other compensates for. Use both.

---

## Default workflow at session start

1. **Read the priority-1 docs** (already required by `CLAUDE.md`):
   - `docs/REFACTOR_PROGRESS.md`
   - `docs/KNOWN_BUGS.md`
   - `docs/INVARIANTS.md`
   - `docs/CHANGELOG.md` (last few entries to know recent work)

2. **Run `git status`** to see if work is in flight or pending.

3. **If pending uncommitted changes exist:**
   - Read the diff — `git diff` (or `git diff --stat` for size)
   - Ask the user (or note in your plan) whether to:
     - Ship with a `codex review --uncommitted` first
     - Continue working
     - Roll back

4. **Identify the task.** If the user has typed a request, use that.
   If this is a `/schedule` autonomous wake-up, infer from the most
   recent CHANGELOG entry + KNOWN_BUGS open items.

---

## When to involve Codex

You don't always need Codex. Use this rule of thumb:

| Situation | Action |
|---|---|
| Routine bug fix, < 50 lines, well-understood | Just code it yourself, run ctest, commit |
| Non-trivial refactor (touching > 3 files or core protocols) | Write the patch, then `codex review --uncommitted` before commit |
| Algorithmic / design change (new feature, new state machine) | Write a `/tmp/<topic>_brief.md`, pipe to `codex exec`, apply patch, verify, iterate |
| Stuck after 2 attempts to fix the same issue | Write findings brief, ask Codex for an orthogonal angle |
| Validating that an INVARIANT isn't being violated | `codex review --uncommitted` with explicit pointer to `INVARIANTS.md` |
| User explicitly delegates "fix it autonomously" | Iterate Claude→Codex→test until convergence (see *autonomous mode* below) |

Don't involve Codex for: trivial fixes, log readings, running tests,
git operations, file moves, doc edits.

---

## The handoff format

When you do involve Codex, **always** use this structure:

### Step 1 — Write the brief

Save to `/tmp/<topic>_findings.md`. The brief must be **self-contained**
(Codex doesn't see your conversation context). Standard sections:

```markdown
# <Topic> — round N

## Status of prior rounds (if iterating)

What's already shipped and what NOT to revert.

## New finding

Symptoms observed (test results, log excerpts).

## Root cause

Where the bug actually is. Code references with file:line.

## Suggested fix

One or more concrete approaches, ordered by recommendation.
Include sketch implementations.

## Test additions

What unit tests should land alongside the fix.

## Reproduction

Command + expected output (before and after).

## Out of scope

Things NOT to touch.

## Files to touch

Bullet list with paths.
```

### Step 2 — Build the prompt

Save to `/tmp/codex_<topic>_prompt.txt`. Format:

```
Read /tmp/<topic>_findings.md — <one-line context>.

Implement <specific option> from the brief.

[Numbered list of 3-7 concrete code changes]

Keep the change tight — no refactor sprawl.

After implementing, run:
  cmake --build build -j4
  ctest --test-dir build --output-on-failure

Report: (a) patch summary with file:line, (b) test results,
(c) any concerns.
```

### Step 3 — Invoke Codex

```bash
codex exec - < /tmp/codex_<topic>_prompt.txt 2>&1 | tee /tmp/codex_<topic>.log | tail -50
```

The `2>&1 | tee | tail` pattern captures the full transcript while
keeping your context window light. Codex reports the patch summary
on stderr at the end.

### Step 4 — Verify before trusting

**Never accept Codex's "tests pass" claim alone.** Always:

1. `git diff` — read the actual changes
2. `cmake --build build -j4` — confirm clean build
3. `ctest --test-dir build --output-on-failure` — confirm 30/30 (or
   whatever the suite count is now)
4. **Hardware smoke test** if the patch touches:
   - `src/protocol/connection*`
   - `src/protocol/selective_repeat_arq*`
   - `src/protocol/frame_v2*`
   - `src/gui/modem/*`
   - `src/ofdm/*`, `src/psk/*`, `src/sync/*`, `src/waveform/*`
   - `src/fec/*`

   Hardware test command:
   ```bash
   SSH_KEY=$HOME/.ssh/id_pi5 ./tools/run_hw_test.sh \
     --file 5120 --rate r1_4 --snr 15 --channel good --inject
   ```
   (Uses 5KB to keep it under 3 min. Bigger if a longer transfer
   is needed to expose the bug.)

5. If hardware test fails, **don't commit**. Iterate (round N+1) or
   roll back.

### Step 5 — If iterating

Round N+1's brief must include **"Status of prior rounds"** explaining
what's already in place that should NOT be reverted. This is critical
because Codex re-reads the codebase fresh each call and could otherwise
undo your prior wins.

---

## Cross-checking: Codex reviews Claude

The reverse direction. When *you* author a non-trivial patch:

```bash
git add -A
codex review --uncommitted 2>&1 | tee /tmp/codex_review.log | tail -100
```

Read its findings critically. Codex will sometimes:
- Catch real bugs (good — fix and re-review)
- Flag false positives (acknowledge, decide to ignore with reason)
- Suggest refactors out of scope for the task (decline politely; the
  scope is defined by the user, not Codex)

You're not obligated to accept every suggestion. You ARE obligated to
read every one.

---

## Autonomous mode

When the user explicitly says "fix it autonomously" / "keep iterating"
/ "just go":

- **No pausing for permission between rounds.** Each round = brief,
  codex, build, ctest, hardware test, decision.
- **Stop only when:**
  - Test passes end-to-end (success — write CHANGELOG, ask permission
    to commit)
  - 4+ rounds without convergence (something is fundamentally wrong;
    write a status report and stop, ask user)
  - A round introduces a *worse* failure than the previous (rollback
    candidate; report)
- **Always write a CHANGELOG entry summarizing the rounds.** Even if
  not committing yet, the entry is the user's read-back.
- **You may NOT push to origin without explicit permission**, even in
  autonomous mode. Commit locally is OK; push is a deliberate user
  action.

The 2026-05-01 adaptive-rate work is the canonical example: 4 rounds,
each round's brief in `/tmp`, each round's codex output captured, each
round verified on hardware before round N+1 was started. Total: 1
clean commit (`64641ac`), no rollbacks needed.

---

## Disagreements

Both AIs can be wrong. Today's work proved it:

- Claude proposed the original adaptive controller (round 0). Codex
  improved it and added two real bug fixes Claude missed (`dataFrameFlags`
  VERSION_V2 clobber, CONNECT_ACK retx race).
- Codex's round-1 patch was correct in scope but incomplete in
  practice. Hardware testing (Claude's domain) exposed thrashing.
- Codex's round-2 patch fixed the thrashing in unit tests but missed
  a fourth-layer ARQ issue. Hardware testing again surfaced it.

**Neither AI is right by default.** When you and Codex disagree:

1. **State both positions clearly.** No appeals to authority.
2. **Find a test that distinguishes them.** Usually a hardware run.
3. **Run it.** Report results to the user.
4. **The losing side updates its understanding** — write a CHANGELOG
   entry or update INVARIANTS.md if a new constraint surfaces.

If the test is ambiguous, surface it to the user as a design question
("we disagree on X — here are both arguments — please decide").

---

## End-of-session cleanup

When work is paused or finished:

1. `/tmp/*_findings.md`, `/tmp/codex_*.log`, `/tmp/codex_*_prompt.txt`
   — delete unless the user explicitly asked to keep them
2. `/tmp/ultra_hw_*` test result dirs — accumulate over weeks; offer
   to clean if there are >50
3. `/tmp/*.log` test launcher logs — delete
4. `git status` clean or all changes committed — never leave
   uncommitted work without telling the user
5. CHANGELOG entry written for any non-trivial work (even if not
   committed)

---

## Why this collaboration matters

The project shipped **rounds 1–4 of the adaptive code-rate work in a
single afternoon**, going from "stuck downgrade bug" to "passing the
50 KB AWGN auto test end-to-end" with full hardware verification. That
required:

- Claude's hardware feedback loop to find each bug
- Codex's careful patch + test design
- Mathieu's strategic direction at each round boundary
- Both AIs honest about what they didn't know

None of the three could have done it alone. Treat future sessions
the same way: **double-check, ask for improvements, run on hardware,
iterate openly**. The user gets a better modem; both AIs get sharper
because they're being checked by the other.

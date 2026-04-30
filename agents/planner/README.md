# Planner

The planner lane is report-only by default. It reads project state and writes
proposed task files; it does not edit modem source, merge PRs, or enqueue work
unless a human explicitly promotes a proposal.

Run once:

```bash
./agents/run_planner.sh
```

Run continuously:

```bash
./agents/planner_watchdog.sh
```

Outputs:

- `agents/planner/reports/`: ignored local planner reports.
- `agents/planner/proposals/`: ignored proposed task files.

Publish proposals to GitHub Issues for remote review:

```bash
./agents/publish_planner_proposals.sh
```

Humans can create follow-up proposals without waiting for the planner:

- From GitHub, open a new issue using "Agent follow-up proposal" or
  "Hardware follow-up proposal".
- From a terminal, fill `agents/manual_followup_template.md` and run
  `agents/create_followup_issue.sh --title "..." --body-file /tmp/followup.md`.

Manual follow-up issues must still be approved explicitly before they are
queued. If the task is blocked by an unmerged PR, leave it unapproved or put it
on hold until the base branch is ready.

Approve from GitHub by commenting one of:

```text
/approve codex
/approve claude
/hold
/reject
```

Only usernames listed in `AGENT_APPROVERS` are honored by the approval watcher.
The watcher ignores all other public comments, even if they contain an approval
command.

Process approved issues once:

```bash
AGENT_APPROVERS=secup ./agents/process_approved_proposals.sh
```

Run the approval watcher continuously:

```bash
AGENT_APPROVERS=secup ./agents/approval_watchdog.sh
```

The approval watcher only writes local ignored task files into
`agents/queue/claude/` or `agents/queue/codex/`. It cannot merge PRs or push to
`main`.

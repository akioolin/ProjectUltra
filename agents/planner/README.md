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

Promote a proposal manually by reviewing it and moving/copying it into
`agents/queue/claude/` or `agents/queue/codex/`.

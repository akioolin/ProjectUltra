# launchd Example

Use this only after `tmux` operation is stable.

Install a local macOS LaunchAgent:

```bash
mkdir -p "$HOME/Library/LaunchAgents"
sed "s#__PROJECT_ROOT__#$PWD#g" \
  agents/launchd/com.projectultra.agent.example.plist \
  > "$HOME/Library/LaunchAgents/com.projectultra.agent.plist"
launchctl load "$HOME/Library/LaunchAgents/com.projectultra.agent.plist"
```

Stop it:

```bash
launchctl unload "$HOME/Library/LaunchAgents/com.projectultra.agent.plist"
```

Logs:

```bash
tail -f /tmp/projectultra-agent.out /tmp/projectultra-agent.err
```

Do not enable `AGENT_AUTO_COMMIT=1`, `AGENT_PUSH=1`, or hardware gates in
launchd until the same settings have run cleanly under `tmux`.

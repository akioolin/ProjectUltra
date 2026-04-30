# Agent Queue

Place one task markdown file per unit of work in this directory.

Recommended naming:

```text
001-short-slug.md
002-next-short-slug.md
```

The runner picks the lexicographically first `*.md` file except this README.
Keep tasks small enough that one agent session can finish, test, and report.

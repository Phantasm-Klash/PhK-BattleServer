## Summary

## Battle Authority Checklist

- [ ] Client input remains intent-only and never authoritative state.
- [ ] Ticket, match, player, sequence, and tick-window checks are preserved.
- [ ] State hashes and replay summaries remain deterministic for the same input stream.
- [ ] Battle Server does not write inventory, rewards, Steam data, or databases.
- [ ] `tools/check_battle_server.py` and battle tests were run locally or are covered by CI.

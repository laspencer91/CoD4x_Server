# Recoil models

`recoil_model` is a server-owned integer, default 0 (native). Set it before the first map. The first
successful selection locks until the process restarts. `recoil_status` lists the model IDs and the
published contract.

A non-native model (1, continuous) is server-authoritative:

- **Per-command fold.** `SV_ClientThink` folds the server's own simulation of each client's view kick
  into the usercmd that `ClientThink` executes (`cl->lastUsercmd`). The wire values, `cmds[]` and
  `PLUGINS_ONCLIENTMOVECOMMAND` keep the client's aim-only angles. Anti-cheat plugins that read the
  plugin event therefore see aim without kick. The player state and demos see the folded angles.
- **Shots.** `G_PlayerEvent`'s recoil call is retargeted to `SV_Recoil_FireRecoil`
  (`asmsource/_g_active_mp.asm`). It runs the native function, then advances the simulation for the
  shot and scales the native gun kick, which moves this client's bullets.
- **Corrections.** `SV_EndClientSnapshot` sends `svc_recoil` (the state after the client's last
  command) while it moves or changes, plus a 500 ms heartbeat. Only clients admitted with
  `cl_recoilProtocol` 2 receive it.
- **Not simulated:** bots, followers (`otherFlags`), and `g_synchronousClients` mode stay native.

The mod supplies `recoil/model_<id>.profile` as a server-only loose file.

`src/recoil/*.h` is generated from the client repository (`CoD4x_Client_pub/tools/sync-recoil.py`). Do
not edit this mirror. Design, evidence and how to add a model: `CoD4x_Client_pub/docs/recoil-models.md`.

Tests:
- `make test-recoil` checks the contract, admission and policy glue.
- The client's `tests/test_recoil.c` checks the shared simulation. Build it with this server's flags
  to check that both builds agree.

The Windows build (`mingw32-make`) links `bin/cod4x18_dedrun.exe`. Only the plugin export step needs
`pexports`.

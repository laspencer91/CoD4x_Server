# Recoil model foundation

`recoil_model` is a server-owned integer, default 0/native. Set it before loading
the first map. A successful selection locks until the server process restarts.
`recoil_status` lists model IDs, effective contract fingerprint and integrated support.

Only native gameplay is integrated. Model 1/continuous is reserved and has a tested
shared kernel/profile, but selecting it refuses the map load with an explicit error.
Accepted-shot state, prediction, snapshot corrections and event-scale integration
remain required before enabling its runtime bit. No player-state layout or native
recoil code is patched by this foundation. Legacy clients remain compatible with 0.

The mod supplies `recoil/model_<id>.profile` as server-only loose files and server-only
`recoil-native.cfg` / `recoil-continuous.cfg` beside the IWD. Values are published as one
ROM/systeminfo `sv_recoilContract`; clients do not select their effective settings.
Non-native admission requires protocol/model support and a matching acknowledgement
after gamestate. Claims in userinfo are not verified code integrity.

`src/recoil/model.h` is generated from the client repository's canonical file. Run
`python tools/sync-recoil.py --check` in that repository when building a matched pair.
Do not edit this mirror. Keep model IDs and wire semantics stable; a new algorithm
needs a new model ID and native implementation, not just a profile file.

The existing makefile discovers `src/sv_recoil.c`. Test its actual server policy with
`make test-recoil`. It links the real info-string helpers (with host services supplied
by the fixture), including a multi-weapon round trip. This fixture supplies host services;
it does not establish a native multiplayer session.

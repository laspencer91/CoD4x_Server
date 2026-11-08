#include "server.h"
#include "filesystem.h"
#include "cmd.h"
#include "qcommon_io.h"
#include "msg.h"
#include "net_game_conf.h"
#include "bg_public.h"
#include "xassets/weapondef.h"
#include "g_shared.h"
#include "recoil/model.h"

/* Server-authoritative recoil (CoD4x_Client_pub docs/authoritative-recoil.md). A non-native model runs
 * the shared simulation (recoil/sim.h) per client: SV_RecoilBeginCommand folds its view kick into each
 * executed usercmd, so everything downstream (bullets, melee, spectators, demos) sees stock-shaped
 * angles; G_PlayerEvent's recoil call (asm) reaches SV_Recoil_FireRecoil for each fire event; and
 * SV_RecoilWriteCorrection sends the state so the client's prediction converges on it. */
/* RECOIL_HEARTBEAT_MSEC bounds how long a misprediction no shot reveals can last. ClientThink_real runs each
 * command at its time clamped to [level.time - RECOIL_GAME_LAG_MSEC, level.time + RECOIL_GAME_LEAD_MSEC]
 * (_g_active_mp.asm, ClientThink_real_10). */
enum { RECOIL_HEARTBEAT_MSEC = 500, RECOIL_GAME_LAG_MSEC = 1000, RECOIL_GAME_LEAD_MSEC = 200 };

void __cdecl BG_WeaponFireRecoil(const playerState_t *ps, float *gunKick, float *viewKick);

typedef struct {
    recoil_sim_t sim;
    qboolean inCommand;
    uint32_t sentShots;             /* sim.shots in the last correction */
    int sentTime;
} sv_recoil_client_t;

static cvar_t *requestedModel, *publishedContract;
static recoil_contract_t active;
static const recoil_model_t *activeModel;
static qboolean locked;
static sv_recoil_client_t recoilClients[MAX_CLIENTS];
static recoil_weapon_cache_t recoilWeapons[RECOIL_WEAPONS];

const recoil_contract_t *SV_RecoilContract(void) { return &active; }

static void SV_RecoilStatus_f(void)
{
    unsigned i;
    Com_Printf(CON_CHANNEL_SERVER,
        "recoil_status: requested=%d active=%u protocol=%d runtime_models=%u contract=%s locked=%d\n",
        requestedModel->integer, active.model, RECOIL_PROTOCOL, Recoil_RuntimeModels(),
        publishedContract->string, locked);
    for (i = 0; i < sizeof(recoil_models) / sizeof(*recoil_models); ++i)
        Com_Printf(CON_CHANNEL_SERVER, "  model %u %s: %s\n", recoil_models[i].id, recoil_models[i].name,
            Recoil_Simulated(&recoil_models[i]) ? "server-simulated" : "stock, client-applied");
}

void SV_RegisterRecoil(void)
{
    requestedModel = Cvar_RegisterInt("recoil_model", 0, INT32_MIN, INT32_MAX, 0,
        "Server recoil model; see recoil_status for IDs/support; restart process to change");
    publishedContract = Cvar_RegisterString("sv_recoilContract", RECOIL_NATIVE_WIRE,
        CVAR_ROM | CVAR_SYSTEMINFO, "Atomic effective recoil model and weapon profiles");
    Recoil_Decode(RECOIL_NATIVE_WIRE, &active);
    activeModel = Recoil_Model(RECOIL_NATIVE);
    Cmd_AddCommand("recoil_status", SV_RecoilStatus_f);
}

void SV_ConfigureRecoil(void)
{
    recoil_contract_t candidate;
    char wire[RECOIL_WIRE_SIZE] = RECOIL_NATIVE_WIRE;
    unsigned model = requestedModel->integer;
    /* Every level load: weapon indices may change. Each client's simulation restarts as it re-enters the world
     * (SV_RecoilEnterWorld). */
    memset(recoilWeapons, 0, sizeof(recoilWeapons));
    if (locked) return;
    if (!Recoil_Model(model)) {
        Com_Error(ERR_DROP, "Unknown recoil_model %u. Use recoil_status to list registered models.", model);
        return;
    }
    if (model != RECOIL_NATIVE) {
        char path[64];
        void *data = NULL;
        int length;
        Com_sprintf(path, sizeof(path), "recoil/model_%u.profile", model);
        length = FS_ReadFile(path, &data);
        if (length < 1 || length > RECOIL_WIRE_SIZE + 1) {
            if (data) FS_FreeFile(data);
            Com_Error(ERR_DROP, "Missing or oversized recoil profile: %s", path);
            return;
        }
        if (memchr(data, '\0', length)) {
            FS_FreeFile(data);
            Com_Error(ERR_DROP, "Embedded NUL in recoil profile: %s", path);
            return;
        }
        while (length && (((char *)data)[length - 1] == '\r' || ((char *)data)[length - 1] == '\n')) --length;
        if (length >= (int)sizeof(wire)) {
            FS_FreeFile(data);
            Com_Error(ERR_DROP, "Oversized recoil payload: %s", path);
            return;
        }
        memcpy(wire, data, length);
        FS_FreeFile(data);
        wire[length] = '\0';
    }
    if (!Recoil_Decode(wire, &candidate) || candidate.model != model) {
        Com_Error(ERR_DROP, "Invalid profile for recoil_model %u; effective model was not changed.", model);
        return;
    }
    active = candidate;
    activeModel = Recoil_Model(model);
    Cvar_SetString(publishedContract, wire);
    Cvar_AddFlags(requestedModel, CVAR_ROM);
    locked = qtrue;
    SV_RecoilStatus_f();
}

qboolean SV_RecoilClientCompatible(const char *userinfo)
{
    const char *text;
    unsigned protocol, models;
    if (active.model == RECOIL_NATIVE) return qtrue;
    text = Info_ValueForKey(userinfo, "cl_recoilProtocol");
    if (!Recoil_Number(&text, &protocol) || *text) return qfalse;
    text = Info_ValueForKey(userinfo, "cl_recoilModels");
    return Recoil_Number(&text, &models) && !*text && Recoil_Compatible(&active, protocol, models);
}

/* Bots never had view kick. g_synchronousClients runs ClientThink_real outside SV_ClientThink, where shots
 * would be lost, so that debug mode stays native. */
static qboolean SV_RecoilSimulated(const client_t *cl)
{
    return Recoil_Simulated(activeModel) && cl->netchan.remoteAddress.type != NA_BOT && !g_synchronousClients->boolean;
}

static const recoil_weapon_kick_t *SV_RecoilWeapon(uint32_t index, const uint32_t **param)
{
    const struct WeaponDef *def;
    *param = NULL;
    if (!index || index >= RECOIL_WEAPONS || (int)index >= BG_GetNumWeapons() || !(def = BG_GetWeaponDef(index)))
        return NULL;
    return Recoil_CachedWeapon(&recoilWeapons[index], &active, def->szInternalName, &def->fHipViewKickPitchMin,
        &def->fAdsViewKickPitchMin, def->hipGunKickReducedKickPercent, def->adsGunKickReducedKickPercent,
        def->iFireTime, param);
}

/* The time the game runs a command at (the clamp in ClientThink_real). Recoil follows game time, so a client
 * that sends times running faster than the game buys no extra recovery. A legitimate client is inside the
 * window, where this is its own time and its prediction agrees. */
static uint32_t SV_RecoilGameTime(int commandTime)
{
    if (commandTime > level.time + RECOIL_GAME_LEAD_MSEC) commandTime = level.time + RECOIL_GAME_LEAD_MSEC;
    if (commandTime < level.time - RECOIL_GAME_LAG_MSEC) commandTime = level.time - RECOIL_GAME_LAG_MSEC;
    return (uint32_t)commandTime;
}

/* SV_ClientEnterWorld (every connect and every round's restart): a fresh simulation, carried to the client by
 * the first snapshot. The shot count starts at a random value: rolls hash the count, so starting at 0 gave
 * every round the same spray, with the same first-shot roll each time. */
void SV_RecoilEnterWorld(client_t *cl)
{
    sv_recoil_client_t *rc = &recoilClients[cl - svs.clients];
    memset(rc, 0, sizeof(*rc));
    if (SV_RecoilSimulated(cl)) Com_RandomBytes((byte *)&rc->sim.shots, sizeof(rc->sim.shots));
}

/* SV_ClientThink, after the raw command is copied into lastUsercmd: recover to the command's game time from
 * the player state before it, then fold the view kick into the copy ClientThink executes (cmds[], plugins
 * and the client's own wire values stay aim-only). */
void SV_RecoilBeginCommand(client_t *cl)
{
    const int clientNum = cl - svs.clients;
    sv_recoil_client_t *rc = &recoilClients[clientNum];
    const playerState_t *ps = SV_GameClientNum(clientNum);
    const uint32_t *param;
    recoil_step_t step;
    int32_t fold[RECOIL_AXES];
    rc->inCommand = qfalse;
    if (!SV_RecoilSimulated(cl)) return;
    step = Recoil_SampleStep(ps->weapFlags, ps->offHandIndex, ps->weapon, ps->fWeaponPosFrac, ps->pm_type,
        ps->otherFlags);
    step.def = SV_RecoilWeapon(step.weapon, &param);
    Recoil_Advance(&rc->sim, activeModel, param, &step, SV_RecoilGameTime(cl->lastUsercmd.serverTime));
    Recoil_Offset(&rc->sim, fold);
    Recoil_FoldAngles(cl->lastUsercmd.angles, fold, 1);
    rc->inCommand = qtrue;
}

void SV_RecoilEndCommand(client_t *cl)
{
    recoilClients[cl - svs.clients].inCommand = qfalse;
}

/* G_PlayerEvent's call for each fire event (asm _g_active_mp.asm). Inside a usercmd of a simulated client the
 * shot drives the simulation and the model scales the native gun kick, which moves this client's bullets. */
void __cdecl SV_Recoil_FireRecoil(const playerState_t *ps, float *gunKick, float *viewKick)
{
    sv_recoil_client_t *rc;
    const uint32_t *param;
    recoil_shot_t shot;
    float before[3];
    VectorCopy(gunKick, before);
    BG_WeaponFireRecoil(ps, gunKick, viewKick);
    if (ps->clientNum < 0 || ps->clientNum >= MAX_CLIENTS || !(rc = &recoilClients[ps->clientNum])->inCommand) return;
    shot = Recoil_SampleShot(ps->weapFlags, ps->offHandIndex, ps->weapon, ps->fWeaponPosFrac,
        ps->weaponRestrictKickTime);
    shot.time = rc->sim.time;   /* the command's game time: SV_RecoilBeginCommand advanced to it */
    shot.shooter = (uint32_t)ps->clientNum;
    if (!(shot.def = SV_RecoilWeapon(shot.weapon, &param))) return;
    Recoil_ScaleGunKick(gunKick, before, Recoil_Shot(&rc->sim, activeModel, param, &shot).gun_scale);
    Com_DPrintf(CON_CHANNEL_SERVER, "recoil: client %d t=%u shots=%u speed=%d,%d,%d\n", ps->clientNum, shot.time,
        rc->sim.shots, rc->sim.kick.speed[0], rc->sim.kick.speed[1], rc->sim.kick.speed[2]);
}

/* SV_EndClientSnapshot: the state after the client's last executed command. Between shots both sides recover
 * from the same inputs, so only a shot gives the client something to check: one correction follows each new
 * shot, whatever the snapshot rate. The heartbeat repairs the rest, such as a shot the client predicted and
 * the server never fired. */
void SV_RecoilWriteCorrection(client_t *cl, msg_t *msg)
{
    sv_recoil_client_t *rc = &recoilClients[cl - svs.clients];
    uint32_t packed[RECOIL_PACKED];
    int i;
    if (!SV_RecoilSimulated(cl) || !rc->sim.time) return;
    if (rc->sim.shots == rc->sentShots && svs.time - rc->sentTime < RECOIL_HEARTBEAT_MSEC) return;
    Recoil_Pack(&rc->sim, packed);
    MSG_WriteByte(msg, svc_recoil);
    for (i = 0; i < RECOIL_PACKED; ++i) MSG_WriteLong(msg, (int)packed[i]);
    rc->sentShots = rc->sim.shots;
    rc->sentTime = svs.time;
}

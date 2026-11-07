#include "server.h"
#include "filesystem.h"
#include "cmd.h"
#include "qcommon_io.h"
#include "recoil/model.h"

static cvar_t *requestedModel, *publishedContract;
static recoil_contract_t active;
static qboolean locked;

const recoil_contract_t *SV_RecoilContract(void) { return &active; }

static void SV_RecoilStatus_f(void)
{
    unsigned i;
    Com_Printf(CON_CHANNEL_SERVER,
        "recoil_status: requested=%d active=%u protocol=%d runtime_models=%u contract=%s locked=%d\n",
        requestedModel->integer, active.model, RECOIL_PROTOCOL, Recoil_RuntimeModels(),
        publishedContract->string, locked);
    for (i = 0; i < sizeof(recoil_models) / sizeof(*recoil_models); ++i)
        Com_Printf(CON_CHANNEL_SERVER, "  model %u %s: %s\n", recoil_models[i].id,
            recoil_models[i].name, recoil_models[i].integrated ?
            "integrated" : "reserved; native integration required");
}

void SV_RegisterRecoil(void)
{
    requestedModel = Cvar_RegisterInt("recoil_model", 0, INT32_MIN, INT32_MAX, 0,
        "Server recoil model; see recoil_status for IDs/support; restart process to change");
    publishedContract = Cvar_RegisterString("sv_recoilContract", RECOIL_NATIVE_WIRE,
        CVAR_ROM | CVAR_SYSTEMINFO, "Atomic effective recoil model and weapon profiles");
    Recoil_Decode(RECOIL_NATIVE_WIRE, &active);
    Cmd_AddCommand("recoil_status", SV_RecoilStatus_f);
}

void SV_ConfigureRecoil(void)
{
    recoil_contract_t candidate;
    char wire[RECOIL_WIRE_SIZE] = RECOIL_NATIVE_WIRE;
    unsigned model = requestedModel->integer;
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
    if (!Recoil_Model(model)->integrated) {
        Com_Error(ERR_DROP,
            "recoil_model %u (%s) is reserved but not integrated into native firing/prediction. Use recoil_model 0.",
            model, Recoil_Model(model)->name);
        return;
    }
    active = candidate;
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

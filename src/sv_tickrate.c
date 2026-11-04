#include "server.h"
#include "tickrate.h"
#include "qcommon_io.h"
#include "cmd.h"
#include "sys_main.h"
#include <stdlib.h>

#define SNAPSHOT_ENTITIES_STOCK 0x2A000 /* stock svs.snapshotEntities count; never allocate fewer */

static clientSnapshot_t *snapshotFrames;
static int deltaCapacity[MAX_CLIENTS];
static int snapshotClientCount;
static int snapshotSlotCount = 32;
static int activeFrameMsec = 50;
static qboolean tickrateLocked;
static cvar_t *tickrateProtocol;
static cvar_t *tickrateActiveMsec;
cvar_t *sv_minSnaps;
cvar_t *sv_minRate;

static void SV_TickrateStatus_f(void)
{
    size_t bytes = (size_t)snapshotClientCount * snapshotSlotCount * sizeof(*snapshotFrames) +
        (size_t)svs.numSnapshotEntities * sizeof(*svs.snapshotEntities) +
        (size_t)svs.numSnapshotClients * sizeof(*svs.snapshotClients);
    Com_Printf(CON_CHANNEL_SERVER,
        "tickrate_status: protocol=%d frame_ms=%d fps=%d time=%d delta_slots=%d history_ms=%d "
        "snapshot_entities=%d snapshot_clients=%d snapshot_memory_mib=%.1f "
        "archive_slots=%d archive_bytes=%d archive_frames=%d archive_oldest_ms=%d\n",
        tickrateLocked ? TICKRATE_PROTOCOL : 0, activeFrameMsec, 1000 / activeFrameMsec,
        svs.time, snapshotSlotCount, activeFrameMsec * (snapshotSlotCount - 4),
        svs.numSnapshotEntities, svs.numSnapshotClients, bytes / 1048576.0,
        NUM_ARCHIVED_FRAMES, ARCHIVEDSSBUF_SIZE, svs.nextArchivedSnapshotFrames,
        SV_ArchiveOldestMsec());
}

void SV_RegisterTickrate(void)
{
    tickrateProtocol = Cvar_RegisterInt("sv_tickrate_protocol", 0, 0, TICKRATE_PROTOCOL,
        CVAR_ROM | CVAR_SERVERINFO | CVAR_SYSTEMINFO, "Installed tickrate client interface version");
    tickrateActiveMsec = Cvar_RegisterInt("sv_tickrate_active_ms", 0, 0, 50,
        CVAR_ROM | CVAR_SERVERINFO | CVAR_SYSTEMINFO, "Active world frame in milliseconds");
    sv_minSnaps = Cvar_RegisterInt("sv_minSnaps", 0, 0, TICKRATE_MAX_FPS, 0,
        "Minimum client snapshots per second; zero uses the world rate above 20 Hz");
    sv_minRate = Cvar_RegisterInt("sv_minRate", 2500, 2500, 100000, 0,
        "Minimum client network rate, limited by sv_maxRate");
    Cmd_AddCommand("tickrate_status", SV_TickrateStatus_f);
}

void SV_ConfigureTickrate(void)
{
    if (!tickrateLocked) {
        int frameMsec = Tickrate_FrameMsec(sv_fps->integer);
        if (!frameMsec) {
            Com_Error(ERR_DROP,
                "Unsupported sv_fps %d; this build supports 20 and 100 Hz. Restart with a supported rate.",
                sv_fps->integer);
            return;
        }
        activeFrameMsec = frameMsec;
#ifdef _WIN32
        if (frameMsec < 50 && !Sys_EnableHighResolutionTimer()) {
            Com_Error(ERR_DROP, "Unable to request the 1 ms timer required for high-rate server pacing");
            return;
        }
#endif
        Cvar_AddFlags(sv_fps, CVAR_ROM);
        tickrateLocked = qtrue;
    }
    /* sv.frameusec is set in SV_PostLevelLoad, after SV_ClearServer has wiped sv. */
    Cvar_SetInt(tickrateProtocol, TICKRATE_PROTOCOL);
    Cvar_SetInt(tickrateActiveMsec, activeFrameMsec);
    Com_Printf(CON_CHANNEL_SERVER, "Tickrate: %d Hz, %d ms; restart process to change.\n",
        1000 / activeFrameMsec, activeFrameMsec);
}

int SV_FrameMsec(void)
{
    return activeFrameMsec;
}

/* Parsed once per userinfo change; read on every snapshot build. */
int SV_ClientDeltaCapacity(const client_t *client)
{
    int capacity = deltaCapacity[client - svs.clients];
    return capacity < snapshotSlotCount ? capacity : snapshotSlotCount;
}

int SV_SnapshotSlots(void)
{
    return snapshotSlotCount;
}

void SV_UpdateClientDeltaCapacity(client_t *client)
{
    deltaCapacity[client - svs.clients] = Tickrate_DeltaCapacity(Info_ValueForKey(client->userinfo, "cl_deltaSlots"));
}

qboolean SV_TickrateClientCompatible(const char *userinfo)
{
    return activeFrameMsec >= 50 ||
        (!Q_stricmp(Info_ValueForKey(userinfo, "cl_tickrateVersion"), "1") &&
         Tickrate_DeltaCapacity(Info_ValueForKey(userinfo, "cl_deltaSlots")) == TICKRATE_PACKET_BACKUP);
}

void SV_FreeSnapshotStorage(void)
{
    free(snapshotFrames);
    free(svs.snapshotEntities);
    free(svs.snapshotClients);
    snapshotFrames = NULL;
    svs.snapshotEntities = NULL;
    svs.snapshotClients = NULL;
    snapshotClientCount = 0;
    svs.numSnapshotEntities = svs.numSnapshotClients = 0;
}

void SV_InitSnapshotStorage(void)
{
    int clients = sv_maxclients->integer;
    int slots = activeFrameMsec == 50 ? 32 : TICKRATE_PACKET_BACKUP;
    size_t entities = (size_t)clients * slots * 64;
    size_t clientStates = (size_t)clients * slots * clients;
    if (entities < SNAPSHOT_ENTITIES_STOCK) entities = SNAPSHOT_ENTITIES_STOCK;
    SV_FreeSnapshotStorage();
    snapshotFrames = calloc((size_t)clients * slots, sizeof(*snapshotFrames));
    svs.snapshotEntities = calloc(entities, sizeof(*svs.snapshotEntities));
    svs.snapshotClients = calloc(clientStates, sizeof(*svs.snapshotClients));
    if (!snapshotFrames || !svs.snapshotEntities || !svs.snapshotClients)
        Com_Error(ERR_FATAL, "Unable to allocate snapshot history");
    snapshotClientCount = clients;
    snapshotSlotCount = slots;
    svs.numSnapshotEntities = (int)entities;
    svs.numSnapshotClients = (int)clientStates;
}

clientSnapshot_t *SV_ClientSnapshot(client_t *client, int sequence)
{
    int index = client - svs.clients;
    assert(snapshotFrames && index >= 0 && index < snapshotClientCount);
    return &snapshotFrames[index * snapshotSlotCount + (sequence & (snapshotSlotCount - 1))];
}

/* The frames live outside client_t, so the connect-time memset of the client does not reach them. */
void SV_ResetClientSnapshots(client_t *client)
{
    clientSnapshot_t *frames = SV_ClientSnapshot(client, 0);
    int i;
    memset(frames, 0, snapshotSlotCount * sizeof(*frames));
    for (i = 0; i < snapshotSlotCount; ++i)
        frames[i].messageAcked = 0xffffffff;
    SV_UpdateClientDeltaCapacity(client);
}

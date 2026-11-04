#include "../../src/server.h"
#include "../../src/g_shared.h"
#include <stddef.h>
#include <stdio.h>
#define FIELD(label,type,field) printf("\"%s\":%u,", label, (unsigned)offsetof(type,field))
int main(void) {
    printf("{");
    FIELD("level_framerate",level_locals_t,framerate);
    FIELD("level_entities",level_locals_t,gentities);
    FIELD("level_time",level_locals_t,time);
    FIELD("level_previous",level_locals_t,previousTime);
    FIELD("level_frame_time",level_locals_t,frameTime);
    FIELD("level_frame_number",level_locals_t,framenum);
    FIELD("entity_type",gentity_t,s.eType);
    FIELD("entity_processed",gentity_t,processedFrame);
    FIELD("entity_inuse",gentity_t,r.inuse);
    FIELD("entity_use_count",gentity_t,useCount);
    FIELD("entity_tag",gentity_t,tagInfo);
    FIELD("entity_parent",gentity_t,parent);
    FIELD("entity_throw_time",gentity_t,s.lerp.pos.trTime);
    FIELD("server_clients",serverStatic_t,clients);
    FIELD("client_userinfo",client_t,userinfo);
    FIELD("client_state",client_t,state);
    FIELD("client_delta",client_t,deltaMessage);
    FIELD("client_outgoing",client_t,netchan.outgoingSequence);
    FIELD("client_gentity",client_t,gentity);
    FIELD("client_ping",client_t,ping);
    FIELD("client_address_type",client_t,netchan.remoteAddress.type);
    FIELD("header_clients",svsHeader_t,clients);
    FIELD("header_time",svsHeader_t,time);
    FIELD("header_entity_count",svsHeader_t,numSnapshotEntities);
    FIELD("header_client_count",svsHeader_t,numSnapshotClients);
    FIELD("snapshot_sent",clientSnapshot_t,messageSent);
    FIELD("snapshot_ack",clientSnapshot_t,messageAcked);
    FIELD("snapshot_time",clientSnapshot_t,var_03);
    FIELD("cvar_integer",cvar_t,integer);
    printf("\"active_state\":%d,\"ip_address_type\":%d,", CS_ACTIVE, NA_IP);
    printf("\"snapshot_size\":%u,\"entity_size\":%u}", (unsigned)sizeof(clientSnapshot_t), (unsigned)sizeof(gentity_t));
    return 0;
}

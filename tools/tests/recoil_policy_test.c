/* Test real server glue with host services supplied by this fixture. */
#include "../../src/sv_recoil.c"
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>

static cvar_t optionVar, contractVar;
static char published[RECOIL_WIRE_SIZE], failure[512];
static const char *profile;
static int fileReads;

cvar_t *Cvar_RegisterInt(const char *name, int value, int min, int max, unsigned short flags, const char *description)
{
    (void)min; (void)max; (void)description;
    assert(!strcmp(name, "recoil_model"));
    optionVar.integer = value; optionVar.flags = flags;
    return &optionVar;
}
cvar_t *Cvar_RegisterString(const char *name, const char *value, unsigned short flags, const char *description)
{
    (void)description;
    assert(!strcmp(name, "sv_recoilContract"));
    assert((flags & (CVAR_ROM | CVAR_SYSTEMINFO)) == (CVAR_ROM | CVAR_SYSTEMINFO));
    strcpy(published, value); contractVar.string = published;
    return &contractVar;
}
void Cvar_SetString(cvar_t *var, const char *value) { assert(var == &contractVar); strcpy(published, value); }
void Cvar_AddFlags(cvar_t *var, unsigned short flags) { var->flags |= flags; }
qboolean Cmd_AddCommand(const char *name, xcommand_t callback)
{ assert(!strcmp(name, "recoil_status") && callback); return qtrue; }
void Com_Printf(conChannel_t channel, const char *format, ...) { (void)channel; (void)format; }
void Com_Error(int level, const char *format, ...)
{
    va_list args; (void)level;
    va_start(args, format); vsnprintf(failure, sizeof(failure), format, args); va_end(args);
}
void Com_PrintWarning(conChannel_t channel, const char *format, ...) { (void)channel; (void)format; }
void Com_DPrintf(conChannel_t channel, const char *format, ...) { (void)channel; (void)format; }
int Q_vsnprintf(char *dest, size_t size, const char *format, va_list args)
{ return vsnprintf(dest, size, format, args); }
int FS_ReadFile(const char *path, void **data)
{
    ++fileReads; assert(!strcmp(path, "recoil/model_1.profile"));
    if (!profile) { *data = NULL; return -1; }
    *data = malloc(strlen(profile) + 1); strcpy(*data, profile);
    return strlen(profile);
}
void FS_FreeFile(void *data) { free(data); }

int main(int argc, char **argv)
{
    {
        char info[BIG_INFO_STRING] = "";
        const char *wire = "1:1:ak74u,2500,3,7500/mp5,5000,4,5000";
        BigInfo_SetValueForKey(info, "sv_recoilContract", wire);
        assert(!strcmp(Info_ValueForKey(info, "sv_recoilContract"), wire));
        assert(Info_Validate(info));
        BigInfo_SetValueForKey(info, "sv_recoilContract", "1:1:a,2500,3,7500;b,2500,3,7500");
        assert(!strcmp(Info_ValueForKey(info, "sv_recoilContract"), wire));
    }
    SV_RegisterRecoil();
    assert(optionVar.integer == 0 && !strcmp(published, "1:0"));
    optionVar.integer = 31;
    SV_ConfigureRecoil();
    assert(strstr(failure, "Unknown recoil_model") && !locked && !fileReads);
    optionVar.integer = 1; profile = NULL;
    SV_ConfigureRecoil();
    assert(strstr(failure, "Missing") && !locked);
    profile = "1:1:ak74u,2500,1,7500";
    SV_ConfigureRecoil();
    assert(strstr(failure, "Invalid profile") && !locked);
    profile = "1:1:ak74u,2500,3,7500\r\n";
    SV_ConfigureRecoil();
    assert(strstr(failure, "not integrated") && !locked && !strcmp(published, "1:0"));
    {
        FILE *corpus;
        char line[1024];
        int found = 0;
        assert(argc == 2);
        corpus = fopen(argv[1], "rb"); assert(corpus);
        while (fgets(line, sizeof(line), corpus)) {
            size_t length = strlen(line);
            while (length && (line[length - 1] == '\r' || line[length - 1] == '\n')) line[--length] = 0;
            if (line[0] == '1' && strlen(line + 2) == RECOIL_WIRE_SIZE - 1) {
                strcat(line, "\r\n"); profile = line + 2;
                SV_ConfigureRecoil();
                assert(strstr(failure, "not integrated") && !locked); ++found;
            }
        }
        fclose(corpus); assert(found == 1);
    }
    optionVar.integer = 0; failure[0] = 0;
    SV_ConfigureRecoil();
    assert(!failure[0] && locked && (optionVar.flags & CVAR_ROM));
    assert(SV_RecoilClientCompatible("", qtrue));
    /* Exercise future admission/ack policy without advertising runtime support. */
    assert(Recoil_Decode("1:1:ak74u,2500,3,7500", &active));
    Recoil_Fingerprint("1:1:ak74u,2500,3,7500", activeFingerprint);
    assert(!SV_RecoilClientCompatible("", qfalse));
    assert(!SV_RecoilClientCompatible("\\cl_recoilProtocol\\1\\cl_recoilModels\\1", qfalse));
    assert(SV_RecoilClientCompatible("\\cl_recoilProtocol\\1\\cl_recoilModels\\3", qfalse));
    assert(!SV_RecoilClientCompatible("\\cl_recoilProtocol\\1\\cl_recoilModels\\3", qtrue));
    {
        char info[128];
        snprintf(info, sizeof(info), "\\cl_recoilProtocol\\1\\cl_recoilModels\\3\\cl_recoilAck\\%s", activeFingerprint);
        assert(SV_RecoilClientCompatible(info, qtrue));
        info[strlen(info) - 1] ^= 1;
        assert(!SV_RecoilClientCompatible(info, qtrue));
    }
    puts("server recoil default, activation refusal, capability and acknowledgement checks passed");
    return 0;
}

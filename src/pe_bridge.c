/* Thin Windows steamclient64.dll. Forwards to the Mac bridge in steamclient64.so. */
typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef long NTSTATUS;
typedef NTSTATUS (__attribute__((ms_abi)) *unixcall_fn)(u64 handle, unsigned int code, void *args);
typedef NTSTATUS (__attribute__((ms_abi)) *qvm_fn)(void *, const void *, int, void *, u64, u64 *);
typedef void *(__attribute__((ms_abi)) *gpa_fn)(void *, const char *);
struct ustr { u16 len, max; u16 *buf; };
struct args { u64 a[8]; u64 ret; };
static unixcall_fn g_call;
static u64 g_funcs;
static int g_ready;
static void *pe_export(unsigned char *base, const char *name) {
    if (!base || *(unsigned short *)base != 0x5a4d) return 0;
    int lfanew = *(int *)(base + 0x3c);
    unsigned char *nt = base + lfanew;
    u32 exp_rva = *(u32 *)(nt + 24 + 112);
    if (!exp_rva) return 0;
    unsigned char *exp = base + exp_rva;
    u32 nnames = *(u32 *)(exp + 24);
    u32 funcs = *(u32 *)(exp + 28);
    u32 names = *(u32 *)(exp + 32);
    u32 ords = *(u32 *)(exp + 36);
    for (u32 i = 0; i < nnames; i++) {
        const char *nm = (const char *)(base + *(u32 *)(base + names + i * 4));
        const char *a = nm, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) {
            u16 ord = *(u16 *)(base + ords + i * 2);
            return base + *(u32 *)(base + funcs + ord * 4);
        }
    }
    return 0;
}
static int wmatch(const u16 *a, const u16 *b) {
    while (*a && *b) {
        u16 ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}
static unsigned char *ntdll_base(void) {
    static const u16 want[] = {'n','t','d','l','l','.','d','l','l',0};
    unsigned char *teb;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb));
    unsigned char *peb = *(unsigned char **)(teb + 0x60);
    unsigned char *ldr = *(unsigned char **)(peb + 0x18);
    unsigned char *head = ldr + 0x10; /* InLoadOrderModuleList */
    for (unsigned char *e = *(unsigned char **)head; e && e != head; e = *(unsigned char **)e) {
        u16 *nm = *(u16 **)(e + 0x60); /* BaseDllName.Buffer */
        if (nm && wmatch(nm, want)) return *(unsigned char **)(e + 0x30);
    }
    return 0;
}
static void ensure(void) {
    u64 res[2];
    static u16 name[] = {'s','t','e','a','m','c','l','i','e','n','t','6','4',0};
    struct ustr us;
    qvm_fn qvm;
    if (g_ready) return;
    g_ready = 1;
    unsigned char *nt = ntdll_base();
    g_call = (unixcall_fn)pe_export(nt, "__wine_unix_call");
    qvm = (qvm_fn)pe_export(nt, "NtQueryVirtualMemory");
    if (!g_call || !qvm) return;
    us.len = 26; us.max = 28; us.buf = name;
    res[0] = res[1] = 0;
    if (qvm((void *)-1, &us, 1002, res, sizeof res, 0)) return;
    g_funcs = res[1];
}
static u64 call(unsigned int code, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
    struct args A;
    ensure();
    A.a[0]=a0; A.a[1]=a1; A.a[2]=a2; A.a[3]=a3; A.a[4]=a4; A.a[5]=a5; A.a[6]=a6; A.a[7]=a7; A.ret=0;
    if (g_call && g_funcs) g_call(g_funcs, code, &A);
    return A.ret;
}

__declspec(dllexport) u64 CreateInterface(u64 a0, u64 a1) { return call(0, a0, a1, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_BConnected(u64 a0) { return call(1, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_BGetCallback(u64 a0, u64 a1) { return call(2, a0, a1, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_BLoggedOn(u64 a0) { return call(3, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_BReleaseSteamPipe(u64 a0) { return call(4, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_ConnectToGlobalUser(u64 a0) { return call(5, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_CreateGlobalUser(u64 a0) { return call(6, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_CreateLocalUser(u64 a0) { return call(7, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_CreateSteamPipe(void) { return call(8, 0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_FreeLastCallback(u64 a0) { return call(9, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSBLoggedOn(u64 a0) { return call(10, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSBSecure(u64 a0) { return call(11, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSGetSteam2GetEncryptionKeyToSendToNewClient(u64 a0, u64 a1, u64 a2, u64 a3) { return call(12, a0, a1, a2, a3, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSGetSteamID(u64 a0) { return call(13, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSLogOff(u64 a0) { return call(14, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSLogOn(u64 a0) { return call(15, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSRemoveUserConnect(u64 a0) { return call(16, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSSendSteam2UserConnect(u64 a0) { return call(17, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSSendSteam3UserConnect(u64 a0) { return call(18, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSSendUserDisconnect(u64 a0) { return call(19, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSSendUserStatusResponse(u64 a0) { return call(20, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSSetServerType(u64 a0) { return call(21, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSSetSpawnCount(u64 a0) { return call(22, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GSUpdateStatus(u64 a0) { return call(23, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_GetAPICallResult(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) { return call(24, a0, a1, a2, a3, a4, a5, 0, 0); }
__declspec(dllexport) u64 Steam_GetGSHandle(u64 a0) { return call(25, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_InitiateGameConnection(u64 a0) { return call(26, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_IsKnownInterface(u64 a0) { return call(27, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_LogOff(u64 a0) { return call(28, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_LogOn(u64 a0) { return call(29, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_NotifyMissingInterface(u64 a0, u64 a1) { return call(30, a0, a1, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_ReleaseThreadLocalMemory(u64 a0) { return call(31, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_ReleaseUser(u64 a0) { return call(32, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_SetLocalIPBinding(u64 a0, u64 a1) { return call(33, a0, a1, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Steam_TerminateGameConnection(u64 a0) { return call(34, a0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 SteamBridgeWait(u64 a0, u64 a1, u64 a2, u64 a3) { return call(35, a0, a1, a2, a3, 0, 0, 0, 0); }
__declspec(dllexport) u64 SteamBridgeProbe(u64 a0, u64 a1, u64 a2, u64 a3) { return call(36, a0, a1, a2, a3, 0, 0, 0, 0); }
__declspec(dllexport) u64 Breakpad_SteamMiniDumpInit(void) { return call(37, 0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Breakpad_SteamSendMiniDump(void) { return call(38, 0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Breakpad_SteamSetAppID(void) { return call(39, 0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Breakpad_SteamSetSteamID(void) { return call(40, 0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Breakpad_SteamWriteMiniDumpSetComment(void) { return call(41, 0, 0, 0, 0, 0, 0, 0, 0); }
__declspec(dllexport) u64 Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId(void) { return call(42, 0, 0, 0, 0, 0, 0, 0, 0); }

__declspec(dllexport) u64 SteamBridgeProbeA(u64 a0, u64 a1, u64 a2, u64 a3) { return SteamBridgeProbe(a0, a1, a2, a3); }
__declspec(dllexport) int DllMain(void *inst, unsigned long reason, void *reserved) {
    (void)inst; (void)reserved;
    if (reason == 1) ensure();
    return 1;
}


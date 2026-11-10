// IcdLinkApi.h - flat C ABI of IcdLink.dll. The C# runtime and the CLI use this.
#pragma once
#include <stdint.h>

#if defined(ICDLINK_DLL_EXPORTS)
#define IL_API __declspec(dllexport)
#elif defined(ICDLINK_STATIC)
#define IL_API
#else
#define IL_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* IL_Handle;

typedef struct IL_ChannelConfig {
    uint32_t baud, periodUs, windowUs, rxTimeoutUs, spinUs, trailerFf, rs485Echo, reserved;
} IL_ChannelConfig;

typedef struct IL_ChannelStats {
    uint64_t slots, txFrames, rxFrames, crcErrors, rxTimeouts;
    uint64_t windowMisses, missedSlots, strayBytes, ioErrors, echoErrors;
    uint64_t oneShots, longOps, longOpWatchdogs;
    uint64_t lastLatenessUs, maxLatenessUs, lastExchangeUs;
} IL_ChannelStats;

typedef struct IL_MsgMeta {
    uint64_t rxSeq, rxTag, lastCommit, sentTag;
    int64_t  rxAgeUs;
    uint32_t rxValid, reserved;
} IL_MsgMeta;

// Every function returning int32_t returns 0 or a negative icd::Status.
// ch is 0 for channel C and 1 for channel D.
IL_API int32_t  IL_Create(IL_Handle* out);
IL_API void     IL_Destroy(IL_Handle h);
IL_API uint32_t IL_SchemaHash(void);
IL_API int32_t  IL_DefaultConfig(int32_t ch, IL_ChannelConfig* out);
IL_API int32_t  IL_ListFtdi(char* buffer, int32_t capacity);  // one interface per line, tab separated
IL_API int32_t  IL_ListCom(char* buffer, int32_t capacity);   // one port per line
IL_API int32_t  IL_LastError(IL_Handle h, char* buffer, int32_t capacity);
IL_API int32_t  IL_LastWarning(IL_Handle h, char* buffer, int32_t capacity);

IL_API int32_t  IL_OpenFtdi(IL_Handle h, int32_t ch, const char* serialBase, const IL_ChannelConfig* cfg);
IL_API int32_t  IL_OpenCom(IL_Handle h, int32_t ch, const char* port, const IL_ChannelConfig* cfg);
IL_API int32_t  IL_OpenSim(IL_Handle h, int32_t ch, const IL_ChannelConfig* cfg);
IL_API int32_t  IL_SimSetFaults(IL_Handle h, int32_t ch, uint32_t dropEvery, uint32_t corruptEvery, uint32_t delayUs);
IL_API int32_t  IL_Close(IL_Handle h, int32_t ch);
IL_API int32_t  IL_IsOpen(IL_Handle h, int32_t ch);

IL_API int32_t  IL_SetCyclic(IL_Handle h, int32_t ch, int32_t enable);
IL_API int32_t  IL_SetCyclicMessage(IL_Handle h, int32_t msg, int32_t enable);
IL_API int32_t  IL_SetPeriod(IL_Handle h, int32_t ch, uint32_t periodUs);

IL_API int32_t  IL_TxnBegin(IL_Handle h, uint32_t timeoutMs, uint64_t* token);
IL_API int32_t  IL_TxnEnd(IL_Handle h, uint64_t token);
IL_API int32_t  IL_Commit(IL_Handle h, uint64_t token, const int32_t* msgs, int32_t count, const uint8_t* flat, uint64_t* seq);
IL_API int32_t  IL_WaitApplied(IL_Handle h, uint64_t seq, uint32_t timeoutMs);
IL_API int32_t  IL_Snapshot(IL_Handle h, uint8_t* tx, uint8_t* rx, IL_MsgMeta* meta, uint64_t* commitSeq);
IL_API int32_t  IL_ReadMsg(IL_Handle h, int32_t msg, int32_t side, uint8_t* out, IL_MsgMeta* meta);
IL_API int32_t  IL_Exchange(IL_Handle h, int32_t msg, uint32_t timeoutMs, uint8_t* rxOut);

IL_API int32_t  IL_LongOpBegin(IL_Handle h, int32_t ch, uint32_t watchdogMs, uint32_t timeoutMs);
IL_API int32_t  IL_LongOpEnd(IL_Handle h, int32_t ch);

IL_API int32_t  IL_GetStats(IL_Handle h, int32_t ch, IL_ChannelStats* out);
IL_API int32_t  IL_ResetStats(IL_Handle h, int32_t ch);

#ifdef __cplusplus
}
#endif

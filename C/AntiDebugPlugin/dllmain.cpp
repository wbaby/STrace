#include <stdint.h>
#include <intrin.h>

#include "Interface.h"
#include "crt.h"
#include "utils.h"
#include "config.h"
#include "probedefs.h"
#include "string.h"
#include "magic_enum.hpp"


#pragma warning(disable: 6011)
PluginApis g_Apis;

#define LOG_DEBUG(fmt,...)  g_Apis.pLogPrint(LogLevelDebug, __FUNCTION__, fmt,   __VA_ARGS__)
#define LOG_INFO(fmt,...)   g_Apis.pLogPrint(LogLevelInfo,  __FUNCTION__, fmt,   __VA_ARGS__)
#define LOG_WARN(fmt,...)   g_Apis.pLogPrint(LogLevelWarn,  __FUNCTION__, fmt,   __VA_ARGS__)
#define LOG_ERROR(fmt,...)  g_Apis.pLogPrint(LogLevelError, __FUNCTION__, fmt,   __VA_ARGS__)

extern "C" __declspec(dllexport) void StpInitialize(PluginApis& pApis) {
    g_Apis = pApis;
    LOG_INFO("Plugin Initializing...\r\n");

    g_Apis.pSetCallback("QueryInformationProcess", PROBE_IDS::IdQueryInformationProcess);
    g_Apis.pSetCallback("QueryInformationThread", PROBE_IDS::IdQueryInformationThread);
    g_Apis.pSetCallback("GetContextThread", PROBE_IDS::IdGetContextThread);
    g_Apis.pSetCallback("SetInformationThread", PROBE_IDS::IdSetInformationThread);
    g_Apis.pSetCallback("Close", PROBE_IDS::IdClose);


    LOG_INFO("Plugin Initialized\r\n");
}
ASSERT_INTERFACE_IMPLEMENTED(StpInitialize, tStpInitialize, "StpInitialize does not match the interface type");

extern "C" __declspec(dllexport) void StpDeInitialize() {
    LOG_INFO("Plugin DeInitializing...\r\n");

    g_Apis.pUnsetCallback("QueryInformationProcess");
    g_Apis.pUnsetCallback("QueryInformationThread");
    g_Apis.pUnsetCallback("GetContextThread");
    g_Apis.pUnsetCallback("SetInformationThread");
    g_Apis.pUnsetCallback("Close");

    LOG_INFO("Plugin DeInitialized\r\n");
}
ASSERT_INTERFACE_IMPLEMENTED(StpDeInitialize, tStpDeInitialize, "StpDeInitialize does not match the interface type");

void PrintStackTrace(CallerInfo& callerinfo) {
    for (int i = 0; i < callerinfo.frameDepth; i++) {
        if ((callerinfo.frames)[i].frameaddress) {
            const auto modulePathLen = (callerinfo.frames)[i].modulePath ? strlen((callerinfo.frames)[i].modulePath) : 0;

            // add brackets around module dynamically
            if (modulePathLen) {
                char moduleName[sizeof(CallerInfo::StackFrame::modulePath) + 2] = { 0 };
                moduleName[0] = '[';
                strcpy(&moduleName[1], (callerinfo.frames)[i].modulePath);
                moduleName[modulePathLen + 1] = ']';

                LOG_INFO("  %-18s +0x%08llx\r\n", moduleName, (callerinfo.frames)[i].frameaddress - (callerinfo.frames)[i].modulebase);
            }
            else {
                LOG_INFO("  %-18s 0x%016llx\r\n", "[UNKNOWN MODULE]", (callerinfo.frames)[i].frameaddress);
            }
        }
        else {
            LOG_INFO("  Frame Missing\r\n");
        }
    }
}

void LiveKernelDump(LiveKernelDumpFlags flags)
{
    const auto MANUALLY_INITIATED_CRASH = 0xE2;
    DbgkWerCaptureLiveKernelDump(L"STRACE", MANUALLY_INITIATED_CRASH, 1, 3, 3, 7, flags);
}

extern "C" __declspec(dllexport) bool StpIsTarget(CallerInfo & callerinfo) {
    if (strcmp(callerinfo.processName, "al-khaser.exe") == 0) {
        return true;
    }
    return false;
}
ASSERT_INTERFACE_IMPLEMENTED(StpIsTarget, tStpIsTarget, "StpIsTarget does not match the interface type");

enum TLS_SLOTS : uint8_t {
    PROCESS_INFO_CLASS = 0,
    PROCESS_INFO_DATA = 1,
    PROCESS_INFO_DATA_LEN = 2,

    CONTEXT_THREAD_DATA = 3,

    THREAD_INFO_HANDLE = 4,
    THREAD_INFO_CLASS = 5,
    THREAD_INFO_DATA = 6,
    THREAD_INFO_DATA_LEN = 7,

    CLOSE_RETVAL = 8,
    CLOSE_OVERWRITE_RETVAL = 9,
};

/*
This is a funny little trick. In a switch case, if you define a new scope with locals they all
get lifted to the parent scope which can allocate lots of stack space even if that case isn't
always taken. The fix for that is to not define locals in a switch case, and call a function instead.
But that's annoying and breaks cleanly putting the code in the switch body. Instead, we can define a lambda.

The lambda acts like we made a function, which we ensure is true by forcing noinline. This way stack space is only
allocated if the case is taken. This basically is a technique to declare a global function, while within a function.
*/
#define NEW_SCOPE(code) [&]() DECLSPEC_NOINLINE { code }()

// no change to retval
DECLSPEC_NOINLINE void noop() {
    volatile uint64_t noop = 0x1337;
}

// Do same checks as original, but otherwise nothing except say ok
DECLSPEC_NOINLINE NTSTATUS NoopNtSetInformationThread(
    HANDLE ThreadHandle,
    THREADINFOCLASS ThreadInformationClass,
    PVOID ThreadInformation,
    ULONG ThreadInformationLength
) {
    auto PreviousMode = ExGetPreviousMode();
    ULONG ProbeAlignment = 0;

    if (PreviousMode != KernelMode) {
        switch (ThreadInformationClass) {
        case THREADINFOCLASS::ThreadHideFromDebugger:
            ProbeAlignment = sizeof(ULONG);
        }

        // mimick ProbeForRead
        if (ThreadInformationLength) {
            KIRQL oldIrql = KfRaiseIrql(DTRACE_IRQL);
            uint64_t tmp = 0;
            if (!g_Apis.pTraceAccessMemory(&tmp, (ULONG_PTR)ThreadInformation, 1, 1, true)) {
                KeLowerIrql(oldIrql);
                return STATUS_ACCESS_VIOLATION;
            }
            KeLowerIrql(oldIrql);
        }
    }

    switch (ThreadInformationClass) {
    case THREADINFOCLASS::ThreadHideFromDebugger:
        if (ThreadInformationLength != 0) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }

        // check if handle is valid
        HANDLE Thread = 0;
        auto status = ObReferenceObjectByHandle(ThreadHandle,
            THREAD_SET_INFORMATION,
            NULL,
            PreviousMode,
            &Thread,
            NULL);

        if (!NT_SUCCESS(status)) {
            return status;
        }
        break;
    }

    return STATUS_SUCCESS;
}

/**
pService: Pointer to system service from SSDT
probeId: Identifier given in KeSetSystemServiceCallback for this syscall callback
paramCount: Number of arguments this system service uses
pArgs: Argument array, usually x64 fastcall registers rcx, rdx, r8, r9
pArgSize: Length of argument array, usually hard coded to 4
pStackArgs: Pointer to stack area containing the rest of the arguments, if any
**/
extern "C" __declspec(dllexport) void StpCallbackEntry(ULONG64 pService, ULONG32 probeId, MachineState& ctx, CallerInfo& callerinfo)
{
    // Ported from: https://github.com/mrexodia/TitanHide
    // Credits: Duncan Ogilvie (mrexodia), Matthijs Lavrijsen (Matti)
    switch ((PROBE_IDS)probeId) {
    case PROBE_IDS::IdQueryInformationProcess:
        NEW_SCOPE(
            auto processInfoClass = ctx.read_argument(1);
            auto pProcessInfo = ctx.read_argument(2);
            auto pProcessInfoLen = ctx.read_argument(4);

            g_Apis.pSetTlsData(processInfoClass, TLS_SLOTS::PROCESS_INFO_CLASS);
            g_Apis.pSetTlsData(pProcessInfo, TLS_SLOTS::PROCESS_INFO_DATA);
            g_Apis.pSetTlsData(pProcessInfoLen, TLS_SLOTS::PROCESS_INFO_DATA_LEN);
        );
        break;
    case PROBE_IDS::IdGetContextThread:
        NEW_SCOPE(
            auto pContextThreadData = ctx.read_argument(1);
            g_Apis.pSetTlsData(pContextThreadData, TLS_SLOTS::CONTEXT_THREAD_DATA);
        );
        break;
    case PROBE_IDS::IdQueryInformationThread:
        NEW_SCOPE(
            auto threadInfoClass = ctx.read_argument(1);
            auto pThreadInfo = ctx.read_argument(2);
            auto pThreadInfoLen = ctx.read_argument(3);

            g_Apis.pSetTlsData(threadInfoClass, TLS_SLOTS::THREAD_INFO_CLASS);
            g_Apis.pSetTlsData(pThreadInfo, TLS_SLOTS::THREAD_INFO_DATA);
            g_Apis.pSetTlsData(pThreadInfoLen, TLS_SLOTS::THREAD_INFO_DATA_LEN);
        );
        break;
    case PROBE_IDS::IdSetInformationThread:
        NEW_SCOPE(
            auto threadInfoClass = ctx.read_argument(1);

            switch (threadInfoClass) {
            case (uint64_t)THREADINFOCLASS::ThreadHideFromDebugger:
                // just do nothing, pretend the call happened ok
                ctx.redirect_syscall((uint64_t)&NoopNtSetInformationThread);
                break;
            }
        );
        break;
    case PROBE_IDS::IdClose:
        /*When under a debugger, NtClose generates an exception for usermode apps if an invalid OR pseudohandle is closed.
        We cannot cancel calls in the way inline hooks can, so we replace the handle with a valid one in these cases instead.*/
        NEW_SCOPE(
            HANDLE Handle = (HANDLE)ctx.read_argument(0);
            auto PreviousMode = ExGetPreviousMode();

            BOOLEAN AuditOnClose;
            NTSTATUS ObStatus = ObQueryObjectAuditingByHandle(Handle, &AuditOnClose);

            if (ObStatus != STATUS_INVALID_HANDLE) {
                // handle isn't invalid, check some additional properties
                BOOLEAN BeingDebugged = PsGetProcessDebugPort(PsGetCurrentProcess()) != nullptr;
                BOOLEAN GlobalFlgExceptions = RtlGetNtGlobalFlags() & FLG_ENABLE_CLOSE_EXCEPTIONS;
                OBJECT_HANDLE_INFORMATION HandleInfo = { 0 };
                if (BeingDebugged || GlobalFlgExceptions)
                {
                    // Get handle info so we can check if the handle has the ProtectFromClose bit set
                    PVOID Object = nullptr;
                    ObStatus = ObReferenceObjectByHandle(Handle,
                        0,
                        nullptr,
                        PreviousMode,
                        &Object,
                        &HandleInfo);

                    if (Object != nullptr) {
                        ObDereferenceObject(Object);
                    }
                }

                // If debugged, or handle not closeable, avoid exception and give noncloseable back
                if ((BeingDebugged || GlobalFlgExceptions) && NT_SUCCESS(ObStatus) && (HandleInfo.HandleAttributes & OBJ_PROTECT_CLOSE))
                {
                    ctx.redirect_syscall((uint64_t)&noop);
                    g_Apis.pSetTlsData(STATUS_HANDLE_NOT_CLOSABLE, CLOSE_RETVAL);
                    g_Apis.pSetTlsData(TRUE, CLOSE_OVERWRITE_RETVAL);
                } else {
                    // really close, it's ok won't raise
                    ctx.redirect_syscall((uint64_t)&noop);
                    g_Apis.pSetTlsData(ObCloseHandle(Handle, PreviousMode), CLOSE_RETVAL);
                    g_Apis.pSetTlsData(TRUE, CLOSE_OVERWRITE_RETVAL);
                }
            } else {
                ctx.redirect_syscall((uint64_t)&noop);
                g_Apis.pSetTlsData(STATUS_INVALID_HANDLE, CLOSE_RETVAL);
                g_Apis.pSetTlsData(TRUE, CLOSE_OVERWRITE_RETVAL);
            }
        );
        break;
    default:
        break;
    }
}
ASSERT_INTERFACE_IMPLEMENTED(StpCallbackEntry, tStpCallbackEntryPlugin, "StpCallbackEntry does not match the interface type");

/**
pService: Pointer to system service from SSDT
probeId: Identifier given in KeSetSystemServiceCallback for this syscall callback
paramCount: Number of arguments this system service uses, usually hard coded to 1
pArgs: Argument array, usually a single entry that holds return value
pArgSize: Length of argument array, usually hard coded to 1
pStackArgs: Pointer to stack area containing the rest of the arguments, if any
**/
extern "C" __declspec(dllexport) void StpCallbackReturn(ULONG64 pService, ULONG32 probeId, MachineState& ctx, CallerInfo & callerinfo) {
    switch ((PROBE_IDS)probeId) {
    case PROBE_IDS::IdQueryInformationProcess:
        // Internally, the kernel sets ProcessInfo first THEN sets ProcessInfoLength. We have to mirror this. So we bypass processInfo values first, then set length.
        // The anti-debug technique used sets both ProcessInfo and ProcessInfo length to be teh same pointer, so if you JUST bypass ProcessInfo then the Length value gets 
        // overwritten too since they're the same buffer. Fixing the Length value means, we have to write it too, which is why we bother backing it up.
        NEW_SCOPE(
            uint64_t processInfoClass = 0;
            uint64_t pProcessInfo = 0;
            uint64_t pProcessInfoLen = 0;

            if (g_Apis.pGetTlsData(processInfoClass, TLS_SLOTS::PROCESS_INFO_CLASS) && g_Apis.pGetTlsData(pProcessInfoLen, TLS_SLOTS::PROCESS_INFO_DATA_LEN) && g_Apis.pGetTlsData(pProcessInfo, TLS_SLOTS::PROCESS_INFO_DATA) && pProcessInfo) {
                // backup length (it can be null, in which case, don't read it)
                uint32_t origProcessInfoLen = 0;
                if (pProcessInfoLen) {
                    g_Apis.pTraceAccessMemory(&origProcessInfoLen, pProcessInfoLen, sizeof(origProcessInfoLen), 1, true);
                }

                switch (processInfoClass) {
                case (uint64_t)PROCESSINFOCLASS::ProcessDebugPort:
                    NEW_SCOPE(
                        DWORD64 newValue = 0;
                        g_Apis.pTraceAccessMemory(&newValue, pProcessInfo, sizeof(newValue), 1, false);
                    );
                    break;
                case (uint64_t)PROCESSINFOCLASS::ProcessDebugFlags:
                    NEW_SCOPE(
                        DWORD newValue = 1;
                        g_Apis.pTraceAccessMemory(&newValue, pProcessInfo, sizeof(newValue), 1, false);
                    );
                    break;
                case (uint64_t)PROCESSINFOCLASS::ProcessDebugObjectHandle:
                    if (ctx.read_return_value() == STATUS_SUCCESS) {
                        HANDLE newValue = 0;
                        g_Apis.pTraceAccessMemory(&newValue, pProcessInfo, sizeof(newValue), 1, false);
                        ctx.write_return_value(STATUS_PORT_NOT_SET);
                    }
                    break;
                }

                // reset length
                if (pProcessInfoLen) {
                    g_Apis.pTraceAccessMemory(&origProcessInfoLen, pProcessInfoLen, sizeof(origProcessInfoLen), 1, false);
                }
            }
        );
        break;
    case PROBE_IDS::IdQueryInformationThread:
        NEW_SCOPE(
            uint64_t threadInfoClass = 0;
            uint64_t pThreadInfo = 0;
            uint64_t pThreadInfoLen = 0;

            if (g_Apis.pGetTlsData(threadInfoClass, TLS_SLOTS::THREAD_INFO_CLASS) && g_Apis.pGetTlsData(pThreadInfoLen, TLS_SLOTS::THREAD_INFO_DATA_LEN) && g_Apis.pGetTlsData(pThreadInfo, TLS_SLOTS::THREAD_INFO_DATA) && pThreadInfo) {
                // backup length (it can be null, in which case, don't read it)
                uint32_t origThreadInfoLen = 0;
                if (pThreadInfoLen) {
                    g_Apis.pTraceAccessMemory(&origThreadInfoLen, pThreadInfoLen, sizeof(origThreadInfoLen), 1, true);
                }

                switch (threadInfoClass) {
                case (uint64_t)THREADINFOCLASS::ThreadWow64Context:
                    NEW_SCOPE(
                        uint64_t newValue = 0;
                        g_Apis.pTraceAccessMemory(&newValue, pThreadInfo + offsetof(WOW64_CONTEXT, Dr0), sizeof(newValue), 1, false);
                        g_Apis.pTraceAccessMemory(&newValue, pThreadInfo + offsetof(WOW64_CONTEXT, Dr1), sizeof(newValue), 1, false);
                        g_Apis.pTraceAccessMemory(&newValue, pThreadInfo + offsetof(WOW64_CONTEXT, Dr2), sizeof(newValue), 1, false);
                        g_Apis.pTraceAccessMemory(&newValue, pThreadInfo + offsetof(WOW64_CONTEXT, Dr3), sizeof(newValue), 1, false);
                        g_Apis.pTraceAccessMemory(&newValue, pThreadInfo + offsetof(WOW64_CONTEXT, Dr6), sizeof(newValue), 1, false);
                        g_Apis.pTraceAccessMemory(&newValue, pThreadInfo + offsetof(WOW64_CONTEXT, Dr7), sizeof(newValue), 1, false);
                    );
                    break;
                case (uint64_t)THREADINFOCLASS::ThreadHideFromDebugger:
                    NEW_SCOPE(
                        // Assume they expect YES back (i.e. someone bothers to check if their SetThreadInfo call worked).
                        BOOLEAN newValue = TRUE;
                        g_Apis.pTraceAccessMemory(&newValue, pThreadInfo, sizeof(newValue), 1, false);
                    );
                    break;
                }

                // reset length
                if (pThreadInfoLen) {
                    g_Apis.pTraceAccessMemory(&origThreadInfoLen, pThreadInfoLen, sizeof(origThreadInfoLen), 1, false);
                }
            }
        );
        break;
    case PROBE_IDS::IdGetContextThread:
        NEW_SCOPE(
            uint64_t pContextThreadData = {0};
            if (g_Apis.pGetTlsData(pContextThreadData, TLS_SLOTS::CONTEXT_THREAD_DATA)) {
                uint64_t newValue = 0;
                g_Apis.pTraceAccessMemory(&newValue, pContextThreadData + offsetof(CONTEXT, Dr0), sizeof(newValue), 1, false);
                g_Apis.pTraceAccessMemory(&newValue, pContextThreadData + offsetof(CONTEXT, Dr1), sizeof(newValue), 1, false);
                g_Apis.pTraceAccessMemory(&newValue, pContextThreadData + offsetof(CONTEXT, Dr2), sizeof(newValue), 1, false);
                g_Apis.pTraceAccessMemory(&newValue, pContextThreadData + offsetof(CONTEXT, Dr3), sizeof(newValue), 1, false);
                g_Apis.pTraceAccessMemory(&newValue, pContextThreadData + offsetof(CONTEXT, Dr6), sizeof(newValue), 1, false);
                g_Apis.pTraceAccessMemory(&newValue, pContextThreadData + offsetof(CONTEXT, Dr7), sizeof(newValue), 1, false);

                g_Apis.pTraceAccessMemory(&newValue, pContextThreadData + offsetof(CONTEXT, LastBranchToRip), sizeof(newValue), 1, false);
                g_Apis.pTraceAccessMemory(&newValue, pContextThreadData + offsetof(CONTEXT, LastBranchFromRip), sizeof(newValue), 1, false);
                g_Apis.pTraceAccessMemory(&newValue, pContextThreadData + offsetof(CONTEXT, LastExceptionToRip), sizeof(newValue), 1, false);
                g_Apis.pTraceAccessMemory(&newValue, pContextThreadData + offsetof(CONTEXT, LastExceptionFromRip), sizeof(newValue), 1, false);
            }
        );
        break;
    case PROBE_IDS::IdClose:
        NEW_SCOPE(
            uint64_t ovrwRetVal = { 0 };
            uint64_t newRetVal = { 0 };
            if (g_Apis.pGetTlsData(ovrwRetVal, TLS_SLOTS::CLOSE_OVERWRITE_RETVAL) && g_Apis.pGetTlsData(newRetVal, TLS_SLOTS::CLOSE_RETVAL) && ovrwRetVal) {
                ctx.write_return_value(newRetVal);
            }
        );
        break;
    default:
        break;
    }
}
ASSERT_INTERFACE_IMPLEMENTED(StpCallbackReturn, tStpCallbackReturnPlugin, "StpCallbackEntry does not match the interface type");

BOOL APIENTRY Main(HMODULE hModule, DWORD  reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}


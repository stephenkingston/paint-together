#!/bin/sh
# Paint (C port) - checks that a Windows .exe can load on the oldest Windows it is for
#
#   tools/check_imports.sh paint.exe xp|win7      (OBJDUMP=... to pick the objdump)
#
# xp: Windows XP SP3, 32-bit; win7: Windows 7 without updates (no UCRT).
# The .exe may import only from the system DLLs below, none of the functions
# Windows added later (a list of the usual suspects: anything newer must be
# found with GetProcAddress at run time, as host/win32_win.c does for DPI),
# and nothing by ordinal. The PE header must not ask for a newer Windows, and
# for XP the code must not use SSE (an XP-era CPU may not have it).
# Exits 1 and says why on any problem.

exe=$1
target=$2
case $target in
xp | win7) ;;
*)
    echo "usage: $0 EXE xp|win7" >&2
    exit 2
    ;;
esac
dump=$(${OBJDUMP:-objdump} -p "$exe") || exit 2
bad=0
fail()
{
    echo "$exe ($target): $*"
    bad=1
}

# The DLLs Paint may use: all of them are on XP. (Not the UCRT's api-ms-win-crt-*
# or ucrtbase, vcruntime, bcrypt, shcore, dwmapi, ...)
DLLS="kernel32 user32 gdi32 shell32 ole32 comdlg32 imm32 ws2_32 iphlpapi advapi32 msvcrt"

# Functions newer than Windows 7 (Windows 8 and 10)
AFTER_WIN7="GetSystemTimePreciseAsFileTime SetThreadDescription GetThreadDescription CreateFile2 GetCurrentThreadStackLimits
PrefetchVirtualMemory SetProcessMitigationPolicy GetProcessMitigationPolicy WaitOnAddress WakeByAddressSingle WakeByAddressAll
SetDefaultDllDirectories AddDllDirectory RemoveDllDirectory GetPackageFamilyName GetCurrentPackageId GetProcessInformation
SetProcessInformation CompareObjectHandles MapViewOfFile3 VirtualAlloc2 GetOverlappedResultEx CreateRemoteThreadEx
GetDpiForWindow GetDpiForSystem GetDpiForMonitor GetSystemMetricsForDpi AdjustWindowRectExForDpi SystemParametersInfoForDpi
SetProcessDpiAwarenessContext SetThreadDpiAwarenessContext GetThreadDpiAwarenessContext EnableNonClientDpiScaling
GetAwarenessFromDpiAwarenessContext AreDpiAwarenessContextsEqual SetProcessDpiAwareness GetProcessDpiAwareness
LogicalToPhysicalPointForPerMonitorDPI PhysicalToLogicalPointForPerMonitorDPI GetPointerInfo GetPointerType
GetPointerFrameInfo EnableMouseInPointer RegisterPointerInputTarget AddClipboardFormatListener
RemoveClipboardFormatListener GetClipboardSequenceNumber2 InetPtonW InetNtopW GetAddrInfoExW FreeAddrInfoExW
GetAddrInfoExCancel GetIfTable2Ex"

# Functions newer than Windows XP (Vista and 7), on top of the ones above
AFTER_XP="GetTickCount64 InitializeConditionVariable SleepConditionVariableCS SleepConditionVariableSRW
WakeConditionVariable WakeAllConditionVariable InitializeSRWLock AcquireSRWLockExclusive AcquireSRWLockShared
ReleaseSRWLockExclusive ReleaseSRWLockShared TryAcquireSRWLockExclusive TryAcquireSRWLockShared InitOnceExecuteOnce
InitOnceInitialize InitOnceBeginInitialize InitOnceComplete CreateEventExW CreateEventExA CreateMutexExW
CreateSemaphoreExW CreateWaitableTimerExW InitializeCriticalSectionEx GetFinalPathNameByHandleW
GetFileInformationByHandleEx SetFileInformationByHandle CancelIoEx CancelSynchronousIo CreateSymbolicLinkW
GetLocaleInfoEx CompareStringEx CompareStringOrdinal LCMapStringEx GetUserDefaultLocaleName LocaleNameToLCID
LCIDToLocaleName FlsAlloc FlsFree FlsGetValue FlsSetValue GetErrorMode QueryFullProcessImageNameW GetThreadId
GetProductInfo GetNamedPipeServerProcessId SetFileCompletionNotificationModes GetQueuedCompletionStatusEx
CreateThreadpoolWork SubmitThreadpoolWork CreateThreadpoolTimer SetThreadErrorMode GetActiveProcessorCount
GetLogicalProcessorInformationEx SetThreadGroupAffinity WerRegisterRuntimeExceptionModule
inet_pton inet_ntop WSAPoll WSASendMsg WSAConnectByNameW if_nametoindex if_indextoname
ConvertInterfaceIndexToLuid ConvertInterfaceLuidToIndex GetIfTable2 GetIfEntry2 GetIpInterfaceEntry
GetUnicastIpAddressTable NotifyIpInterfaceChange NotifyUnicastIpAddressChange CancelMibChangeNotify2 FreeMibTable
SetProcessDPIAware IsProcessDPIAware ChangeWindowMessageFilter ChangeWindowMessageFilterEx RegisterTouchWindow
GetTouchInputInfo CloseTouchInputHandle SetGestureConfig GetGestureInfo ShutdownBlockReasonCreate
ShutdownBlockReasonDestroy CalculatePopupWindowPosition GetDisplayConfigBufferSizes QueryDisplayConfig
RegisterPowerSettingNotification IsTouchWindow SetWindowDisplayAffinity
SHGetKnownFolderPath SHCreateItemFromParsingName SHGetStockIconInfo SetCurrentProcessExplicitAppUserModelID
SHGetPropertyStoreForWindow SHOpenFolderAndSelectItems
_fseeki64 _ftelli64 _get_errno _set_errno _get_doserrno _set_doserrno _get_fmode _set_fmode _get_pgmptr _get_wpgmptr
_set_invalid_parameter_handler _get_invalid_parameter_handler _set_abort_behavior _set_output_format
_get_output_format _set_purecall_handler _get_purecall_handler _create_locale _free_locale __acrt_iob_func"

deny=$AFTER_WIN7
[ "$target" = xp ] && deny="$deny $AFTER_XP"

# "dll:function" per import ("dll:#n" by ordinal)
imports=$(printf '%s\n' "$dump" | awk '
    /DLL Name:/ { dll = tolower($3); sub(/\.dll$/, "", dll); next }
    dll && /^\t[0-9a-f]+ / { print dll ":" ($2 == "<none>" ? $4 : "#" $2) }
    /^$/ { dll = "" }')
[ -n "$imports" ] || fail "no imports found (not a PE file?)"

for dll in $(printf '%s\n' "$imports" | cut -d: -f1 | sort -u); do
    case " $DLLS " in
    *" $dll "*) ;;
    *) fail "imports from $dll.dll, which isn't one of Paint's system DLLs" ;;
    esac
done
for imp in $imports; do
    fn=${imp#*:}
    case $fn in
    "#"*) fail "imports $imp by ordinal" ;;
    esac
    case " $(echo $deny) " in
    *" $fn "*) fail "imports $imp, newer than $target (load it with GetProcAddress)" ;;
    esac
    if [ "$target" = xp ] && [ "${imp%%:*}" = msvcrt ]; then
        case $fn in
        *_s | *_l) fail "imports $imp: XP's msvcrt.dll has no secure (_s) or locale (_l) functions" ;;
        esac
    fi
done

# The PE header: the Windows versions it asks for (XP is 5.1; Windows 7 is 6.1)
max=$([ "$target" = xp ] && echo 501 || echo 601)
for field in OSystem Subsystem; do
    major=$(printf '%s\n' "$dump" | awk -v f="Major${field}Version" '$1 == f { print $2 }')
    minor=$(printf '%s\n' "$dump" | awk -v f="Minor${field}Version" '$1 == f { print $2 }')
    [ $((major * 100 + minor)) -le $max ] || fail "the PE header asks for Windows $major.$minor ($field version)"
done
if [ "$target" = xp ]; then
    printf '%s\n' "$dump" | grep -q 'pei-i386' || fail "not a 32-bit x86 executable"
    sse=$(${OBJDUMP:-objdump} -d "$exe" | grep -c '%xmm')
    [ "$sse" -eq 0 ] || fail "$sse instructions use SSE registers"
fi

[ $bad -eq 0 ] && echo "$exe: loads on $target ($(printf '%s\n' "$imports" | wc -l | tr -d ' ') imports checked)"
exit $bad

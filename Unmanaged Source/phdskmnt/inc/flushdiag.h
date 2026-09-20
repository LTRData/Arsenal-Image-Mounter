// M2 observation only. No waits, provider calls, or I/O status/ordering changes.
// Shared by phdskmnt and deviodrv; this is not a wire/API structure.
#pragma once

#define AIM_DIAG_TAG 'dFmA'
#define AIM_DIAG_HISTORY 16
#define AIM_DIAG_KINDS 4

// Keep the existing pre-XP/NT4 build surface from importing DbgPrintEx.
#if defined(NT4_COMPATIBLE) || (defined(_NT_TARGET_VERSION) && _NT_TARGET_VERSION < 0x0501)
#define AIM_DIAG_LOG(...) DbgPrint(__VA_ARGS__)
#else
#define AIM_DIAG_LOG(...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, __VA_ARGS__)
#endif

// Defined once in each driver. Use driver/load, Id and Created100ns together
// for incarnation identity. Print can be disabled through the debugger.
extern volatile LONG AimDiagNextId;
extern volatile LONG AimDiagPrint;

inline ULONGLONG AimDiagTime()
{
#ifdef NT4_COMPATIBLE
    LARGE_INTEGER ticks;
    KeQueryTickCount(&ticks);
    return ticks.QuadPart * (ULONGLONG)KeQueryTimeIncrement();
#else
    return KeQueryInterruptTime();
#endif
}

enum AIM_DIAG_KIND { AimDiagWrite10, AimDiagWrite16, AimDiagUnmap, AimDiagZero };
enum AIM_DIAG_ROUTE { AimDiagUnknown, AimDiagVm, AimDiagQueuedFile,
    AimDiagParallelFile, AimDiagAwe, AimDiagStreamProxy, AimDiagShmProxy,
    AimDiagDevio };

struct AIM_DIAG_CONTEXT;
struct AIM_DIAG_TOKEN
{
    LIST_ENTRY Link;
    AIM_DIAG_CONTEXT* Owner;
    ULONGLONG Accepted100ns;
    ULONGLONG Number;
    ULONG Kind;
    ULONG Phase;
    BOOLEAN ZeroDerived;
};

struct AIM_DIAG_SNAPSHOT
{
    ULONGLONG Event;
    ULONGLONG Time100ns;
    ULONGLONG Oldest100ns;
    ULONGLONG AcceptedTotal;
    ULONGLONG CompletedTotal;
    ULONGLONG TypeTotal[AIM_DIAG_KINDS];
    ULONGLONG Sync10;
    ULONGLONG Sync16;
    ULONGLONG Flush;
    ULONGLONG WithPending;
    ULONGLONG ZeroAttempts;
    ULONGLONG ZeroFallbacks;
    ULONGLONG RawUnmapCount;
    ULONGLONG RawUnmapErrors;
    NTSTATUS LastUnmapStatus;
    NTSTATUS LastUnmapError;
    ULONG Accepted;
    ULONG Phase[3];
    ULONG HighAccepted;
    ULONG HighAfterPhase0;
    ULONG ZeroPending;
    ULONG HighPhase[3];
    ULONG PeakAtSync;
    ULONG TypePending[AIM_DIAG_KINDS];
    ULONG Route;
    ULONG MountFlags;
    ULONG SrbFlags;
    UCHAR Opcode;
    UCHAR QueueAction;
    UCHAR Cdb[16];
};

struct AIM_DIAG_RAW_STATUS
{
    ULONGLONG Number;
    ULONGLONG Time100ns;
    ULONGLONG Request;
    NTSTATUS Status;
};

struct AIM_DIAG_CONTEXT
{
    volatile LONG References;
    KSPIN_LOCK Lock;
    LIST_ENTRY Pending;
    ULONG Id;
    ULONG DeviceNumber;
    ULONG_PTR Adapter;
    ULONGLONG Created100ns;
    AIM_DIAG_SNAPSHOT Now;
    AIM_DIAG_SNAPSHOT History[AIM_DIAG_HISTORY];
    AIM_DIAG_RAW_STATUS UnmapHistory[AIM_DIAG_HISTORY];
};

inline void AimDiagReference(AIM_DIAG_CONTEXT* context)
{
    if (context != NULL)
        InterlockedIncrement(&context->References);
}

inline void AimDiagRelease(AIM_DIAG_CONTEXT* context)
{
    if (context != NULL && InterlockedDecrement(&context->References) == 0)
        ExFreePoolWithTag(context, AIM_DIAG_TAG);
}

inline AIM_DIAG_CONTEXT* AimDiagCreate(ULONG_PTR adapter, ULONG device, ULONG route)
{
    AIM_DIAG_CONTEXT* context = (AIM_DIAG_CONTEXT*)ExAllocatePoolWithTag(
        NonPagedPool, sizeof(AIM_DIAG_CONTEXT), AIM_DIAG_TAG);
    if (context != NULL)
    {
        RtlZeroMemory(context, sizeof(*context));
        context->References = 1; // Published LU/file-context owner.
        context->Id = (ULONG)InterlockedIncrement(&AimDiagNextId);
        context->Adapter = adapter;
        context->DeviceNumber = device;
        context->Created100ns = AimDiagTime();
        context->Now.Route = route;
        KeInitializeSpinLock(&context->Lock);
        InitializeListHead(&context->Pending);
    }
    return context; // Allocation failure never fails an I/O or mount.
}

inline void AimDiagSetRoute(AIM_DIAG_CONTEXT* context, ULONG route, ULONG flags)
{
    if (context == NULL) return;
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    context->Now.Route = route;
    context->Now.MountFlags = flags;
    KeReleaseSpinLock(&context->Lock, irql);
}

// Caller holds a lookup/owner reference. Tokens are zero initialized, and only
// the existing request owner changes them. The list is diagnostic only.
inline void AimDiagAccept(AIM_DIAG_TOKEN* token, AIM_DIAG_CONTEXT* context, ULONG kind)
{
    if (context == NULL) return;
    AimDiagReference(context);
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    token->Owner = context;
    token->Kind = kind;
    token->Phase = 0;
    token->Accepted100ns = AimDiagTime();
    token->Number = ++context->Now.AcceptedTotal;
    InsertTailList(&context->Pending, &token->Link);
    ++context->Now.Accepted;
    ++context->Now.Phase[0];
    ++context->Now.TypePending[kind];
    ++context->Now.TypeTotal[kind];
    if (context->Now.Accepted > context->Now.HighAccepted)
        context->Now.HighAccepted = context->Now.Accepted;
    if (context->Now.Phase[0] > context->Now.HighPhase[0])
        context->Now.HighPhase[0] = context->Now.Phase[0];
    KeReleaseSpinLock(&context->Lock, irql);
}

// phdskmnt: 0 backing pending (including setup/queue), 1 backing finished,
// 2 inside final port notification. deviodrv: 0 received, 1 submitting/sent,
// 2 reply processing. The current accepted count equals the sum of phases.
inline void AimDiagPhase(AIM_DIAG_TOKEN* token, ULONG phase)
{
    AIM_DIAG_CONTEXT* context = token->Owner;
    if (context == NULL || token->Phase == phase) return;
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    --context->Now.Phase[token->Phase];
    ++context->Now.Phase[phase];
    token->Phase = phase;
    if (context->Now.Phase[phase] > context->Now.HighPhase[phase])
        context->Now.HighPhase[phase] = context->Now.Phase[phase];
    ULONG after = context->Now.Phase[1] + context->Now.Phase[2];
    if (after > context->Now.HighAfterPhase0) context->Now.HighAfterPhase0 = after;
    KeReleaseSpinLock(&context->Lock, irql);
}

// Only for direct setup failure, when no callback was installed/submitted.
// Transfer list membership and its reference to StartIo's stack token without
// creating a false completed interval or changing oldest-request order.
inline void AimDiagMove(AIM_DIAG_TOKEN* to, AIM_DIAG_TOKEN* from)
{
    AIM_DIAG_CONTEXT* context = from->Owner;
    if (context == NULL) return;
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    *to = *from;
    to->Link.Blink->Flink = &to->Link;
    to->Link.Flink->Blink = &to->Link;
    from->Owner = NULL;
    KeReleaseSpinLock(&context->Lock, irql);
}

// Call after notification returns, before freeing its work item. Never read an
// SRB, IRP or LU here: they may already have been returned/freed by that call.
inline void AimDiagComplete(AIM_DIAG_TOKEN* token)
{
    AIM_DIAG_CONTEXT* context = token->Owner;
    if (context == NULL) return;
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    RemoveEntryList(&token->Link);
    --context->Now.Accepted;
    --context->Now.Phase[token->Phase];
    --context->Now.TypePending[token->Kind];
    if (token->ZeroDerived) --context->Now.ZeroPending;
    ++context->Now.CompletedTotal;
    token->Owner = NULL;
    KeReleaseSpinLock(&context->Lock, irql);
    AimDiagRelease(context);
}

inline void AimDiagMarkZero(AIM_DIAG_TOKEN* token)
{
    AIM_DIAG_CONTEXT* context = token->Owner;
    if (context == NULL) return;
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    ++context->Now.ZeroAttempts;
    ++context->Now.ZeroPending;
    token->ZeroDerived = TRUE;
    KeReleaseSpinLock(&context->Lock, irql);
}

inline void AimDiagZeroResult(AIM_DIAG_TOKEN* token, NTSTATUS status)
{
    AIM_DIAG_CONTEXT* context = token->Owner;
    if (context == NULL) return;
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    if (!NT_SUCCESS(status)) ++context->Now.ZeroFallbacks;
    KeReleaseSpinLock(&context->Lock, irql);
}

inline void AimDiagUnmapResult(AIM_DIAG_TOKEN* token, NTSTATUS status)
{
    AIM_DIAG_CONTEXT* context = token->Owner;
    if (context == NULL) return;
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    ULONGLONG number = ++context->Now.RawUnmapCount;
    context->Now.LastUnmapStatus = status;
    if (!NT_SUCCESS(status))
    {
        ++context->Now.RawUnmapErrors;
        context->Now.LastUnmapError = status;
    }
    AIM_DIAG_RAW_STATUS* entry = &context->UnmapHistory[(number - 1) % AIM_DIAG_HISTORY];
    entry->Number = number;
    entry->Time100ns = AimDiagTime();
    entry->Request = token->Number;
    entry->Status = status;
    KeReleaseSpinLock(&context->Lock, irql);
}

// All fields in a snapshot share one lock acquisition. The 16-entry ring can
// overwrite older snapshots; Event is its monotonic sequence (no silent loss).
inline AIM_DIAG_SNAPSHOT AimDiagCapture(AIM_DIAG_CONTEXT* context,
    UCHAR opcode, UCHAR queueAction, ULONG flags, const UCHAR* cdb)
{
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    AIM_DIAG_SNAPSHOT* now = &context->Now;
    ++now->Event;
    now->Time100ns = AimDiagTime();
    now->Oldest100ns = 0;
    if (!IsListEmpty(&context->Pending))
    {
        AIM_DIAG_TOKEN* oldest = CONTAINING_RECORD(context->Pending.Flink, AIM_DIAG_TOKEN, Link);
        now->Oldest100ns = now->Time100ns - oldest->Accepted100ns;
    }
    now->Opcode = opcode;
    now->QueueAction = queueAction;
    now->SrbFlags = flags;
    RtlZeroMemory(now->Cdb, sizeof(now->Cdb));
    if (cdb != NULL) RtlCopyMemory(now->Cdb, cdb, sizeof(now->Cdb));
    if (opcode == 0x35) ++now->Sync10;
    else if (opcode == 0x91) ++now->Sync16;
    else if (opcode == IRP_MJ_FLUSH_BUFFERS) ++now->Flush;
    if (opcode == 0x35 || opcode == 0x91 || opcode == IRP_MJ_FLUSH_BUFFERS)
    {
        if (now->Accepted != 0) ++now->WithPending;
        if (now->Accepted > now->PeakAtSync) now->PeakAtSync = now->Accepted;
    }
    AIM_DIAG_SNAPSHOT result = *now;
    context->History[(now->Event - 1) % AIM_DIAG_HISTORY] = result;
    KeReleaseSpinLock(&context->Lock, irql);
    return result;
}

// Only mount/teardown/flush paths print. Each line is <512 bytes and carries
// source, incarnation and event IDs for interleaved multi-CPU output. No lock
// is held here; a caller reference keeps the diagnostic context alive.
// Do not inline the spinlock-taking capture into filetable.cpp's PAGE section.
__declspec(noinline) inline void AimDiagReport(AIM_DIAG_CONTEXT* context, const char* source,
    UCHAR opcode, UCHAR queueAction, ULONG flags, const UCHAR* cdb)
{
    if (context == NULL)
    {
        if (AimDiagPrint)
            AIM_DIAG_LOG(
                "AIMDIAG1 %s unavailable op=%02x (no counters; inconclusive)\n", source, opcode);
        return;
    }
    AIM_DIAG_SNAPSHOT s = AimDiagCapture(context, opcode, queueAction, flags, cdb);
    if (!AimDiagPrint) return;
    AIM_DIAG_LOG(
        "AIMDIAG1 %s id=%lu e=%I64u ctx=%p adapter=%p lu=%08lx born=%I64u t=%I64u route=%lu mount=%08lx op=%02x immed=%u qa=%02x srb=%08lx cdb=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
        source, context->Id, s.Event, context, (PVOID)context->Adapter, context->DeviceNumber,
        context->Created100ns, s.Time100ns, s.Route, s.MountFlags, opcode,
        (s.Cdb[1] >> 1) & 1, queueAction, flags,
        s.Cdb[0], s.Cdb[1], s.Cdb[2], s.Cdb[3], s.Cdb[4], s.Cdb[5], s.Cdb[6], s.Cdb[7],
        s.Cdb[8], s.Cdb[9], s.Cdb[10], s.Cdb[11], s.Cdb[12], s.Cdb[13], s.Cdb[14], s.Cdb[15]);
    AIM_DIAG_LOG(
        "AIMDIAG1 %s id=%lu e=%I64u accepted=%lu p0=%lu p1=%lu p2=%lu oldest100ns=%I64u high=%lu,%lu,%lu,%lu highAfter0=%lu peakSync=%lu total=%I64u completed=%I64u\n",
        source, context->Id, s.Event, s.Accepted, s.Phase[0], s.Phase[1], s.Phase[2],
        s.Oldest100ns, s.HighAccepted, s.HighPhase[0], s.HighPhase[1], s.HighPhase[2],
        s.HighAfterPhase0, s.PeakAtSync, s.AcceptedTotal, s.CompletedTotal);
    AIM_DIAG_LOG(
        "AIMDIAG1 %s id=%lu e=%I64u typePending=%lu,%lu,%lu,%lu typeTotal=%I64u,%I64u,%I64u,%I64u sync=%I64u,%I64u flush=%I64u busySync=%I64u\n",
        source, context->Id, s.Event, s.TypePending[0], s.TypePending[1], s.TypePending[2], s.TypePending[3],
        s.TypeTotal[0], s.TypeTotal[1], s.TypeTotal[2], s.TypeTotal[3], s.Sync10, s.Sync16,
        s.Flush, s.WithPending);
    AIM_DIAG_LOG(
        "AIMDIAG1 %s id=%lu e=%I64u zero=%I64u zeroPending=%lu fallback=%I64u unmapRaw=%I64u errors=%I64u last=%08lx lastError=%08lx\n",
        source, context->Id, s.Event, s.ZeroAttempts, s.ZeroPending, s.ZeroFallbacks, s.RawUnmapCount,
        s.RawUnmapErrors, s.LastUnmapStatus, s.LastUnmapError);
}

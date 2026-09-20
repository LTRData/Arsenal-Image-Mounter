// Host-side accounting/ownership test of the actual M2 helper. The shim is not
// a WDK build or a simulation of Windows port/IRP lifetime guarantees.
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

using LONG = int32_t;
using ULONG = uint32_t;
using ULONG_PTR = uintptr_t;
using ULONGLONG = uint64_t;
using UCHAR = unsigned char;
using BOOLEAN = unsigned char;
using NTSTATUS = int32_t;
using KIRQL = unsigned char;
using KSPIN_LOCK = std::mutex;
using PVOID = void*;
struct LIST_ENTRY { LIST_ENTRY* Flink; LIST_ENTRY* Blink; };
constexpr bool TRUE = true;
constexpr int NonPagedPool = 0;
constexpr int DPFLTR_IHVDRIVER_ID = 0;
constexpr int DPFLTR_INFO_LEVEL = 0;
constexpr int IRP_MJ_FLUSH_BUFFERS = 9;
#define __declspec(x) __attribute__((x))
#define NT_SUCCESS(s) ((s) >= 0)
#define RtlZeroMemory(p,n) std::memset(p,0,n)
#define RtlCopyMemory(p,q,n) std::memcpy(p,q,n)
#define CONTAINING_RECORD(p,t,f) reinterpret_cast<t*>(reinterpret_cast<char*>(p)-offsetof(t,f))
static std::atomic<ULONGLONG> clock100ns{100};
static std::atomic<int> allocations{0};
static bool failAllocation;
static thread_local int locksHeld;
static ULONGLONG KeQueryInterruptTime() { return clock100ns.fetch_add(100); }
static LONG InterlockedIncrement(volatile LONG* p) { return __atomic_add_fetch(p,1,__ATOMIC_SEQ_CST); }
static LONG InterlockedDecrement(volatile LONG* p) { return __atomic_sub_fetch(p,1,__ATOMIC_SEQ_CST); }
static void KeInitializeSpinLock(KSPIN_LOCK* p) { new (p) std::mutex(); }
static void KeAcquireSpinLock(KSPIN_LOCK* p, KIRQL* irql) { *irql=0; p->lock(); ++locksHeld; }
static void KeReleaseSpinLock(KSPIN_LOCK* p, KIRQL) { --locksHeld; p->unlock(); }
static void InitializeListHead(LIST_ENTRY* p) { p->Flink=p->Blink=p; }
static bool IsListEmpty(LIST_ENTRY* p) { return p->Flink==p; }
static void InsertTailList(LIST_ENTRY* head, LIST_ENTRY* p)
{
    p->Flink=head; p->Blink=head->Blink; head->Blink->Flink=p; head->Blink=p;
}
static void RemoveEntryList(LIST_ENTRY* p)
{
    p->Flink->Blink=p->Blink; p->Blink->Flink=p->Flink;
}
static void* ExAllocatePoolWithTag(int, size_t size, unsigned)
{
    if (failAllocation) return nullptr;
    ++allocations;
    return std::malloc(size);
}
static void ExFreePoolWithTag(void*, unsigned);
static void DbgPrintEx(int, int, const char*, ...) { assert(locksHeld==0); }
#include "../../Unmanaged Source/phdskmnt/inc/flushdiag.h"
volatile LONG AimDiagNextId = 0;
volatile LONG AimDiagPrint = 1;
static void ExFreePoolWithTag(void* p, unsigned)
{
    assert(locksHeld==0);
    auto* context = static_cast<AIM_DIAG_CONTEXT*>(p);
    assert(context->Now.Accepted==0);
    assert(IsListEmpty(&context->Pending));
    context->Lock.~mutex();
    --allocations;
    std::free(p);
}
static AIM_DIAG_SNAPSHOT snapshot(AIM_DIAG_CONTEXT* c, UCHAR op=0x35)
{
    UCHAR cdb[16] = {op, 2};
    auto s=AimDiagCapture(c,op,0x22,0x1234,cdb);
    assert(s.Accepted==s.Phase[0]+s.Phase[1]+s.Phase[2]);
    assert(s.AcceptedTotal-s.CompletedTotal==s.Accepted);
    assert(s.Accepted==s.TypePending[0]+s.TypePending[1]+s.TypePending[2]+s.TypePending[3]);
    assert(s.Cdb[1]==2 && s.QueueAction==0x22 && s.SrbFlags==0x1234);
    return s;
}

static void boundaries_and_inline()
{
    auto* c=AimDiagCreate(11,22,AimDiagParallelFile);
    AimDiagReference(c); // ScsiExecute's protected lookup reference.
    assert(snapshot(c).Accepted==0);
    AIM_DIAG_TOKEN t={};
    AimDiagAccept(&t,c,AimDiagWrite10); // Before a possible inline IoCallDriver callback.
    assert(snapshot(c).Phase[0]==1);
    auto callback=[&] {
        AimDiagPhase(&t,1);
        assert(snapshot(c).Phase[1]==1);
        AimDiagPhase(&t,2);
        assert(snapshot(c).Phase[2]==1); // Reentrant observation during notification.
        AimDiagRelease(c); // LU owner can disappear during the upper completion.
        AimDiagComplete(&t);
    };
    callback();
    assert(snapshot(c).Accepted==0);
    assert(c->References==1); // Lookup still valid even though token and LU are gone.
    AimDiagRelease(c);
    assert(allocations==0);
}

static void mixed_out_of_order_and_failure_move()
{
    auto* c=AimDiagCreate(1,2,AimDiagQueuedFile);
    AIM_DIAG_TOKEN a={},b={},u={},immediate={};
    AimDiagAccept(&a,c,AimDiagWrite10);
    AimDiagAccept(&b,c,AimDiagWrite16);
    AimDiagAccept(&u,c,AimDiagUnmap);
    const auto originalTime=a.Accepted100ns;
    AimDiagMarkZero(&b);
    AimDiagZeroResult(&b,-1); // Falling back does not complete/reaccept the write.
    assert(snapshot(c).Accepted==3 && c->Now.ZeroPending==1);
    AimDiagPhase(&b,1);
    AimDiagUnmapResult(&u,-2);
    AimDiagUnmapResult(&u,0); // Keep the earlier hidden error separately.
    AimDiagPhase(&u,1);
    auto s=snapshot(c,0x91);
    assert(s.Phase[0]==1 && s.Phase[1]==2 && s.HighAfterPhase0==2);
    assert(s.RawUnmapErrors==1 && s.LastUnmapError==-2 && s.LastUnmapStatus==0);
    assert(s.Oldest100ns==s.Time100ns-originalTime);
    AimDiagPhase(&a,1); // Direct setup failure: existing work item must be freed.
    AimDiagMove(&immediate,&a);
    assert(a.Owner==nullptr && immediate.Number==1);
    s=snapshot(c);
    assert(s.Oldest100ns==s.Time100ns-originalTime);
    AimDiagComplete(&u);
    AimDiagComplete(&b);
    assert(snapshot(c).Accepted==1 && c->Now.ZeroPending==0);
    AimDiagPhase(&immediate,2);
    AimDiagComplete(&immediate);
    s=snapshot(c);
    assert(s.CompletedTotal==3 && s.Accepted==0 && s.ZeroAttempts==1 && s.ZeroFallbacks==1);
    AimDiagRelease(c);
}

static void tokens_outlive_owner()
{
    auto* c=AimDiagCreate(1,2,AimDiagParallelFile);
    AIM_DIAG_TOKEN first={},middle={},last={},moved={};
    AimDiagAccept(&first,c,AimDiagWrite10);
    AimDiagAccept(&middle,c,AimDiagWrite16);
    AimDiagAccept(&last,c,AimDiagUnmap);
    AimDiagMove(&moved,&middle); // Preserve intrusive links for a non-head token.
    AimDiagRelease(c); // No LU/file owner and no temporary lookup reference.
    assert(c->References==3);
    AimDiagComplete(&last);
    AimDiagComplete(&first);
    auto s=snapshot(c); // The remaining token still owns the context.
    assert(s.Accepted==1 && s.Oldest100ns==s.Time100ns-moved.Accepted100ns);
    AimDiagPhase(&moved,1);
    AimDiagPhase(&moved,1); // Worker's fallback boundary must be idempotent.
    AimDiagComplete(&moved); // Last token frees the context, not the old LU.
    assert(allocations==0);
}

static void devio_requeue_cancel_and_rings()
{
    auto* c=AimDiagCreate(1,0,AimDiagDevio);
    AIM_DIAG_TOKEN w={},z={},u={};
    AimDiagAccept(&w,c,AimDiagWrite10);
    AimDiagAccept(&z,c,AimDiagZero);
    AimDiagAccept(&u,c,AimDiagUnmap);
    AimDiagPhase(&w,1); // Off-list, including before publication to sent list.
    AimDiagPhase(&w,0); // Too-small exchange buffer; no new token/reference.
    assert(snapshot(c,9).Phase[0]==3);
    AimDiagPhase(&w,1);
    AimDiagPhase(&z,1);
    AimDiagPhase(&z,2); // Reply for second issued request arrives first.
    auto s=snapshot(c,9);
    assert(s.Phase[0]==1 && s.Phase[1]==1 && s.Phase[2]==1);
    AimDiagComplete(&u); // Disconnect before dispatch.
    AimDiagComplete(&w); // Disconnect after dispatch, without a provider reply.
    AimDiagComplete(&z);
    for (int i=0;i<40;++i) snapshot(c,9);
    s=snapshot(c,9);
    assert(s.CompletedTotal==3 && s.Accepted==0);
    assert(c->History[(s.Event-1)%AIM_DIAG_HISTORY].Event==s.Event);
    assert(c->History[s.Event%AIM_DIAG_HISTORY].Event==s.Event-AIM_DIAG_HISTORY+1);
    AimDiagReport(c,"test",9,0,0,nullptr); // Verify report never prints under a lock.
    AimDiagRelease(c);
}

static void concurrent_snapshots()
{
    auto* c=AimDiagCreate(1,2,AimDiagParallelFile);
    std::atomic<int> running{4};
    std::vector<std::thread> threads;
    for (unsigned k=0;k<4;++k) threads.emplace_back([&,k] {
        for (unsigned i=0;i<25000;++i)
        {
            AIM_DIAG_TOKEN t={};
            AimDiagAccept(&t,c,k);
            AimDiagPhase(&t,1);
            AimDiagPhase(&t,2);
            AimDiagComplete(&t);
        }
        --running;
    });
    while (running) snapshot(c);
    for (auto& t:threads) t.join();
    auto s=snapshot(c);
    assert(s.AcceptedTotal==100000 && s.CompletedTotal==100000 && s.Accepted==0);
    assert(s.HighAccepted<=4 && c->References==1);
    AimDiagRelease(c);
}

int main()
{
    failAllocation=true;
    assert(AimDiagCreate(0,0,0)==nullptr);
    AIM_DIAG_TOKEN empty={};
    AimDiagAccept(&empty,nullptr,AimDiagWrite10);
    AimDiagPhase(&empty,1);
    AimDiagComplete(&empty);
    AimDiagReport(nullptr,"test",0x35,0,0,nullptr);
    AimDiagRelease(nullptr);
    failAllocation=false;
    boundaries_and_inline();
    mixed_out_of_order_and_failure_move();
    tokens_outlive_owner();
    devio_requeue_cancel_and_rings();
    concurrent_snapshots();
    assert(allocations==0);
    std::printf("PASS: boundary, inline, move, mixed/zero, requeue/cancel, ring, allocation-failure and 100000-request concurrent tests; context=%zu token=%zu bytes (host shim)\n",sizeof(AIM_DIAG_CONTEXT),sizeof(AIM_DIAG_TOKEN));
}

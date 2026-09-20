# M2: flush boundary diagnostics

This is an observation build for the M1 synchronization investigation. It does
not implement a barrier. The owner test gate remains between M2 and M3.

Baseline: `ArsenalRecon/Arsenal-Image-Mounter`, origin
`https://github.com/ArsenalRecon/Arsenal-Image-Mounter.git`, clean `master` at
`642f08bd3452df09c119c1fb18e45b18f77ad6e5`. Work branch:
`diagnostic/flush-boundaries-m2`. Record the tested commit with `git rev-parse HEAD`.

If using the delivered mail patch, apply it in a clean checkout at this baseline.
`--keep-cr` preserves the existing mixed LF/CRLF source files:

```sh
git switch -c diagnostic/flush-boundaries-m2 642f08bd3452df09c119c1fb18e45b18f77ad6e5
git am --keep-cr /path/to/AIM-M2-diagnostics.patch
```

## Scope and implementation

The changes add one nonpaged statistics context per data LU / deviodrv file
context, and an embedded token in each existing work/request allocation. The
shared implementation is `Unmanaged Source/phdskmnt/inc/flushdiag.h`. It is an
internal header, not an exported interface or a DevIO wire structure.

The existing admission queues, submissions, provider calls, completion calls,
returned status, cancellation and timeout decisions are unchanged. In particular,
SYNCHRONIZE CACHE(10/16), IMMED, `SRB_FUNCTION_FLUSH`, deviodrv
`IRP_MJ_FLUSH_BUFFERS`, cache/FUA advertisement and UNMAP error suppression retain
their baseline behavior. No VhdAccess changes or protocol additions are included.

| Files | Diagnostic change |
| --- | --- |
| `phdskmnt/inc/phdskmnt.h`, `phdskmnt.cpp` | Context/token declarations; stack token for direct setup failure; retirement at existing final completion sites |
| `phdskmnt/scsi.cpp` | Reference statistics during protected LU lookup; accept WRITE10/16 and UNMAP tokens; snapshot real data-LU SYNCHRONIZE CACHE commands |
| `phdskmnt/workerthread.cpp`, `iodisp.cpp` | Backing completion observations, zero attempt/fallback, raw lower UNMAP status; context publication/detachment and route |
| `deviodrv/deviodrv.h`, `deviodrv.cpp`, `filetable.cpp` | File-context statistics ownership and request tokens |
| `deviodrv/irpfwd.cpp`, `dispatch.cpp` | Received/submitting/reply/completed boundaries; native flush snapshot |
| `tests/flush-diagnostics/accounting.cpp` | Host tests of the actual accounting helper with kernel primitive shims |

Paths in the table are relative to `Unmanaged Source`, except the test.
Existing line endings and recent parallel-I/O fixes are preserved.

## Exact measured boundaries

For phdskmnt, **accepted** means a validated modifying SRB has a successfully
allocated work item and has been registered before work publication or lower
setup/submission. Rejected commands and failures to allocate that work item
cannot modify backing and are not admitted. Concurrent acceptance and flush
observations are linearized by the diagnostic lock; this is not a timestamp of
arrival higher in the Windows stack.

| phdskmnt field | Meaning at the snapshot |
| --- | --- |
| `accepted` | Accepted modifying tokens whose final `RequestComplete` wrapper has not yet retired them |
| `p0` | Backing phase unfinished: includes lower setup, the LU worker queue and backing I/O |
| `p1` | Backing phase finished, before entering the final port-notification wrapper |
| `p2` | Entered the wrapper immediately before `RequestComplete`; not yet retired just after that call returns |
| `p1 + p2` | Backing completed, final notification return not yet observed |

For direct `FILE_OBJECT` writes, p0 ends on entry to
`ImScsiParallelReadWriteImageCompletion`, including error completion. For queued
writes it ends immediately after `ImScsiWriteDevice` returns; for UNMAP it ends
at the existing common end of dispatch after all attempted lower calls. The
worker's additional idempotent transition covers failures before backing work.
A direct setup failure moves to p1 and transfers the token to StartIo's stack
before the original work item is freed.

**p2 is a call interval, not a claim that Windows still owns an uncompleted SRB.**
It includes the few instructions between the diagnostic transition and the
call, and between the call's return and retirement. Port notification can cause
reentrant activity; the logical SRB may already have been returned during that
interval. Consequently `accepted` is conservative at that boundary. Keeping p1
and p2 separate lets owner results inform whether M3 needs to wait for any part
of final notification. M2 makes no such decision.

For deviodrv, acceptance is after successful request allocation and the second
connected-server check under `IrpListLock`, before publishing or sending work.

| deviodrv field | Meaning at a native flush snapshot |
| --- | --- |
| `p0` | Received, awaiting an exchange buffer; also the small interval before publishing a requeued request |
| `p1` | Submitting or sent, including the off-list preparation window before tagged sent-list publication |
| `p2` | Tagged reply removed from the sent list and being processed; includes upper `IoCompleteRequest` execution |
| `completed` | Cumulative terminal client completions after `IoCompleteRequest` returns, including errors/disconnection/cancellation |

Disconnect completion can go directly from p0 or p1 to completed without a
provider reply. Too-small exchange buffers move p1 back to p0 without creating
a new request. Reads, server exchange IRPs and server memory IRPs have empty
tokens. Only supported write, zero and trim client requests participate.

Neither driver can observe provider-private work after the provider acknowledges
the corresponding operation. Completion counts are not durability or success
counts. The two drivers' snapshots are independently consistent, not one atomic
cross-driver snapshot, and their totals need not match.

## Snapshot and output format

There are 16 snapshot slots and 16 raw UNMAP status slots per context. Each
snapshot is copied under one context spin lock, then printed after releasing
that lock. No per-read/write/completion debug printing is added. Mount (`op=fe`),
teardown (`op=ff`), real SYNCHRONIZE CACHE (`35`, `91`) and native deviodrv flush
(`09`) produce four lines beginning `AIMDIAG1`. Every line includes source,
context incarnation `id` and monotonic event `e`, so interleaved lines can be
reassembled. Each individual line fits the 512-byte DbgPrintEx limit.

| Fields | Interpretation |
| --- | --- |
| source, `id`, `born`, `adapter`, `lu` | Driver and LU/file-context incarnation; IDs are per driver load. Include boot/run identity when combining logs. `lu` is the existing packed `DEVICE_NUMBER`; deviodrv uses its file-context address as adapter identity and `lu=0`. |
| `ctx` | Statistics address for inspection with matching private symbols while still alive; not a persistent handle |
| `t`, `oldest100ns` | Interrupt-time timestamp and age of the oldest accepted token, across all phases. Divide age by 10,000 for milliseconds. Units are 100 ns; resolution is the system clock tick, not 100 ns. |
| `route`, `mount` | Backing route and original create flags. Routes: 0 unknown/initializing, 1 VM RAM, 2 queued file, 3 parallel FILE_OBJECT, 4 AWE FILE_OBJECT, 5 stream proxy, 6 shared-memory proxy, 7 deviodrv. |
| `op`, `immed`, `qa`, `srb`, `cdb` | Observed opcode, CDB byte 1 bit 1, raw SRB QueueAction, raw SrbFlags and all 16 SRB CDB bytes. For a 10-byte command only the first 10 CDB bytes are meaningful. These fields do not alter handling. |
| `accepted`, `p0`, `p1`, `p2` | Current populations, as defined above |
| `high=a,b,c,d` | High-water marks for accepted, p0, p1, p2 since context creation |
| `highAfter0` | High-water mark of p1+p2; individual phase highs must not be added |
| `peakSync` | Largest accepted population seen at an actual SYNC/native-flush event |
| `total`, `completed` | Cumulative accepted and retired logical modifying requests |
| `typePending`, `typeTotal` | Current/cumulative WRITE10, WRITE16, UNMAP, ZERO, in that order. deviodrv generic writes use the first slot; its trim and zero FSCTLs use the third and fourth. |
| `sync=a,b`, `flush`, `busySync` | SYNC10, SYNC16, native flush counts and number of these events with accepted nonzero |
| `zero`, `zeroPending`, `fallback` | phdskmnt zero-optimization attempts, still-accepted writes that attempted zero, and failed attempts leading to the unchanged write fallback |
| `unmapRaw`, `errors`, `last`, `lastError` | Number of actual lower UNMAP calls observed, number with failing NTSTATUS, latest returned status and latest failing status; inspect `errors` before interpreting `lastError` |

UNMAP with multiple lower zero-data calls has one accepted SRB and multiple raw
status observations. A zero-optimized WRITE remains its original WRITE10/16
token throughout success or fallback: it is not counted twice. UNMAP implemented
using lower zero-data calls remains an UNMAP token in phdskmnt and appears as
ZERO at deviodrv. `ZeroPending` annotates writes that tried the optional zero
optimization; it is not another population to add to accepted. The raw UNMAP
ring stores request sequence, call sequence, time and NTSTATUS before the
existing final status suppression. No new lower call or wait is made.

Snapshot invariants, for each complete event:

```text
accepted = p0 + p1 + p2
accepted = total - completed
accepted = sum(typePending)
```

`AIM_DIAG_CONTEXT::Now` contains live counters but timestamp/oldest/CDB fields
from its latest capture. Use a complete log event or a `History` slot as a
coherent snapshot; do not treat a debugger dump of Now as a new flush event.
History slot is `(e-1)%16`. Raw-status slot is `(Number-1)%16`. Sequence numbers
expose overwritten entries. Bounded history does not retain the whole workload.

SYNCHRONIZE CACHE is observed after successful data-LU lookup, before the existing
initialization check. A command to an initializing LU can therefore have route 0
and still receive the existing busy response. The ScsiPort dummy controller LU
and unsuccessful LU lookups do not count as data-LU synchronization events.
The normal deviodrv-backed AIM route appears as **route 3 in phdskmnt** and route
7 in deviodrv. It is not a stream/shared-memory proxy mount.

If statistics allocation fails, I/O proceeds unchanged and reports
`unavailable ... (no counters; inconclusive)` on the observation paths. This is
not a zero-outstanding result.

## Counter and lifetime ownership proof

For one context, references equal its published owner (if any), temporary
lookup/report references, and one reference per accepted token. Acceptance
adds exactly one reference and one intrusive list entry. Phase changes and
zero/status annotations do not change ownership. Retirement removes the entry,
decrements its current phase/type and accepted counts, increments completed,
clears token ownership, unlocks, and drops exactly that reference.

| Path | Ownership and balance |
| --- | --- |
| LU creation / initialization failure | Zeroed LU receives the initial statistics owner before LU-list publication. Normal cleanup, including initialization failure, detaches under LUListLock and releases that owner after reporting. Allocation failure leaves NULL. |
| Successful LU lookup | ScsiGetLUExtension takes a statistics reference while LUListLock protects publication. ScsiExecute releases it at its common exit. Async diagnostics never fetch statistics from the raw LU afterward. |
| Validation/work allocation failure | No token accepted; no token reference or counter to retire. Immediate completion sees an empty stack token. |
| Direct lower setup failure | No IRP has been submitted. Move replaces the token's list links with the stack token's links under the statistics lock, preserving its reference, sequence and age. The work token becomes empty before its original free. StartIo's existing notification retires the stack token. |
| Direct submission, including inline callback | Acceptance precedes setup and IoCallDriver. Callback moves to p1, then p2 at notification, then retires before the existing work free. The successful submission path makes no new access to the work token after the helper returns. The independent lookup reference survives even if the callback frees the token inline. |
| Queued write / UNMAP, success or failure | One token stays with the work item. Backing return/common worker fallback establishes p1. StorPort retirement follows the existing worker notification. ScsiPort retains p1 through its response queue and retires in the existing completion pump. |
| Zero optimization and fallback | One write token and reference; annotation only. The attempt can succeed or fall back without retirement/reacceptance. The original terminal path retires once. |
| deviodrv creation / final close | Statistics created after other fallible creation allocations, before file publication. Open file-context ownership remains until final close. Final close detaches the statistics owner before freeing the old context. |
| deviodrv flush/admission | The still-active IRP's FILE_OBJECT keeps its file context alive during reference acquisition. Native flush takes/releases a temporary statistics reference before its unchanged successful completion. Admission adds a token reference before visibility to another request owner. |
| deviodrv queued, immediate send, buffer-too-small requeue | Same request/token across every list transition. Phase updates precede release/publication that allows a new owner to process it. No second acceptance or allocation. |
| deviodrv reply, mapping/internal error, disconnect cancellation | All accepted terminal paths use DevIoDrvCompleteIrpQueueItem. It retires after the original IoCompleteRequest and before the existing item free, using only the token/statistics. No new IRP/file access follows completion. |
| Server/memory IRP cancellation and management work | Zeroed, unaccepted tokens. Existing frees require no statistics release. |
| LU/file owner gone while completions remain | Tokens independently retain statistics. Last token/temporary reference frees only the statistics allocation. No completion-side diagnostic code accesses HW_LU_EXTENSION or OBJECT_CONTEXT for statistics. |

The intrusive diagnostic list never dispatches I/O or transfers existing request
ownership. Only an existing request owner mutates its token, and its list entry
is removed before that owner's allocation/stack storage expires. Snapshot readers
hold the same lock, so they cannot race the oldest token's removal or transfer.

The statistics lock is held only for bounded counter/list/ring work; it is never
held across provider calls, port notification, IoCompleteRequest, debug printing,
allocation/free or waits. deviodrv sometimes enters it while holding IrpListLock;
there is no reverse acquisition. LU lookup uses only an interlocked reference
while holding LUListLock. All statistics and pending tokens are nonpaged.
The report helper is explicitly not inlined into filetable.cpp's PAGE section,
so capture remains in nonpaged code when it raises IRQL. Current StorPort,
worker and lower completion paths operate at or below DISPATCH_LEVEL. Historical
ScsiPort call-site coverage is retained but is not a tested legacy-driver claim.

This proof covers the added accounting under the existing single-terminal-owner
request contract. It does not repair or prove the baseline LU/SRB lifetime model.
In particular, old reset/removal races and deviodrv cancellation's existing
completion-under-IrpListLock behavior remain. A request that never reaches its
existing terminal path remains counted and retains its statistics reference;
diagnostics do not forcibly retire it. Teardown can legitimately log outstanding
tokens, since teardown is not a new drain and statistics may survive it.

## Validation performed

The editing host is Linux x86-64. MSBuild, cl, clang-cl, dotnet, Wine and the
Windows WDK are unavailable. **Neither .sys was built or loaded here. No Windows
binary, signing result, StorPort runtime pass or ScsiPort runtime pass is claimed.**

The actual shared helper passed the following host build and run:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -Wno-multichar -Wno-class-memaccess \
  -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
  tests/flush-diagnostics/accounting.cpp -o /tmp/aim-m2-accounting
ASAN_OPTIONS=detect_leaks=0 /tmp/aim-m2-accounting
git -c core.whitespace=cr-at-eol diff --check
```

Covered: empty snapshot, inline completion and reentrant observation, owners
released before token retirement, direct-failure list transfer (including a
middle entry), mixed/out-of-order completion, zero fallback counted once, raw
UNMAP error retention, deviodrv requeue/cancel, snapshot ring overwrite,
allocation failure and 100,000 concurrent requests with consistent snapshots.
AddressSanitizer and UndefinedBehaviorSanitizer passed. LeakSanitizer cannot run
in this host's ptrace environment; explicit allocation/reference balance and
empty-list assertions passed with leak detection disabled. This shim is not a
WDK/DDI/IRQL or port-lifetime simulator. Its context/token sizes are 4,680/56
bytes; obtain actual target sizes from that build's PDB.

## Windows build instructions (not executed here)

Use a complete checkout and a VS 2022 Developer PowerShell with WDK integration,
the SDK/WDK and C++ compiler libraries for the requested architectures, including
the ARM64 Spectre libraries requested by the existing project. The checked-in
x64/Win32 projects select WDK 8.1; ARM64 selects WDK 10. phdskmnt names SDK
10.0.22621.0, deviodrv 10.0.26100.0. No project retargeting was committed.

The following explicit **current Windows 10/11 validation profile** uses WDK
10.0.26100.0 for both drivers, x64/ARM64, Debug/Release. The command-line
overrides are intentional; this does not validate Windows 7/8 compatibility.
Run from the repository root and retain all logs and PDBs:

```powershell
$aimBuild = 'C:\AIM-M2\build'
New-Item -ItemType Directory -Force $aimBuild | Out-Null
git rev-parse HEAD | Set-Content "$aimBuild\commit.txt"
foreach ($arch in @('x64', 'ARM64')) {
    foreach ($mode in @('Debug', 'Release')) {
        foreach ($driver in @('phdskmnt', 'deviodrv')) {
            $cfg = if ($driver -eq 'phdskmnt') { "Win8.1 $mode" } else { $mode }
            $dest = "$aimBuild\$driver\$arch\$mode"
            New-Item -ItemType Directory -Force $dest | Out-Null
            & msbuild ".\Unmanaged Source\$driver\$driver.vcxproj" /m /t:Rebuild `
                "/p:Configuration=$cfg" "/p:Platform=$arch" `
                /p:PlatformToolset=WindowsKernelModeDriver10.0 `
                /p:WindowsTargetPlatformVersion=10.0.26100.0 `
                /p:TargetVersion=Windows10 /p:DriverTargetPlatform=Desktop `
                /p:SignMode=Off "/p:OutDir=$dest\out\" "/p:IntDir=$dest\obj\" `
                "/bl:$dest\build.binlog" /fl "/flp:logfile=$dest\build.log;verbosity=normal"
            if ($LASTEXITCODE -ne 0) { throw "Build failed: $driver $arch $cfg" }
        }
    }
}
Get-ChildItem $aimBuild -Recurse -Filter *.sys | Get-FileHash -Algorithm SHA256 |
    Format-List | Out-File "$aimBuild\sha256.txt"
```

For the repository's original target profile, use the installed matching older
toolsets/SDKs and omit the toolset/SDK/TargetVersion overrides. Win32 and historical
ScsiPort require their compatible build environments; the current profile above
does not build them. A missing-toolset failure should be reported as such, not
worked around by changing unrelated source. The build emits unsigned artifacts;
use the owner's existing driver signing and test deployment process. Prefer the
Release pair for workload measurement because existing DBG traces are numerous.
Keep the current installed pair available for the baseline comparison.

## Diagnostic collection

1. Record OS/build, machine architecture, exact source commit, loaded driver
   hashes and provider/version/mount options. Use matching private PDBs and
   verify loaded modules with `lmvm phdskmnt` and `lmvm deviodrv` in WinDbg.
2. Attach kernel WinDbg before mounting the test disk. Create `C:\AIM-M2\logs`
   on the debugger host. Preserve the original IHVDRIVER mask, then enable INFO
   bit 3 without removing its other bits:

   ```text
   dd nt!Kd_IHVDRIVER_Mask L1
   ed nt!Kd_IHVDRIVER_Mask (dwo(nt!Kd_IHVDRIVER_Mask) | 8)
   .logopen /t C:\AIM-M2\logs\fixed.txt
   g
   ```

3. Mount one test disk at a time and retain its `op=fe` identity/route records.
   Run the workload without injecting extra flushes. Note wall-clock start,
   browser-reported completion, continued allocation/write activity and eventual
   idle separately. Do not break into the debugger or use completion breakpoints
   during the timed interval.
4. After idle, perform the normal unmount and retain `op=ff`, then `.logclose`.
   Repeat with a fresh log and mount for the dynamic provider. Keep the complete
   log, including incomplete events and errors; do not send only selected hits.
5. If measuring print perturbation, set `ed phdskmnt!AimDiagPrint 0` and
   `ed deviodrv!AimDiagPrint 0` while stopped, then resume for a separate run.
   This disables printing, not accounting or capture. Reset to 1 before the next
   logged run. Restore the original filter mask after collection.

Each flush snapshot comprises four lines with the same source/id/e. Group by
those keys within one driver load; verify the equations above. Gaps in `e`,
missing lines, unavailable contexts or wrong binaries make negative findings
inconclusive. Snapshot printing can perturb flush timing even though the
snapshot precedes printing; do not use this build for precise latency claims.
An `op=ff` count summary confirms cumulative SYNC counts even when none occurred
during the browser download. Mount/teardown themselves do not increment SYNC.

With matching PDBs and a **known still-live** context, a debugger stop can inspect
`dt phdskmnt!AIM_DIAG_CONTEXT <ctx>` (or the deviodrv type) and its `History` and
`UnmapHistory`. Never dereference a logged address after teardown or invoke the
helper from the debugger. There is no new query IOCTL or retained post-free
registry. The log is the primary collection method.

Microsoft references: [virtual StorPort StartIo IRQL](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/storport/nc-storport-hw_startio),
[DbgPrintEx and its output/IRQL limits](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-dbgprintex),
[debug message filtering](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/reading-and-filtering-debugging-messages),
and [KeQueryInterruptTime units and resolution](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kequeryinterrupttime).

## Short owner test matrix

Use the same filesystem, capacity, browser, download payload and mount options
where applicable; repeat each natural workload three times on fresh mounts.
Do not change the provider's acknowledgement behavior to make the hypothesis
easier to observe.

| Test | Fixed VM RAM | Dynamic deviodrv provider | Collect |
| --- | --- | --- | --- |
| Idle mount and ordinary idle flush/unmount | Yes | Yes | Identity/route, zero-outstanding events if actually issued, cumulative command counts |
| Reporter workload or closest available browser-download reproduction, baseline driver pair | Yes | Yes | Timing, provider activity and whether the symptom reproduces |
| Same natural workload, M2 Release driver pair | Yes | Yes | Full AIMDIAG1 log; phase populations at real SYNC10/16; symptom and timing |
| Sustained write load followed by idle | Yes | Yes | High-water marks, repeated SYNC events if Windows sends them, final accepted population |
| File deletion/TRIM and zero-filled data mixed with ordinary writes, using already-supported operations | As supported | As supported | Type totals, zero/fallback counts and raw UNMAP errors; do not enable capabilities |
| Controlled provider failure/disconnect and pending LU removal, separate disposable test | Removal only | Yes, after normal runs | Crash/hang or diagnostic balance evidence and dumps; this exercises existing teardown hazards |

If the provider also offers fixed/preallocated RAM, include that mode as a useful
third comparison. A diagnostic-control run issuing known SYNC10 and SYNC16 can
validate collection separately from the natural workload. A direct native flush
test against deviodrv is likewise separate. Do not modify the production driver
or provider to manufacture these commands during the reproduction. M2 does not
provide a new command-injection harness or claim those control tests were run.

## Interpretation and the M2 stop gate

| Observed real event | Supported conclusion / next evidence |
| --- | --- |
| SYNC35/91 with `p0 > 0` | Earlier accepted modifying operations have not reached the measured backing boundary when SYNC is observed. This supports an ordering investigation; it does not prove that those operations cause the download symptom. |
| `p0=0`, `p1>0` | Earlier modifying backing work finished, but at least one request has not entered final notification. Consider this separately when designing M3. |
| Only `p2>0` | Overlap is at final notification/retirement, potentially reentrant. It is not evidence of unfinished backing work or a reason by itself to wait for notification return. |
| All three populations zero at SYNC | No overlap at these boundaries at that instant. Private provider work after acknowledgement and higher-layer delayed writes remain outside this measurement. |
| No natural SYNC events, but mount/teardown and cumulative counters are present | The workload did not expose this mechanism at the miniport during the capture. A SYNC barrier cannot explain or fix that run without further evidence. |
| deviodrv native flush with p0/p1/p2 nonzero | Requests are respectively received, submitting/sent or in reply processing at that lower-layer observation. The current native flush still returns immediate success. |
| No deviodrv native flush events | Expected if only phdskmnt's current SYNC no-op was exercised: it does not forward a native flush. This is not missing deviodrv I/O accounting. |
| Provider still allocates/writes after both drivers' relevant terminal boundaries | Consistent with work outside the acknowledged request lifetime; correlate with provider instrumentation before proposing protocol or persistence changes. |
| `errors>0` / raw UNMAP failure | A previously hidden lower status was observed. M2 still returns the baseline final status. Report the error separately from barrier overlap. |
| A counter equation fails, history is incomplete, or teardown is followed by a crash/hang | Treat as an instrumentation/lifetime or capture problem requiring investigation; do not use it to choose barrier semantics. Nonzero teardown alone is not proof of a leak. |

Return the build logs, source SHA, .sys hashes, matched PDBs if a dump is needed,
complete per-run logs, workload/timing notes and provider mode/version. M3 remains
unstarted pending owner review and these results. The SourceForge report is not
claimed solved by this diagnostic build.

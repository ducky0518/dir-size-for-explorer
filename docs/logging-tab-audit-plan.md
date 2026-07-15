# Logging Tab Deep Review (Tray App)

## Key Issues Identified

1. Critical: UTF-8 conversion buffer overflow in service code
   - `service/ipc_server.cpp:192`, `service/ipc_server.cpp:197`
   - `service/log_buffer.cpp:121`, `service/log_buffer.cpp:124`
   - Both allocate `needed - 1` bytes but pass `needed` to `WideCharToMultiByte` with `-1` input length (writes null terminator), causing a potential 1-byte overwrite.
   - This is a likely contributor to intermittent blank/misaligned behavior.

2. Logging tab uses static state across dialog instances
   - `tray/settings_dialog.cpp:48` (`s_lastSeqNum`)
   - `tray/settings_dialog.cpp:49` (`s_verbosityFilter`)
   - On reopen, UI controls reset, but static filter/sequence may not, causing out-of-sync behavior and blank logs until new entries arrive.

3. Verbosity dropdown and active filter can diverge
   - `tray/settings_dialog.cpp:396` always sets combo to Normal.
   - `tray/settings_dialog.cpp:492` updates static filter only on selection change.
   - If filter persisted from a previous dialog, UI says Normal but behavior is still previous filter.

4. Failure handling can preserve stale/blank status
   - `tray/settings_dialog.cpp:232` failure path.
   - `tray/settings_dialog.cpp:235` only shows "Service not connected" when `s_lastSeqNum == 0`.
   - Reopened dialog + carried sequence can leave status blank or stale during failures.

5. Status text can incorrectly show Idle during active scan
   - `tray/settings_dialog.cpp:272` requires `isScanning && !currentPath.empty()` to show Scanning.
   - `service/scanner.cpp:190` clears path before `service/scanner.cpp:201` sets scanning false.
   - This timing window can produce contradictory "Idle" while scan is still active.

6. Parser/sequence robustness gaps
   - `tray/settings_dialog.cpp:248` updates sequence before complete payload validation.
   - If payload is malformed/truncated, logs may appear dropped until manual reset.
   - Sequence/truncation bit overloading (`service/ipc_server.cpp:208`, `tray/settings_dialog.cpp:242`) is fragile long-term.

## Detailed Remediation Plan

### Phase 1: Safety/Correctness Hotfixes (highest priority)
- Fix UTF-8 conversion buffer sizing in:
  - `service/ipc_server.cpp`
  - `service/log_buffer.cpp`
- Add strict conversion return-value checks and safe fallback behavior.

### Phase 2: Logging Tab State Lifecycle Cleanup
- Remove static cross-dialog logging state (or fully reinitialize on dialog init/tab enter).
- Ensure verbosity combo and filter always have one source of truth.
- On Logging tab open, define deterministic initial behavior:
  - show recent buffered history, or
  - show only new events for this session.

### Phase 3: Refresh/Parse Hardening
- Validate full payload layout before applying `latestSeqNum`.
- If `currentPathLength` or entry bounds are invalid, fail gracefully and retain last good UI state.
- Add explicit degraded/disconnected status after configurable consecutive poll failures.

### Phase 4: Status Consistency
- Expose a single atomic scanner status snapshot from service:
  - `isScanning`, `currentPath`, `lastScanTimestamp`
- In UI, when `isScanning == true` and path is empty, show "Scanning (path unavailable)" rather than Idle.

### Phase 5: Protocol Robustness / Future-proofing
- Replace truncation-in-high-bit sequence encoding with explicit flags in payload/header.
- Consider widening sequence handling and documenting restart/wrap semantics.
- Add regression tests for:
  - dialog reopen behavior,
  - verbosity synchronization,
  - transient IPC failures,
  - malformed payload handling,
  - service restart sequence continuity.

## Why This Matches Reported Symptoms

- Blank on load: static sequence + fresh edit control + no new entries.
- "Need to switch to verbose then back": that path resets sequence/filter and forces full re-fetch.
- "Not true current status": mixed snapshot timing + UI logic requiring non-empty path for scanning display.

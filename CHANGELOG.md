# Changelog

## 2.4.0

Correctness and security release: fixes several bugs that silently
corrupted cached totals (especially on network shares), stops the shell
hook from affecting other applications, and hardens the installer and IPC.
Driven by an external code review; all high-priority findings addressed.

### Data correctness (cache accuracy)

- **Fixed incremental updates writing ancestor totals under wrong keys.**
  Paths read back from SQLite carried an embedded terminating NUL
  (`Utf8ToWide` counted the terminator), so every ancestor update from a
  shallow rescan re-upserted the parent under an invisible `path\0` key:
  parent folder totals never actually updated between full scans, and
  unreachable ghost rows accumulated. Deleted-subtree purging was broken
  by the same bug (`RemoveByPrefix` on a `GetChildDirs` result matched
  nothing). A schema migration (v2) deletes accumulated ghost rows from
  existing databases; stale real rows self-heal through normal scanning.
- **Enumeration failures are no longer treated as successful scans.** A
  directory listing that ends in anything other than `ERROR_NO_MORE_FILES`
  (SMB session drop, device error) is now recognized as truncated:
  shallow rescans abandon the update (keeping cached data) and retry
  after the cooldown, instead of writing undercounted totals and purging
  every cached child the enumeration never reached. Deep scans propagate
  a `complete` flag — no entry is written for any directory whose subtree
  enumeration failed, and failed subtrees are no longer folded into
  parents as zero bytes.
- **Full scans are now real reconciliations.** After a fully completed
  root scan, rows under the root whose `scan_time` predates the scan are
  purged — previously full scans only ever added rows, so subtrees
  deleted while the engine was off lingered in the cache forever.
- **Interrupted scans can no longer masquerade as fresh data.** Each root
  scan sets a persisted "scan owed" flag that is cleared only on
  completion. At startup, a set flag forces a reconciliation scan
  regardless of how fresh the shutdown heartbeat makes the cache look
  (previously a scan cancelled by shutdown was silently skipped forever).
- **Watcher overflow/reconnect now triggers a deep reconciliation scan.**
  Notification-buffer overflows and watcher reconnects lose changes at
  unknown depths; the previous shallow root rescan reused cached child
  totals and repaired nothing, while real-time coverage kept the interval
  backstop suspended. Both paths now queue a deep scan of the root that
  bypasses the real-time suspension (persisted across restarts, retried
  on failure). Relatedly, an in-flight interval scan is no longer
  cancelled when change watching becomes active — that scan is often the
  one repairing what the watcher missed while it was down.

### Explorer/system safety

- **The Detours hooks are now installed only inside `explorer.exe`.** The
  shell DLL also gets loaded into any application that shows a common
  file dialog; previously the process-wide `NtQueryDirectoryFile` hook
  then rewrote directory-entry sizes for that application's own file
  enumerations (backup tools, sync clients, installers saw fabricated
  data). Hooks are also no longer detached during process termination
  (loader-lock/Detours deadlock risk); the module pin covers live hooks.

### Security

- **Settings moved to HKCU.** Configuration is now per user
  (`HKCU\SOFTWARE\DirSizeForExplorer`), written by the settings dialog
  without elevation. `HKLM` holds read-only machine defaults — the
  installer no longer grants the `Users` group write access to it, which
  previously let any local user repoint every other user's scan engine
  (e.g. at a hostile UNC path scanned with that user's credentials).
  Settings saved by older versions in HKLM keep working as fallbacks
  until the first per-user save.
- **IPC hardened on both ends.** The client (Explorer context menu,
  settings dialog) previously used unbounded synchronous reads/writes
  after connecting — a stalled engine could hang Explorer's UI thread —
  and trusted the response length field for allocations (a spoofed pipe
  could demand gigabytes). All client IO is now overlapped with the
  request timeout applied per operation, and response payloads are capped
  at 16 MB. The server's unbounded `FlushFileBuffers` (a client that
  never drained its response could wedge the single-threaded listener
  forever) was replaced with a time-limited wait for the client to
  finish.

### Installer

- **No VC++ redistributable required.** Binaries now link the CRT and
  sqlite3 statically (`/MT`, vcpkg `x64-windows-static`); previously the
  package shipped no runtime, so on clean machines the tray and shell
  extension silently failed to load. `sqlite3.dll` is no longer shipped.
  (Rebuilding from source: delete `build\` once — the vcpkg triplet
  changed.)
- **Registration failures now fail the install** (with rollback) instead
  of reporting success while Explorer integration doesn't work.
  `DllRegisterServer` reports errors instead of always returning `S_OK`.
- **Uninstall actually cleans up:** the tray app is closed on install,
  upgrade, and uninstall (`WM_CLOSE` first, terminate as last resort),
  and Explorer is stopped before file removal and restarted afterwards —
  previously the hooked DLL stayed pinned inside Explorer and files
  remained locked until reboot.
- **Third-party property handlers are preserved.** `Directory` has a
  single machine-wide property-handler slot; installing DirSize now saves
  any existing handler and uninstalling restores it (and leaves the slot
  alone entirely if another product has taken it over since), instead of
  overwriting on install and blind-deleting on uninstall.

## 2.3.0

Log clarity and settings-dialog fixes.

- **Scan-type labels throughout the log:** every operation now names its
  trigger — "Shallow rescan queued/done" (with file count and elapsed ms,
  closing the loop on queued entries that previously looked like they
  vanished into an idle engine), "Baseline scan", "Interval scan",
  "Full (Scan Now) scan" (previously mislabeled as Interval),
  "Reconciliation scan", "Manual scan", and "Startup check" (previously
  logged as a "Full scan … completed in 0.0 seconds" even when every
  baseline was reused and nothing was walked).
- **Logging tab paint fix:** switching to the Logging tab could show a
  blank log until a new entry arrived or the box was clicked — the tab
  control's background painting raced the freshly shown sibling controls.
  The tab now clips siblings (`WS_CLIPSIBLINGS`) and tab pages invalidate
  their controls on show, fixing the same latent issue on all tabs.

## 2.2.0

Watcher resilience and notification-storm efficiency.

- **Self-healing change watchers:** SMB change-notify subscriptions die
  with the session (idle disconnects, NAS reboots, network drops), and
  previously that silently killed the watcher thread while interval scans
  stayed suspended — leaving the root permanently un-tracked until the
  tray was restarted. Watchers now detect the loss, resume interval scans
  immediately, retry the subscription once a minute, and on reconnect
  queue a root rescan to cover the blind window. Losses and recoveries
  log at Normal verbosity ("change notifications lost/flowing for X"),
  including a one-time delivery confirmation per connection so a healthy
  watcher no longer looks silent outside Verbose mode.
- **Per-directory rescan cooldown (30 s):** sustained writes (large
  downloads, builds) re-notify their folder on every flush, which caused
  the same hot directory to be re-enumerated every couple of seconds for
  the duration of the transfer. The first change to a folder still
  rescans promptly; repeats within the cooldown are deferred and merged
  into a single scan, cutting SMB enumeration traffic during bursts by
  an order of magnitude. Notification-buffer overflows are handled
  explicitly (shallow root rescan) instead of being treated as errors.

## 2.1.0

Refinements from real-world use on multi-million-file NAS shares.

### Scanning behavior

- **Interval scans suspend at watch-arm time**, not at the first change
  notification. Waiting for an actual change meant quiet shares never
  confirmed real-time coverage and kept receiving hours-long interval
  scans despite a healthy watcher.
- **In-flight interval scans cancel** when change watching becomes active
  for the root being scanned (baseline and Scan-Now scans are never
  cancelled).
- **Baselines are reused across restarts** instead of re-walking every
  watched tree at each logon.
- **Downtime reconciliation:** the engine writes a liveness heartbeat
  every minute; at startup, any root whose "blind window" (time with no
  engine running) exceeds its interval setting gets a reconciliation
  scan. Quick restarts cost nothing; changes made while the engine was
  off are picked up automatically.
- **Manual Scan tab:** browse to any folder and scan it on demand — even
  folders outside the watched directories (results are cached, so
  Explorer shows their sizes immediately; a way to try the product before
  setting up a watched directory). Shows the last five manual scans with
  start/finish times, result totals, and whether each folder is covered
  by a configured watched directory.

### Explorer integration

- **Live view refresh:** after a rescan the engine bumps a shared
  size-data generation counter (invalidating the shell extension's
  in-process cache) and sends `SHChangeNotify` for every directory whose
  total changed, so open Explorer windows update within seconds instead
  of waiting out the 60 s cache TTL.

### Housekeeping

- **Database pruning:** trees no longer under any watched root are
  removed when the watched-dir list changes (and at engine startup);
  VACUUM reclaims file space after large prunes and exclusion purges.
- Interval/baseline scans log at Normal verbosity with distinct labels,
  so a busy status is always explainable from the log; the status line
  shows the baseline's real age (with date) instead of "Never" after a
  restart that reused it.

## 2.0.0

Major release: the scan engine moved from a Windows service into the user's
session, built around network-drive support as the primary use case.

### Architecture

- **Removed the LocalSystem Windows service.** The scan engine (scanner,
  IPC server, change watchers) is now a static library (`DirSizeEngine`)
  hosted by `DirSizeTray.exe`, auto-started at logon. Running in the user's
  session means mapped drive letters resolve and SMB shares authenticate
  with the user's own credentials — network drives work with zero
  configuration.
- **Per-user database** moved from `%ProgramData%` to
  `%LocalAppData%\DirSizeForExplorer`. This also fixes the shell extension
  being unable to read the database at all: SQLite WAL readers need write
  access to the `-shm`/`-wal` side files, which non-admin Explorer never
  had under ProgramData.
- **Session-scoped IPC pipe** (`\\.\pipe\DirSize_<sessionId>`) with default
  (owner-only) security, replacing a single global pipe with a NULL DACL.
- **Watched directories are canonicalized to UNC form on save** (mapped
  letter `H:\...` → `\\server\share\...`), so entries stay valid if the
  letter is remapped and match the keys used by the scanner and Explorer.
- Tray app is single-instance per session; installer no longer installs a
  service or a shared data folder; new one-step `build-installer.cmd`.

### Real-time change tracking

- **USN change journal replaced with `ReadDirectoryChangesW` watchers** —
  no admin rights needed, works on any filesystem that supports change
  notifications (local disks, Windows shares, most NAS). Zero CPU while
  idle (blocking overlapped reads), 2 s debounce during write bursts.
- **Shallow rescans:** a change re-enumerates only the affected directory;
  subdirectory totals are reused from the database and the size delta is
  propagated up the ancestor chain. Updates cost one directory listing
  even on high-latency shares (previously: full subtree rescan, and
  ancestors stayed stale until the next full scan).
- **Interval scans auto-suspend per root** once real-time coverage is
  confirmed (shown as "Real-time: Active" in settings; the Interval
  column shows "—" while suspended). Coverage loss automatically resumes
  interval scanning; "Scan Now" always scans everything. Every root is
  guaranteed one baseline full scan before suspension can apply — a newly
  added directory whose watcher fires immediately still gets its initial
  scan.
- **Per-directory scan intervals** — each watched root has its own
  full-scan interval (new list view in settings: Directory / Interval /
  Real-time). Deadline-based scheduling measured from each root's last
  scan (previously any activity reset a single global timer, postponing
  full scans indefinitely).
- **Rename tracking** ("Follow watched folders when renamed or moved",
  on by default): watched roots are followed via their open handle —
  config and cached sizes move to the new path automatically. Renamed or
  moved subfolders keep their cached subtree via a single database rewrite
  instead of a rescan.
- **Excluded directories** — new settings list; excluded trees are not
  scanned, not counted in any parent's total, and are purged/restored
  incrementally when the exclusion list changes.

### Reliability and performance fixes

- Explorer crash risk removed: the shell DLL pinned itself once Detours
  hooks are installed and `DllCanUnloadNow` refuses unload (previously COM
  could unload the DLL with live hooks patched into ntdll).
- Fixed wrong `FileName` offsets for two NT directory-info classes
  (`FileIdFullDirectoryInformation` 72→80, `FileIdExtdDirectoryInformation`
  120→88) that corrupted name reads in the enumeration hook.
- Negative caching in the shell extension: unknown paths no longer trigger
  a SQLite query per directory entry on every Explorer refresh; failed
  database opens retry every 30 s instead of leaving Explorer blind until
  restart; hooks early-out when the database is unavailable.
- Database: fixed self-deadlock in `Open()` failure path; escaped SQL LIKE
  wildcards (paths containing `_`/`%` could delete unrelated rows, and
  `C:\A` also matched `C:\AB`); `BEGIN IMMEDIATE` batch transactions;
  case-sensitive LIKE so prefix operations use the index.
- Scanner: rescan queue no longer holds its lock during scans (blocked the
  watcher and IPC threads); queued paths are deduplicated; entries flush
  in 4096-row chunks instead of buffering entire trees in memory; UNC
  long-path prefixes; throttling is per directory instead of per file
  (Very Low priority previously slept 50 ms per 10 *files*), and
  background-priority mode is properly exited on setting changes.
- IPC server hardened: untrusted length fields capped, all pipe IO is
  overlapped with timeouts and shutdown cancellation (a stalled client
  could previously hang the listener and block service shutdown).
- Installer: property-schema registration actually works now (`regsvr32`
  instead of an invalid `rundll32 PSRegisterPropertySchema` call); removed
  a stale duplicate custom-actions file that broke the documented build.
- Settings dialog: Logging tab shows history immediately on reopen
  (previously blank until a new entry arrived); Clear no longer un-clears
  on the next poll; verbosity selector stays in sync; multi-select
  (Shift/Ctrl) removal in both directory lists; settings Apply no longer
  restarts unaffected watchers (which reset their real-time state).
- MSVC builds use `/utf-8` (fixes mojibake like `â€"` in the UI and logs).
- Tray: status polling is a direct in-process query instead of IPC every
  3 s; "Recalculate Size" cache invalidation actually works (canonicalized
  path); larger path buffers throughout (deep paths beyond `MAX_PATH`).

## 1.1.0

Initial architecture: LocalSystem service scanner with USN change-journal
monitoring, ProgramData SQLite cache, Detours-based Explorer hooks, tray
settings app.

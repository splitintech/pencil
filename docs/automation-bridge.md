# Optional local automation bridge (development)

This experimental integration is disabled in normal upstream-style builds.
It is protocol neutral: no MCP, Python, provider SDK or arbitrary code execution
is embedded in the application. Python MCP/CLI source lives in
https://github.com/splitintech/stanlee.

Build with qmake `STANLEE_AUTOMATION=1` or CMake
`-DSTANLEE_AUTOMATION=ON`. Code uses C++17. Qt 5 and Qt 6 qmake compatibility
and Qt 6 CMake builds are retained. Normal builds preserve their identity.
Automation builds use a separate Pencil2D–Stanlee identity and settings name.

Runtime activation requires all of:

- `STANLEE_AUTOMATION=1`.
- `STANLEE_WORKSPACES`: absolute existing directories separated by the platform
  path-list separator.
- `STANLEE_DISCOVERY_DIR`: absolute private directory (POSIX owner-only mode).
- Optional `STANLEE_SETTINGS_DIR`: isolated settings and instance-lock directory.

The enabled application shows an automation status and a disconnect button.
Start from a blank native document, then open a file through the authenticated
API. Discovery records contain session-scoped credentials and must never be
published. The Python session listing strips the token.

IPC uses QLocalServer and a four-byte big-endian byte length followed by UTF-8
JSON, bounded to 4 MiB. Each connection handles one authenticated request and
then closes. The request includes `protocol_version`, `id`, `token`,
`session_id`, `operation`, `arguments`, `document_id`, and `expected_revision`.
All native dispatch occurs on the application thread. Mutations reject stale
documents/revisions, active tools, playback and modal widgets. Native signals
advance revisions and populate a bounded event log; clients poll `events.poll`
and must resynchronize if a gap is reported.

`app/data/automation-operations.json` is the versioned shared command registry.
Keep its contents byte-for-byte synchronized with Stanlee's contracts and Python
package. No arbitrary QAction/QObject invocation is exposed. Unsupported commands
fail explicitly. The registry records undo limitations; layer creation and FPS
changes are not yet undoable and must not be included in atomic batches.

`Editor::snapshot` clones native content into an independent Object/working
directory and copies manager state without marking live history clean.
`document.snapshot` never temporarily saves the active document under another
name. An optional FPS override changes only the copy. Native/Python tests check
source path, dirty state, bitmap backing files, source bytes, preview and undo
preservation. Recovery orchestration and comprehensive sound/vector fixtures
remain pending.

Current qualification is macOS arm64 only. Windows uses a current-user-owned
discovery DACL and QLocalServer user-access named pipes; runtime qualification
is pending Windows CI. Invalid or uninspectable ACLs fail closed.
Full editor coverage, atomic batches, robust document
recovery, platform/client acceptance and signed release packaging remain pending.
This integration is not a complete Stanlee alpha and is not ready for upstream
feature review. See the public build plan and coverage register for release gates.

`tool.inspect` returns native property names, enabled states and limits. `tool.configure` validates all supplied values before applying them and notifying native tool widgets. Stroke samples use native tablet events with pressure and keyboard modifiers on Qt 5 and Qt 6.

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
fail explicitly. `native_document_transaction` operations use one reversible
content transaction. `edit.batch` validates 1–64 eligible steps before dispatch
and has a 16,384 pointer-sample budget. Files, settings, history, view changes and
nested batches are excluded. Native context failures roll back the content and
preserve earlier redo entries.

`Editor::snapshot` clones native content into an independent Object/working
directory and copies manager state without marking live history clean.
`document.snapshot` never temporarily saves the active document under another
name. An optional FPS override changes only the copy. Native/Python tests check
source path, dirty state, bitmap backing files, source bytes, preview and undo
preservation. Checkpoint recovery invalidates old handles and history. Full
legacy-format, sound and vector qualification remains pending.

Transactions work with both legacy and QUndoStack history. They restore content
inside the same Object and working directory, preserve native human frame history,
retain independent audio bytes, and keep retired layer IDs unavailable for reuse.
A content-signature preflight rejects snapshot undo/redo if newer unrecorded human
changes would be overwritten. Active tools, colors, view and preferences stay
live. Project FPS changes do not write the application FPS default. Full-document
snapshots currently make memory/preparation cost proportional to project size.

Layer properties, duplication/order/deletion, timeline exact-key editing, exposure,
uniform opacity and native camera key/path/easing operations are advertised by
the registry. Previews render independent normalized content. Camera copy/path
serialization and bitmap save bounds have round-trip regression tests.

Previous foundation qualification passed the three-platform Qt 5/6 CI matrix;
new editing changes require a fresh matrix run. Windows uses a current-user-owned
discovery DACL and QLocalServer user-access named pipes. Invalid or uninspectable
ACLs fail closed. Full editor coverage, comprehensive recovery, platform/client
acceptance and signed release packaging remain pending.
This integration is not a complete Stanlee alpha and is not ready for upstream
feature review. See the public build plan and coverage register for release gates.

`tool.inspect` returns native property names, enabled states and limits. `tool.configure` validates all supplied values before applying them and notifying native tool widgets. Stroke samples use native tablet events with pressure and keyboard modifiers on Qt 5 and Qt 6.

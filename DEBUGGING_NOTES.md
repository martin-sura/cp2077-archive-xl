# ArchiveXL Regression & Root Cause Analysis on Cyberpunk 2077 Patch 2.5

## 0. Update (2026-10-29): Confirmed Root Cause & Fix

> [!IMPORTANT]
> Tests E–F (section 2, section 6.3) supersede the conclusion below and the recommendation in 6.1.
>
> **Root cause:** the affected mod meshes reference a chunk material (`material_01_multilayer`) that has no entry in the mesh's `materialEntries`. Since patch 2.5, `CMesh::LoadMaterialsAsync` then returns `engine\materials\fallback.remt` for **every** material in the request, so the whole mesh renders invisible. The built-in DynamicMesh extension (bridge `vfunc[+0x70]`) compensates for this, which is why nulling only its slot (Test C2) or suppressing the built-in service (Test B, upstream `1417f40`) exposes the bug. The meshes are **not** ArchiveXL-dynamic (no `@` markers), so ArchiveXL's own `MeshExtension` ignored them.
>
> **Fix (Test F, verified in-game):** `App::MeshExtension::LoadMaterialsWithMissingEntries` substitutes missing names with a valid one before the engine call and puts an empty material into those slots afterwards. This works with the built-in service suppressed, keeping upstream's design. Re-enabling the built-in service (6.1) is no longer needed.

## 1. Executive Summary & Original Root Cause Hypothesis

Following the release of **Cyberpunk 2077 Patch 2.5** and **ArchiveXL 1.28.3**, several dynamic appearance items—specifically Dynamic Mesh clothing items such as `nd_aquelyras_goth_belt` and `nd_aquelyras_punk_shoes`—failed to render in-game (appearing completely invisible), while static and standard dynamic appearance items (such as `axellysse_stars_belt`) continued to render normally.

Through systematic reverse engineering of `Cyberpunk2077.exe`, assembly inspection, and selective memory-slot bisection testing, the exact root cause has been mathematically isolated and verified in both directions:

> [!IMPORTANT]
> **The Definitive Root Cause:**
> In Patch 2.5, CDPR embedded ArchiveXL 1.28.2 directly inside `Cyberpunk2077.exe` via a global bridge interface pointer at `[0x143BCDBE0]`.
> During mesh evaluation in REDengine, `CMesh::FindAppearance` (at `+0x42` and `+0xE2`) and `CMesh::LoadMaterialsAsync` (at `+0x595`) directly invoke the bridge methods `vfunc[+0x68]` (`0x142303500`) and `vfunc[+0x70]` (`0x142303820`).
> These methods delegate to the embedded **`MeshExtension`** (`0x14232A8A0` / `0x14232AC30`) to resolve dynamic material templates (`@context` and `@material`) and instantiate them asynchronously via a dedicated `JobQueue`.
>
> When ArchiveXL commit `1417f40` ("Disable built-in extension service") suppressed `Raw::ExtensionService::Bootstrap`, REDengine's global embedded extension pointers remained `nullptr`. Consequently, `CMesh::FindAppearance` and `CMesh::LoadMaterialsAsync` bypassed material resolution and took a silent no-op fallback path. Although the mesh component was successfully assembled and attached to the entity, it possessed **no resolved material instances**, rendering it completely invisible.

---

## 2. Experimental Verification Matrix

Every potential point of failure was tested in-game with exact runtime logs:

| Test | Configuration | Built-in Bootstrap | Built-in Garment Slot (`0x3963858`) | Built-in Mesh Slot (`0x3963898`) | In-Game Render Result | Conclusion |
| :--- | :--- | :---: | :---: | :---: | :---: | :--- |
| **Reference** (`51d88b1`) | Working baseline commit | ✅ Active | ✅ Active | ✅ Active | **Visible** | Both belt and shoes render cleanly. |
| **Test A** | TruthLogger with `AXL_SUPPRESS_BUILTIN_EXTENSIONS=0` | ✅ Active | ✅ Active | ✅ Active | **Visible** | Confirmed: Problem only occurs when built-in bootstrap is suppressed. |
| **Test B** | TruthLogger with `AXL_SUPPRESS_BUILTIN_EXTENSIONS=1` (`1417f40`) | ❌ Intercepted (no-op) | ❌ Inactive (`nullptr`) | ❌ Inactive (`nullptr`) | **Invisible** | Confirmed: Suppressing built-in extension service reliably reproduces the bug. |
| **Test C1** | Selective Slot Bisection: Mode `GARMENT` | ✅ Active | ❌ **Nulled out** (`0`) | ✅ Active | **Visible** | **GarmentExtension is completely innocent.** Garments render properly even when embedded GarmentExtension is eliminated. |
| **Test C2** | Selective Slot Bisection: Mode `MESH` | ✅ Active | ✅ Active | ❌ **Nulled out** (`0`) | **Invisible** | **MeshExtension is 100% the sole root cause.** Nulling only the Mesh slot reproduces the exact invisibility. |
| ~~**Test D (Sol. 1)**~~ | ~~Bridge VTable Hook with `AXL_SUPPRESS_BUILTIN_EXTENSIONS=1`~~ | ❌ Intercepted (no-op) | — | — | **Invisible** | **Abandoned.** 0 bridge calls logged. Most likely the global bridge pointer is never set when `Bootstrap` is suppressed, so every call site takes its `jz` fallback and the patched VTable is unreachable (unverified, see 6.2). Approach discarded in favour of Test E. |
| **Test E** | External `App::MeshExtension` tracing with `AXL_SUPPRESS_BUILTIN=1` + `AXL_MESH_TRACE=<filter>` | ❌ Intercepted (no-op) | — | — | **Invisible** | All external hooks fire (`PostLoad`, `FindAppearance`, `LoadMaterialsAsync`). **`PrepareMeshState` reports `dynamic=false` for all affected meshes** → no `MeshState` → `OnFindAppearance` skips, material job aborts (`no target mesh state`) although `unresolved=true`. Every appearance reports `chunks=44` regardless of mesh (suspicious). Log: `ArchiveXL-2026-10-29-02-48-27.log`. |
| **Test E2** | As Test E + name/hash dump + RTTI layout check | ❌ Intercepted (no-op) | — | — | **Invisible** | **RTTI layout matches SDK** (no offset shift). **CNames resolve correctly.** The meshes contain no `@` template markers; `chunks=44` is the real submesh count. `dynamic=false` is therefore **correct**: these are static meshes and the external `MeshExtension` legitimately ignores them. New question: what does the built-in DynamicMesh do for *static* meshes that 2.5 depends on? Log: `ArchiveXL-2026-10-29-02-53-10.log`. |
| **Test E3** | As E2 + requested material names and material state after the engine load job, run with `AXL_SUPPRESS_BUILTIN=1` and `=0` | 1: ❌ / 0: ✅ | — | — | 1: **Invisible** / 0: **Visible** | **Smoking gun.** Same request (`black`, `latex_black_v1`, …). Built-in active: material loads as `CMaterialInstance` with `base=engine\materials\metal_base.remt` / `multilayered.mt`. Built-in suppressed: the **same material comes back as a bare `CMaterialTemplate`** (unresolved). Exception: `s1_nd_aquelyras_neozima_boots` resolves correctly in both runs. So in 2.5 the engine's own `LoadMaterialsAsync` no longer fully resolves certain *static* mesh materials; the built-in DynamicMesh finishes that job. Logs: `ArchiveXL-2026-10-29-02-57-00.log` (=1), `ArchiveXL-2026-10-29-02-59-25.log` (=0). |
| **Test E4** | As E3 + material entry dump (local/external, index, buffer sizes) and the template's own resource path | 1 | — | — | **Invisible** | **Root cause candidate.** All materials are local (`external=0`, buffer headers = entries). Failing meshes (belt, punk shoes) request a chunk material **`material_01_multilayer` that has no entry in the mesh** (`NOT FOUND`); the working neozima boots have it (entry 0). For the failing meshes *every* material in the request, including valid ones such as `black` (entry 13) and `latex_black_v1` (entry 0), comes back as `engine\materials\fallback.remt`. Conclusion: in 2.5 one missing chunk material makes `LoadMaterialsAsync` fall back for the whole request; the built-in DynamicMesh compensates (in E3 `material_01_multilayer` became an empty `CMaterialInstance`, the rest resolved). Log: `ArchiveXL-2026-10-29-03-04-26.log`. |
| **Test F** | Fix: `MeshExtension::LoadMaterialsWithMissingEntries`, built-in suppressed. Toggle `AXL_FIX_MISSING_MATERIALS=0` disables it | 1 | — | — | **Visible** ✅ | **Root cause confirmed.** With the built-in service suppressed, belt, punk shoes and goth shoes all render. Log shows substitution of `material_01_multilayer` on exactly the affected meshes (belt, punk shoes L/R, goth shoes L/R); no new errors. Log: `ArchiveXL-2026-10-29-03-10-25.log`. |
| **Test G** | Broad regression test: many different clothing mods with the fix, built-in suppressed | 1 | — | — | **All visible** ✅ | **Passed.** Many clothing mods tested, no visual issues. The fix applied to 15 meshes, all with chunk materials that have no entry in the mesh: 12× `material_01_multilayer`, 1× `material_01_multilayer ` (trailing space), 1× `None` (fishnet tights), 1× `personal_slot_dec__MAT` (unresolved hash `6028804543938264933`). No new errors or warnings. Log: `ArchiveXL-2026-10-29-03-18-32.log`. |

---

## 3. Reverse Engineering Deep Dive: REDengine 2.5 Bridge Architecture

### 3.1 The Global Bridge Interface (`[0x143BCDBE0]`)
During engine initialization (`0x14087517F`), `Cyberpunk2077.exe` creates an instance of an internal bridge interface (VTable at `0x1436EBA40`) stored at VA `0x143BCDBE0`.
Throughout the engine, **59 call sites** invoke methods on this bridge:
```asm
mov rcx, cs:[0x143BCDBE0]
test rcx, rcx
jz normal_engine_path
mov rax, [rcx]
call qword ptr [rax + vtable_offset] ; Dispatches into wrapper 0x1423027C0..0x142304B10
```

### 3.2 Bridge Wrapper Pattern and Extension Getters
Each bridge wrapper method (`0x142303xxx`) retrieves its corresponding embedded extension instance via dedicated getter functions (`0x142300830`..`0x142300A60`):
```asm
push rbx
sub  rsp, 20h
xor  eax, eax
mov  rbx, rcx
mov  [rcx], rax
mov  [rcx+8], rax
mov  rcx, [ctrl_block_addr]   ; Control block pointer (e.g., [0x1439638A0])
test rcx, rcx
je   fallback                 ; If null -> return {nullptr, nullptr}
call 0x142305230              ; TryLock (validates refcount > 0)
test al, al
je   fallback
mov  rax, [instance_addr]     ; Instance pointer (e.g., [0x143963898])
mov  [rbx], rax
mov  rax, [ctrl_block_addr]
mov  [rbx+8], rax
mov  rax, rbx
add  rsp, 20h
pop  rbx
ret
```

When a getter returns `{nullptr, nullptr}`, the caller wrapper detects `instance == nullptr` and executes its fallback or no-op branch.

### 3.3 Exact Memory Slot Mapping in REDengine 2.5

The 8 internal global extension instances created during `Raw::ExtensionService::Bootstrap` (`0x14231F0A0`):

| Slot | Subsystem Name | Instance RVA | Control Block RVA | Getter VA | Bridge VTable Offsets | Purpose & Engine Interop |
| :---: | :--- | :---: | :---: | :---: | :--- | :--- |
| **1** | **GarmentExtension** | `0x3963858` | `0x3963860` | `0x142300880` | `0xE0, 0xD8, 0xE8, 0x100, 0x108, 0x110, 0x118, 0x120, 0x130` | `ProcessSkinnedMesh`, `AddItem`, `OnGameDetach` (Proven innocent in Test C1) |
| **2** | **MeshExtension** | `0x3963898` | `0x39638A0` | `0x142300970` | `0x68, 0x70, 0x78, 0xA0` | **`CMesh::FindAppearance`, `LoadMaterialsAsync`, `ShouldPreload` (ROOT CAUSE)** |
| **3** | Slot_830 | `0x39638A8` | `0x39638B0` | `0x142300830` | `0x40, 0x48, 0x50, 0x58, 0x60, 0x88` | Resource / Index helper |
| **4** | Slot_9C0 | `0x3963878` | `0x3963880` | `0x1423009C0` | `0x90, 0x98, 0xB0, 0xB8, 0xC0, 0xC8` | Internal helper |
| **5** | Slot_8D0 | `0x39638C8` | `0x39638D0` | `0x1423008D0` | `0x28, 0x30` | Internal helper |
| **6** | Slot_920 | `0x39638E8` | `0x39638F0` | `0x142300920` | `0x140, 0x148, 0x150, 0x158, 0x160, 0x168` | Internal helper |
| **7** | Slot_A10 | `0x3963888` | `0x3963890` | `0x142300A10` | `0x80` | Internal helper |
| **8** | Slot_A60 | `0x39638D8` | `0x39638E0` | `0x142300A60` | `0x18, 0x20` | Internal helper |

---

## 4. Disassembly Analysis: Why `MeshExtension` Is Required

### 4.1 Inlined Call in `CMesh::FindAppearance` (`0x1404B2258`)
In `Cyberpunk2077.exe`:
```asm
00000001404B2281: cmp rbx, rcx
00000001404B2284: je  00000001404B22C5   ; Not in static appearances list
00000001404B2286: mov rax, [rbx]
00000001404B228E: cmp [rax+30h], rdi     ; Compare appearance name
00000001404B2292: je  00000001404B229A   ; Appearance matched!
...
00000001404B229A: mov rcx, [0x143BCDBE0] ; Check bridge
00000001404B22A1: test rcx, rcx
00000001404B22A4: je  00000001404B2360   ; Return static appearance directly
00000001404B22AA: mov rax, [rcx]
00000001404B22AD: lea r9, [rsp+30h]      ; &appearanceName
00000001404B22B2: mov r8, rsi            ; aMesh
00000001404B22BA: mov rdx, rbx           ; Handle<meshMeshAppearance>&
00000001404B22BD: call [rax+68h]         ; Bridge vfunc[+0x68] -> 0x142303500
```

Inside `0x142303500` (`vfunc[+0x68]`):
```asm
0000000142303515: call 0x142300970       ; Query embedded MeshExtension
000000014230351B: cmp  qword ptr [rsp+20h], 0
0000000142303521: je   000000014230352F   ; If null -> NO-OP FALLBACK
0000000142303529: call 0x14232A8A0       ; MeshExtension::ProcessAppearance
```
At `0x14232A8A0`, the function checks for `@` (`TemplateMaterialMarker = 0x40`) in the appearance chunk materials, resolves dynamic context suffixes (e.g. `_1` for flat shoes or `_2` for high heels), and constructs the expanded chunk materials list.

### 4.2 Inlined Call in `CMesh::LoadMaterialsAsync` (`0x1420B3430`)
At offset `+0x595` (`0x1420B39C5`):
```asm
mov rcx, [0x143BCDBE0]
mov rax, [rcx]
call qword ptr [rax+70h]                 ; Bridge vfunc[+0x70] -> 0x142303820
```
Inside `0x142303820`:
If `MeshExtension` (`0x142300970`) is present, it calls `0x14232AC30`:
1. It validates `materialNames.Size() == aToken.data->materials.Size()`.
2. It calls `0x1423299E0` (`HasUnresolvedMaterials`).
3. It allocates a `JobQueue` (`0x140409838`) and dispatches an asynchronous job to instantiate and resolve all dynamic material instances.

When `MeshExtension` is suppressed:
`0x142303820` immediately returns `0` (`xor ebx, ebx`). The unresolved dynamic material references remain uninstantiated, leaving the mesh invisible.

---

## 5. Falsified Hypotheses & Disproven Leads

During investigation, three earlier hypotheses were rigorously disproven:

### ❌ Falsified Lead 1: `gameItemObject` VTable Offset `0x280` vs. `0x288`
- **Initial Hypothesis:** `GetConnectedItem()` moved from offset `0x288` to `0x280` in Patch 2.5.
- **Disproof:** Disassembly of both offsets revealed:
  - `0x280` (`0x140196970`): returns `*(uint8_t*)(this + 0x26C) == 1` (a boolean flag, 0 for clothing items).
  - `0x288` (`0x140196E50`): returns `(*(uint8_t*)(this + 0x26C) - 2 <= 1)` (a boolean flag, 1 for equipped items).
  Neither offset writes a `CName` into `aOut`. In commit `51d88b1`, visual tags worked because `IsVisualTagActive` read `itemObject->appearanceName` directly from the struct field before falling back to the vfunc.

### ❌ Falsified Lead 2: Fallback in `ProcessSkinnedMesh` (`0x142133420` vs. `0x142325230`)
- **Initial Hypothesis:** The engine fallback `0x142133420` executed when `GarmentExtension` was suppressed handled replaced tokens incorrectly.
- **Disproof:** Static disassembly demonstrated that `0x142133420` is a bitwise identical clone of the native body `0x140947989`. Furthermore, Test C1 proved that completely nullifying `GarmentExtension` (`0x3963858`) results in garments rendering 100% correctly.

### ❌ Falsified Lead 3: 8× Retry Loop in `ProcessGarmentMesh`
- **Initial Hypothesis:** The 8× calls to `ProcessGarmentMesh` spaced ~70 ms apart in the log indicated an engine retry failure.
- **Disproof:** Log analysis of Test A (`ArchiveXL-2026-10-29-00-33-34.log`) showed the exact same 8× pattern occurring when items were completely visible. It is normal engine behaviour (or an artifact of `LoadResourceToken` forced checks), not an indicator of rendering failure.

---

## 6. Implementation & Resolution Strategy

### 6.1 ~~Clean Upstream Integration~~ (Superseded by Test F, see section 0)

> [!NOTE]
> Re-enabling the built-in service works, but only because the built-in DynamicMesh masks undefined chunk materials. The fix from Test F addresses the actual cause and keeps the built-in service suppressed as upstream intends. Kept here for reference.
To permanently resolve the regression without conflict:
1. **Allow REDengine's Built-in ExtensionService to Bootstrap:**
   In [ExtensionService.cpp](src/App/Extensions/ExtensionService.cpp), set `AXL_SUPPRESS_BUILTIN_EXTENSIONS = 0`.
2. **Built-in & External Coexistence:**
   In REDengine 2.5, CDPR natively incorporated the Dynamic Mesh appearance resolution pipeline directly into `Cyberpunk2077.exe`:
   - Slot 2 extension name: `"DynamicMesh"` (string literal at RVA `0x36EE650`).
   - Slot 2 VTable at RVA `0x36EE1F8` points to `OnFindAppearance` (`0x232A8A0`) and `OnLoadMaterials` (`0x232AC30`).
   Allowing REDengine's native `DynamicMesh` extension to run restores full material and appearance resolution for modded items without breaking engine subsystems.
   External ArchiveXL hooks continue providing extended functionality (e.g. world streaming sectors, attachment fixes, custom resource links, localization, factory indexing).
3. **Safe Bisection Toggle:**
   The bisection logic introduced in [ExtensionService.cpp](src/App/Extensions/ExtensionService.cpp) is maintained with default `AXL_BISECT_DEFAULT "NONE"`, allowing future diagnostic inspection via the `AXL_BISECT` environment variable without requiring recompilation.

---

### 6.2 Test D (Bridge VTable Hook): Abandoned
- **Concept:** Keep `Raw::ExtensionService::Bootstrap` suppressed and overwrite the Bridge VTable (`vfunc[+0x68]`, `+0x70`, `+0x78`) at RVA `0x36EBA40` to route calls into external `App::MeshExtension`.
- **Outcome:** Items invisible, 0 bridge calls logged. The code has been removed.
- **Most likely cause:** every bridge call site loads the global bridge pointer and checks it (`test rcx, rcx` / `jz fallback`). Without `Bootstrap` that pointer stays `nullptr`, so no call ever dispatches through the VTable and the patch is unreachable.
- **Caveats (unverified / contradictory in earlier notes):**
  - Bridge global is given as `0x143BCDBE0` (section 3.1) and as RVA `0x4334610`; `Bootstrap` as `0x14231F0A0` (section 3.3) and as RVA `0x2224D70`, which is also labelled `GetExtensionBridge()`. Call site count: 59 vs. "70+". These need to be reconciled by logging both globals in Test A and Test B.
  - Section 4.2 shows no null check before `call [rax+70h]`; if the bridge were really `nullptr` in Test B without such a check, the game would crash rather than render invisibly.
- **Why it was dropped:** even with a live bridge, routing it into `App::MeshExtension` would double-process meshes, because `App::MeshExtension` already hooks `CMesh::FindAppearance` / `LoadMaterialsAsync` directly. The real question is why those direct hooks do not work on 2.5 (Test E).
- **Git history:** `205accc` *Support patch 2.5* → `e93ba2f` *Re-enable built-in extension service* (26 Oct) → `1417f40` *Disable built-in extension service* (28 Oct). The reason for the final disable is undocumented; re-enabling it (6.1) should not be considered final until that reason is known.

### 6.3 Test E: Tracing the External `App::MeshExtension` (Option A)
**Hypothesis:** with the built-in service suppressed (upstream's intended setup), ArchiveXL's own hooks on `CMesh::FindAppearance`, `CMesh::LoadMaterialsAsync` and `CMesh::PostLoad` (via `ResourcePatchExtension` → `PrepareMeshState`) should resolve dynamic materials. Since items are invisible in Test B, one of these steps fails silently on 2.5.

**Runtime switches (no recompile needed):** read from the process environment first, then from `red4ext/plugins/ArchiveXL/axl_debug.ini` (`KEY=VALUE` per line). Use the file when launching via Steam, which does not forward console environment variables. The resolved file path is logged at startup (`[TruthLogger] Debug settings file: ...`).
| Env variable | Effect |
| :--- | :--- |
| `AXL_SUPPRESS_BUILTIN=1` | Suppresses the built-in ExtensionService (overrides the compile-time default `AXL_SUPPRESS_BUILTIN_EXTENSIONS`). |
| `AXL_MESH_TRACE=<substring>` | Logs `[MeshTrace]` lines for every mesh whose path contains `<substring>` (e.g. `aquelyras`). `*` traces all dynamic meshes (noisy; use if the path only resolves to a hash). |
| `AXL_BISECT=<mode>` | Unchanged; only active when the built-in service is not suppressed. |
| `AXL_FIX_MISSING_MATERIALS=0` | Disables the Test F fix (enabled by default). Each mesh the fix applies to is logged once as `Mesh "..." requests undefined chunk materials [...]`, independent of `AXL_MESH_TRACE`. |

The compile-time default of `AXL_SUPPRESS_BUILTIN_EXTENSIONS` is now `1` (upstream behaviour), since the fix makes the built-in service unnecessary.

**Trace points and what each failure means:**
| Log line | Failure signature | Interpretation |
| :--- | :--- | :--- |
| `PrepareMeshState ... dynamic=` | Line missing, or `dynamic=false` | `CMesh::PostLoad` hook not firing on 2.5, or the mesh is not detected as dynamic → no `MeshState`. |
| `OnFindAppearance ...` | Line missing | `CMesh::FindAppearance` hook not reached (wrong address / function replaced in 2.5). |
| `-> SKIPPED: no mesh state` | — | `PrepareMeshState` did not run for this mesh. |
| `-> processed, chunks=[...]` | Chunk names lack the expected `appearance@template` expansion | Expansion logic broken. |
| `OnLoadMaterials ... tokenMaterials= unresolved=` | Line missing | `CMesh::LoadMaterialsAsync` hook not reached. |
| `-> EARLY RETURN` | `names != tokenMaterials` or `unresolved=false` | Likely `MeshMaterialsToken` layout or `LoadMaterialsAsync` signature changed in 2.5. |
| `-> job ABORTED: no ... mesh state` | — | Mesh state lookup fails at job time. |
| `-> job running` but still invisible | Check for `[DynamicMesh]` errors right after | Failure inside `ProcessDynamicMaterials`. |

**Procedure:**
1. Put `AXL_SUPPRESS_BUILTIN=1` and `AXL_MESH_TRACE=aquelyras` into `axl_debug.ini`, launch the game, equip belt + shoes, collect `ArchiveXL-*.log`.
2. Repeat with `AXL_SUPPRESS_BUILTIN=0` (same trace filter) as the visible reference and diff the `[MeshTrace]` sequences.

# ArchiveXL Regression & Root Cause Analysis on Cyberpunk 2077 Patch 2.5

## 1. Executive Summary & Definitive Root Cause

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
| **Test D (Sol. 1)**| Bridge VTable Hook with `AXL_SUPPRESS_BUILTIN_EXTENSIONS=1` | ❌ Intercepted (no-op) | ❌ Inactive (`nullptr`) | ❌ Inactive (`nullptr`) | **Invisible** | **Bridge VTable hook never called.** `Bootstrap` at RVA `0x2224D70` initializes the global Bridge instance at RVA `0x4334610`; suppressing `Bootstrap` leaves the Bridge uninitialized across 70+ engine subsystems. |

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

### 6.1 Clean Upstream Integration (Definitive Resolution)
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

### 6.2 Post-Mortem on Solution 1 (Bridge VTable Hook)
- **Concept:** Keep `Raw::ExtensionService::Bootstrap` suppressed (`AXL_SUPPRESS_BUILTIN_EXTENSIONS = 1`) and overwrite the Bridge VTable (`vfunc[+0x68]`, `vfunc[+0x70]`, `vfunc[+0x78]`) at RVA `0x36EBA40` to route calls into external `App::MeshExtension`.
- **Outcome:** **Failed (In-game items invisible, 0 Bridge calls logged).**
- **Disassembly Analysis:**
  1. In `Cyberpunk2077.exe`, `Raw::ExtensionService::Bootstrap` (entry at RVA `0x2224D70`) executes `call 0x2301410`, which initializes the Bridge and writes the Bridge pointer to global instance `0x4334610`.
  2. Over 70 engine subsystems call `0x2224D70` (`GetExtensionBridge()`).
  3. When `Bootstrap` is no-op intercepted, `0x4334610` remains an uninitialized NULL structure. Consequently, engine callers never execute indirect virtual dispatch on the Bridge, and no calls ever reached the hooked VTable.
- **Git History Insight:**
  In psiberx's repository:
  - Commit `205accc`: *Support patch 2.5* (introduced Bootstrap suppression).
  - Commit `e93ba2f`: *Re-enable built-in extension service* (removed Bootstrap suppression — everything worked).
  - Commit `1417f40` (v1.28.2): *Disable built-in extension service* (re-introduced Bootstrap suppression — caused the Patch 2.5 Dynamic Mesh regression).
- **Conclusion:**
  Suppressing `Raw::ExtensionService::Bootstrap` is inherently destructive to REDengine 2.5's subsystem architecture. Keeping `AXL_SUPPRESS_BUILTIN_EXTENSIONS = 0` is the single architecturally correct resolution.

# ArchiveXL on Cyberpunk 2077 Patch 2.5: Invisible Clothing Root Cause Analysis

## 1. Summary

**Symptom:** with ArchiveXL 1.28.2 on patch 2.5, some clothing items render completely invisible, for example `nd_aquelyras_goth_belt` and `nd_aquelyras_punk_shoes`. Other items, e.g. `axellysse_stars_belt`, render normally. The issue appears with upstream commit `1417f40` (*Disable built-in extension service*).

**Root cause (confirmed, Tests E4 and F):**
- The affected mod meshes contain chunk materials that have **no entry in the mesh's `materialEntries`**, most commonly `material_01_multilayer`. This is an authoring error that had no visible effect before 2.5.
- Since patch 2.5, `CMesh::LoadMaterialsAsync` then returns `engine\materials\fallback.remt` for **every** material in the request, including valid ones. The whole mesh renders invisible.
- The game's built-in DynamicMesh extension compensates for this: it leaves only the undefined slot empty. That is why the bug only appears when the built-in extension service is suppressed (Test B, upstream `1417f40`) or only its mesh slot is removed (Test C2).
- The affected meshes are **static**, not ArchiveXL-dynamic (no `@` markers). ArchiveXL's own `MeshExtension` therefore never touched them.

**Fix (verified in-game, Tests F and G):** `App::MeshExtension::LoadMaterialsWithMissingEntries` substitutes undefined names with a valid one before calling the engine, then puts an empty material into the undefined slots. The built-in service stays suppressed, as upstream intends. Each affected mesh is logged once.

**Branches:**
| Branch | Content |
| :--- | :--- |
| `research/root-cause-analysis` | Fix plus all research tooling (switches, tracing, bisection) and these notes. |
| `fix/undefined-chunk-materials` | Fix only, based on `upstream/main` (`83e1ec2`), intended for an upstream PR. Log message: `Mesh "..." uses undefined chunk materials [...], they will be rendered empty.` (warning). |

---

## 2. Test Matrix

"Built-in" = the game's built-in extension service (`Raw::ExtensionService::Bootstrap`).

| Test | Configuration | Built-in | Result | Finding |
| :--- | :--- | :---: | :---: | :--- |
| **Reference** (`51d88b1`) | Fork baseline, built-in re-enabled | Active | Visible | Belt and shoes render. |
| **A** | `AXL_SUPPRESS_BUILTIN_EXTENSIONS=0` | Active | Visible | Issue only occurs when the built-in service is suppressed. |
| **B** | `AXL_SUPPRESS_BUILTIN_EXTENSIONS=1` (upstream `1417f40`) | Suppressed | Invisible | Reproduces the bug. |
| **C1** | Bisection: null the built-in Garment slot only | Active | Visible | The built-in Garment extension is not involved. |
| **C2** | Bisection: null the built-in Mesh (DynamicMesh) slot only | Active | Invisible | The built-in DynamicMesh extension is what keeps the items visible. (It compensates for the bug; it does not cause it, see E4.) |
| ~~**D**~~ | Bridge VTable hook, built-in suppressed | Suppressed | Invisible | **Abandoned**, see section 5. |
| **E** | `[MeshTrace]` on ArchiveXL's `MeshExtension` | Suppressed | Invisible | All ArchiveXL hooks fire (`PostLoad`, `FindAppearance`, `LoadMaterialsAsync`). `PrepareMeshState` reports `dynamic=false` → no `MeshState` → `OnFindAppearance` skips, material job aborts. Log `ArchiveXL-2026-10-29-02-48-27.log`. |
| **E2** | E + name/hash dump + RTTI layout check | Suppressed | Invisible | RTTI offsets match the SDK, CNames resolve. Meshes contain no `@` markers; `chunks=44` is the real submesh count. `dynamic=false` is **correct**. Log `…-02-53-10.log`. |
| **E3** | E2 + requested material names and material state after the engine load job; run with built-in suppressed and active | Both | Invisible / Visible | Same request in both runs. Built-in active: `black`, `latex_black_v1` load as `CMaterialInstance` (`metal_base.remt` / `multilayered.mt`). Built-in suppressed: the same materials come back as `CMaterialTemplate`. Neozima boots resolve in both runs. Logs `…-02-57-00.log` (suppressed), `…-02-59-25.log` (active). |
| **E4** | E3 + material entry dump | Suppressed | Invisible | **Root cause.** All materials are local. Belt and punk shoes request `material_01_multilayer`, which has **no entry** in those meshes; neozima boots have it. For belt and punk shoes, **every** material, including valid ones (`black` = entry 13, `latex_black_v1` = entry 0), comes back as `engine\materials\fallback.remt`. Log `…-03-04-26.log`. |
| **F** | Fix enabled | Suppressed | **Visible** | Belt, punk shoes, goth shoes render. Substitution logged exactly for the affected meshes, no new errors. Log `…-03-10-25.log`. |
| **G** | Broad test with many clothing mods, fix enabled | Suppressed | **All visible** | No visual issues. Fix applied to 15 meshes, all with undefined chunk materials (see 2.1). No new errors. Log `…-03-18-32.log`. |
| **G2** | Clean build of `fix/undefined-chunk-materials` | Suppressed | **Visible** | Same behaviour, warning logged per affected mesh, no errors. Log `…-03-38-47.log`. |

### 2.1 Undefined Chunk Materials Seen in Tests G/G2
| Undefined name | Meshes (examples) |
| :--- | :--- |
| `material_01_multilayer` | ND / Aquelyras: goth belt, goth necklace, punk shoes, goth shoes, chic heels, office lady skirt + corset, combat vest, street top, girly bag, absolute mini jacket + skirt, casual shorts, plaid mini skirt, beach top |
| `material_01_multilayer ` (trailing space) | goth shoes L, chic heels L |
| `None` (empty name) | ND fishnet tights, stockings collection, alt pants, alt pants 2, beach top |
| `leo` | `axellysse\ankle_boots\axellysse_ankle_boots.mesh` |
| `gold` | `axellysse\sweet_outfit_dress\sweet_stocking_base_body_high_heels.mesh` |
| `personal_slot_dec__MAT` | unresolved path hash `6028804543938264933` |

---

## 3. Research Switches & Tracing (research branch only)

Every switch is read from the process environment first, then from `red4ext/plugins/ArchiveXL/axl_debug.ini` next to `ArchiveXL.dll`. Use the file when launching via Steam, which does not forward console environment variables. Format: one `KEY=VALUE` per line, `#` starts a comment. The resolved file path is logged at startup as `[TruthLogger] Debug settings file: ...`. Implementation: `src/App/Utils/DebugSettings.hpp`.

| Switch | Default | Effect |
| :--- | :--- | :--- |
| `AXL_SUPPRESS_BUILTIN` | `1` | `1`: suppress the built-in extension service (upstream behaviour). `0`: let it bootstrap. The compile-time default is the macro `AXL_SUPPRESS_BUILTIN_EXTENSIONS` in `ExtensionService.cpp`. |
| `AXL_BISECT` | `NONE` | Only used when `AXL_SUPPRESS_BUILTIN=0`. After the built-in bootstrap, dumps the 8 built-in extension slots and nulls the selected ones: `GARMENT`, `MESH`, `GARMENT_AND_MESH`, `ALL_EXCEPT_GARMENT`, `ALL_EXCEPT_MESH`, `ALL`, `NONE`. Compile-time default: `AXL_BISECT_DEFAULT`. Slot addresses are only valid for the 2.5 executable they were taken from. |
| `AXL_FIX_MISSING_MATERIALS` | `1` | `0` disables the fix, for A/B comparisons. When enabled, each affected mesh is logged once as `[DynamicMesh] Mesh "..." requests undefined chunk materials [...], substituted with "...".` |
| `AXL_MESH_TRACE` | empty (off) | Logs `[MeshTrace]` lines for every mesh whose resolved path contains the given substring (e.g. `aquelyras`). `*` traces **all** meshes (very noisy, but needed if a path only resolves to a hash). At startup it also logs an RTTI layout check (`Layout ... runtime=... compiled=...`, `<-- MISMATCH` on difference). |

Example `axl_debug.ini`:
```ini
AXL_SUPPRESS_BUILTIN=1
AXL_FIX_MISSING_MATERIALS=1
# AXL_MESH_TRACE=aquelyras
# AXL_BISECT=NONE
```

### 3.1 `[MeshTrace]` Lines
| Line | Meaning |
| :--- | :--- |
| `PrepareMeshState "..." dynamic=… materialEntries=… appearances=…` + `material[i]`, `appearance[i]`, `chunk[j]` dumps | Mesh post-load. Without `dynamic=true` no `MeshState` is created and the dynamic pipeline ignores the mesh. |
| `OnFindAppearance "..." app=… meshState=… chunks=…` → `processed, chunks=[…]` / `SKIPPED: no mesh state` | Appearance lookup and chunk expansion. |
| `OnLoadMaterials "..." names=… tokenMaterials=… unresolved=…` + `requested=[…]` | Material load request, state right after the engine call (materials may still be loading). |
| `EARLY RETURN` / `dispatching dynamic material job` | Whether the dynamic material job is scheduled. |
| `job ABORTED: no target/source mesh state` / `job running` | Outcome of the dynamic material job. |
| `mesh "..." entries=… external=… localInstances=… localBufferHeaders=…` + `entry "..." at … local=… index=…` / `NOT FOUND` | Material entries for each requested name, after the engine load job. |
| `after-load "..." [i] "name" = <class> self=<path> base=<path> unresolved=…` | Final material per slot after the engine load job. `self=engine\materials\fallback.remt` indicates the 2.5 fallback. |

---

## 4. Background: The Game's Built-in Extension Service

> [!WARNING]
> Reverse-engineering notes, **not verified at runtime**. Addresses refer to one specific 2.5 executable. Some earlier notes contradicted each other (see 4.4). They are kept as a starting point, not as facts.

### 4.1 Bridge Interface
- Patch 2.5 contains an internal extension framework with a global bridge object (pointer reported at VA `0x143BCDBE0`, VTable at `0x1436EBA40`).
- Engine call sites load the pointer, check it for null and call a VTable slot:
  ```asm
  mov rcx, cs:[0x143BCDBE0]
  test rcx, rcx
  jz normal_engine_path
  mov rax, [rcx]
  call qword ptr [rax + vtable_offset]
  ```
- Each bridge method fetches its extension instance through a getter that returns `{nullptr, nullptr}` when the instance or its control block is missing; the method then takes a fallback path.

### 4.2 Built-in Extension Slots (as dumped by `AXL_BISECT`)
| Slot | Name | Instance RVA | Control block RVA | Getter VA | Bridge VTable offsets |
| :---: | :--- | :---: | :---: | :---: | :--- |
| 1 | Garment | `0x3963858` | `0x3963860` | `0x142300880` | `0xD8, 0xE0, 0xE8, 0x100, 0x108, 0x110, 0x118, 0x120, 0x130` |
| 2 | Mesh ("DynamicMesh") | `0x3963898` | `0x39638A0` | `0x142300970` | `0x68, 0x70, 0x78, 0xA0` |
| 3 | Slot_830 | `0x39638A8` | `0x39638B0` | `0x142300830` | `0x40, 0x48, 0x50, 0x58, 0x60, 0x88` |
| 4 | Slot_9C0 | `0x3963878` | `0x3963880` | `0x1423009C0` | `0x90, 0x98, 0xB0, 0xB8, 0xC0, 0xC8` |
| 5 | Slot_8D0 | `0x39638C8` | `0x39638D0` | `0x1423008D0` | `0x28, 0x30` |
| 6 | Slot_920 | `0x39638E8` | `0x39638F0` | `0x142300920` | `0x140 … 0x168` |
| 7 | Slot_A10 | `0x3963888` | `0x3963890` | `0x142300A10` | `0x80` |
| 8 | Slot_A60 | `0x39638D8` | `0x39638E0` | `0x142300A60` | `0x18, 0x20` |

Runtime dump with the built-in service active (Test A): all slots populated except Slot_9C0 (`0x0`).

### 4.3 Mesh Hooks in the Engine
- `CMesh::FindAppearance` calls bridge `vfunc[+0x68]` (wrapper `0x142303500` → `0x14232A8A0`) after an appearance match.
- `CMesh::LoadMaterialsAsync` calls bridge `vfunc[+0x70]` (wrapper `0x142303820` → `0x14232AC30`). That function checks `materialNames.Size() == token materials`, checks for unresolved materials and dispatches a job. This matches the observed behaviour in E3: with the built-in active, undefined slots end up empty and the remaining materials resolve.

### 4.4 Known Inconsistencies
- The bridge global was also reported as RVA `0x4334610`, and `Bootstrap` both as `0x14231F0A0` and as RVA `0x2224D70` (the latter also labelled `GetExtensionBridge()`). Call site counts of 59 and "70+" were given.
- The `LoadMaterialsAsync` snippet shows no null check before `call [rax+70h]`. If the bridge pointer were null with the built-in suppressed and no check existed, the game would crash instead of rendering invisibly.
- Not reconciled, because the fix does not depend on these internals.

---

## 5. Falsified Hypotheses & Abandoned Approaches

### ❌ "The built-in DynamicMesh resolves `@` templates that ArchiveXL no longer resolves"
Original hypothesis of this investigation. Disproved by E2/E4: the affected meshes contain no `@` markers. The built-in extension masks undefined chunk materials; it does not provide dynamic template resolution these meshes need.

### ❌ Re-enabling the built-in service as the fix
Works (Test A) but only masks the bug, and reverts upstream's deliberate `1417f40`. Upstream history: `205accc` *Support patch 2.5* → `e93ba2f` *Re-enable built-in extension service* (26 Oct) → `1417f40` *Disable built-in extension service* (28 Oct). Superseded by the fix (Test F).

### ❌ Test D: Bridge VTable Hook
- **Idea:** keep the built-in suppressed and redirect bridge `vfunc[+0x68/+0x70/+0x78]` into ArchiveXL's `MeshExtension`.
- **Result:** invisible, 0 bridge calls logged. Code removed.
- **Likely reason (unverified):** without `Bootstrap` the bridge pointer stays null, so every call site takes its `jz` fallback and never reaches the VTable.
- **Also flawed by design:** ArchiveXL's `MeshExtension` already hooks the same `CMesh` functions directly, so routing the bridge into it would process meshes twice.

### ❌ Shifted struct layout / unreadable CNames in 2.5
Suspected after E showed `chunks=44` for every mesh. Disproved by E2: RTTI offsets of `meshMeshAppearance::name/chunkMaterials/tags` and `CMesh::appearances/materialEntries` match the SDK, names resolve, 44 is the real submesh count.

### ❌ `gameItemObject` VTable Offset `0x280` vs. `0x288`
- **Hypothesis:** `GetConnectedItem()` moved from `0x288` to `0x280` in 2.5.
- **Disproof:** `0x280` (`0x140196970`) returns `*(uint8_t*)(this + 0x26C) == 1`, `0x288` (`0x140196E50`) returns `(*(uint8_t*)(this + 0x26C) - 2 <= 1)`. Neither writes a `CName`. Visual tags work in `51d88b1` because `IsVisualTagActive` reads `itemObject->appearanceName` directly before falling back to the vfunc.

### ❌ Fallback in `ProcessSkinnedMesh` (`0x142133420`)
- **Hypothesis:** the engine fallback used without the built-in Garment extension mishandles replaced tokens.
- **Disproof:** `0x142133420` is a byte-identical clone of the native body at `0x140947989`, and Test C1 shows garments render with the Garment slot nulled.

### ❌ 8× `ProcessGarmentMesh` calls as a retry loop
The same 8× pattern (~70 ms apart) appears in Test A with visible items (`ArchiveXL-2026-10-29-00-33-34.log`). Normal engine behaviour, not a failure indicator.

---

## 6. Open Questions
- Why `LoadMaterialsAsync` fails the whole request in 2.5 instead of only the undefined slot is inferred from observation (3 meshes in E4, 15+ in G), not from disassembly.
- Upstream's reason for `1417f40` is undocumented.
- Mesh hash `6028804543938264933` (`personal_slot_dec__MAT`) is not identified; it may be a game asset or another mod.

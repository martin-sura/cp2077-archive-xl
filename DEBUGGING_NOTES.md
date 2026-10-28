# ArchiveXL Patch 2.5 Regression: Debugging Results & Resolution

## 1. Executive Summary

Following the release of **ArchiveXL 1.28.3** for **Cyberpunk 2077 Patch 2.5**, multiple users reported that various custom clothing items—specifically dynamic appearance meshes including shoes (e.g. Nola Dreamer Goth shoes/heels), belts, and feet-state-dependent garments—completely failed to render in-game.

Through systematic in-game git bisect testing on Cyberpunk 2077 Patch 2.5, the precise root causes were isolated and resolved without losing any of the upstream 1.28.3 improvements (such as world node mutations, streaming sector fixes, or hair/beard expansion fixes).

---

## 2. Git Bisect Execution & Findings

A systematic step-by-step bisect was conducted to identify the exact commit responsible:

| Commit / Build | Description | In-Game Render Test | Outcome |
| :--- | :--- | :---: | :--- |
| `4694caa` | Baseline (1.28.2 CustomPatch) | **Rendered** | Known-good reference baseline |
| `60f66ae` | "Update shared lib and sdk" | **Rendered** | Validated working state before regression |
| `1417f40` | "Disable built-in extension service" | **FAILED** (Invisible) | **Root Cause Introduced** |
| `83e1ec2` | Upstream `v1.28.3` Release HEAD | **FAILED** (Invisible) | Unfixed upstream release state |
| **Final Fix** | Clean fix on top of `83e1ec2` | **Rendered** | 100% verified working in-game |

---

## 3. Detailed Technical Root Causes

### 3.1 Root Cause 1: Neutralization of REDengine's Built-in Extension Service (`1417f40`)

In commit `1417f40`, `src/App/Extensions/ExtensionService.cpp` introduced three no-op hooks intercepting REDengine's internal extension system:

```cpp
Hook<Raw::ExtensionService::Bootstrap>(+[](void*) {}).OrThrow();
Hook<Raw::ExtensionService::Shutdown>(+[](void*) {}).OrThrow();
Hook<Raw::ExtensionLoader::Configure>(+[](void*) {}).OrThrow();
```

* **Impact:** In Cyberpunk 2077 Patch 2.5, CDPR's internal `gameExtensionService` and `gameExtensionLoader` manage extension registration and resource configuration. Neutering these functions with empty lambdas completely stops REDengine from configuring and bootstrapping extensions. Consequently, dynamic garment mesh loading and component registration fail silently in-game.
* **Resolution:** Removed the three no-op hooks so REDengine's extension service runs normally as intended.

---

### 3.2 Root Cause 2: Erroneous VTable Offset for `GetAppearanceName` (`6d56712`)

In commit `6d56712`, `src/Red/ItemObject.hpp` altered the virtual function offset:

```diff
 namespace Raw::ItemObject
 {
 using ItemID = Core::OffsetPtr<0x288, Red::ItemID>;
 
 constexpr auto GetAppearanceName = Core::RawVFunc<
-    /* offset = */ 0x280,
+    /* offset = */ 0x288,
     /* type = */ Red::CName* (Red::ItemObject::*)(Red::CName& aOut)>();
 }
```

* **Impact:** Offset `0x288` is the struct member offset for `Red::ItemID` (`Core::OffsetPtr<0x288, Red::ItemID>`). In contrast, `0x280` (`vtable index 0x280 / 8 = 80`) has been the correct virtual method offset for `GetAppearanceName` since patch 2.0 / 2.1. Invoking offset `0x288` as a vtable pointer calls the wrong method index.
* **Resolution:** Reverted `GetAppearanceName` offset back to `0x280`.

---

### 3.3 Root Cause 3: Removal of Direct `appearanceName` Fallback in `IsVisualTagActive` (`a29c3eb`)

In commit `a29c3eb`, `src/App/Extensions/Attachment/Extension.cpp` was modified to eliminate reading `slotData->itemObject->appearanceName` directly:

```diff
-        if (slotData->itemObject)
-        {
-            auto itemAppearance = slotData->itemObject->appearanceName;
-            if (!itemAppearance || itemAppearance == EmptyAppearanceName)
-            {
-                Raw::ItemObject::GetAppearanceName(slotData->itemObject, itemAppearance);
-            }
-            return itemAppearance && itemAppearance != EmptyAppearanceName && ...
+        if (slotData->itemObject)
+        {
+            Red::CName itemAppearance;
+            Raw::ItemObject::GetAppearanceName(slotData->itemObject, itemAppearance);
+            return itemAppearance && itemAppearance != EmptyAppearanceName && ...
```

* **Impact:** For standard attached items, `slotData->itemObject->appearanceName` is already valid and cached. For dynamic appearances where the appearance might be evaluated lazily, falling back to `Raw::ItemObject::GetAppearanceName` is required. Completely removing the direct `slotData->itemObject->appearanceName` check created a fragile single point of failure.
* **Resolution:** Restored the hybrid evaluation: check `slotData->itemObject->appearanceName` first, and if empty or missing, query `Raw::ItemObject::GetAppearanceName(slotData->itemObject, itemAppearance)`.

---

## 4. Summary of Code Changes

### `src/App/Extensions/ExtensionService.cpp`
Removed the three hooks disabling CDPR's ExtensionService and unused header `#include "Red/ExtensionService.hpp"`.

### `src/Red/ItemObject.hpp`
Restored `GetAppearanceName` offset to `0x280`.

### `src/App/Extensions/Attachment/Extension.cpp`
Restored the robust hybrid check:
```cpp
if (slotData->itemObject)
{
    auto itemAppearance = slotData->itemObject->appearanceName;
    if (!itemAppearance || itemAppearance == EmptyAppearanceName)
    {
        Raw::ItemObject::GetAppearanceName(slotData->itemObject, itemAppearance);
    }

    return itemAppearance && itemAppearance != EmptyAppearanceName &&
           aTransactionSystem->MatchVisualTag(slotData->itemObject, aVisualTag, false);
}
```

---

## 5. Build & Deployment Artifacts

* **Live DLL:** `red4ext/plugins/ArchiveXL/ArchiveXL.dll` (SHA256: `4a1e626842a16168963dccd095ca27e68e98a0d792beb4958f498f9b99e34d34`)
* **Vortex Mod Zip:** `ArchiveXL-1.28.3-FinalFix.zip` (SHA256: `93b6f1776c4fc47f7f48007f236b123ddcde1f9a85ae5287f3ff7c315ea78aef`)

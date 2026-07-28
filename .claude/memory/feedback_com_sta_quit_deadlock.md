---
name: feedback-com-sta-quit-deadlock
description: COM STA deadlock when calling Quit() after Cancel=true in BeforeClose — fix via RemoveEventHandler before Quit
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2151f1f7-cf54-405b-b323-fc008f69371c
---

When intercepting Word/Excel close events with `Cancel=true` (to keep the document open for our save logic), calling `wd.Quit()` / `exl.Quit()` afterwards causes a COM STA deadlock:

- `Quit()` blocks the UI thread waiting for Word/Excel to finish
- Word/Excel fires `DocumentBeforeClose` / `BeforeClose` which must be delivered back to the UI thread
- UI thread is blocked inside `Quit()` → circular deadlock
- A `programmaticClose` bool flag does NOT fix this — the handler is never reached

**Fix:** Use `ComAwareEventInfo.RemoveEventHandler(...)` to unsubscribe the handler BEFORE calling `Quit()`. Word/Excel fires BeforeClose but there is no handler to cancel, so it closes cleanly.

```csharp
// Word
try {
    new System.Runtime.InteropServices.ComAwareEventInfo(
        typeof(WdApp.ApplicationEvents4_Event), "DocumentBeforeClose")
        .RemoveEventHandler(wd, new WdApp.ApplicationEvents4_DocumentBeforeCloseEventHandler(wd_DocumentBeforeClose));
} catch (Exception) { }
try { if (wd != null) wd.Quit(); } catch (Exception) { }

// Excel
try {
    new System.Runtime.InteropServices.ComAwareEventInfo(
        typeof(XlApp.WorkbookEvents_Event), "BeforeClose")
        .RemoveEventHandler(wbk, new XlApp.WorkbookEvents_BeforeCloseEventHandler(wbk_BeforeClose));
} catch (Exception) { }
try { exl.Quit(); } catch (Exception) { }
```

Also: use `wd.Quit()` with NO arguments (not `wd.Quit(false)`) — the `false` parameter can throw exceptions in some Word versions. With `DisplayAlerts = wdAlertsNone` already set, Word won't prompt to save.

**Why:** COM STA re-entrancy via DoEvents/co-wait does NOT help when `Quit()` itself is the blocking call on the UI thread.

**How to apply:** Any time you need to programmatically close a Word/Excel instance that has a `Cancel=true` BeforeClose handler — always RemoveEventHandler first.

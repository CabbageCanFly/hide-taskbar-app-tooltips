# hide-taskbar-app-tooltips

A minimal Windhawk mod for Windows 11 x64 that hides application hover tooltips on the taskbar. Start, system tray, clock/status, tray overflow and unrelated shell tooltips remain visible. Taskbar thumbnail previews and hover/click interactions are preserved. No polling or background service is added.

## Installation

1. Disable the original **Hide Taskbar Tooltips** mod before loading this version. Never enable both together.
2. In [Windhawk](https://windhawk.net/), choose **Create a new mod** and replace the entire editor contents with [`hide-taskbar-app-tooltips-only.wh.cpp`](hide-taskbar-app-tooltips-only.wh.cpp).
3. Save and select **Compile Mod**, then exit editing mode. Compilation may load the mod immediately. No settings are needed.

To roll back, disable this custom mod. You can then re-enable the original if desired. The mod does not restart Explorer.

## Attribution and license

Based on [Hide Taskbar Tooltips v1.4.0 by gilnett](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/hide-taskbar-tooltips.wh.cpp). Modifications by [CabbageCanFly](https://github.com/CabbageCanFly) narrow suppression to positively identified application tooltips while preserving the original hover handlers.

Licensed under [GNU GPL version 3](LICENSE). Original attribution and the upstream source link are retained in the mod source.

## Known limitations

- Supports the native Windows 11 x64 taskbar. Windows updates or replacement taskbars may change the private symbols or classes it relies on.
- Unknown tooltips are intentionally allowed. If classification fails or the bounded registration cache evicts an entry, an application tooltip may remain visible.
- Classification diagnostics are logged once per Explorer process through Windhawk's debug logging.

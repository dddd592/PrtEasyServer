# Printer configuration: explicit Modify button

This update adds a dedicated **Modify selected / 修改所选项** button to the printer configuration page.

## How to use

1. Click a printer row in the list.
2. Click **Modify selected** (简体中文：修改所选项; 繁體中文：修改所選項目).
3. Edit the printer, port, or RAW/XPS mode fields above.
4. Click **Update row** to apply the changes to that entry.
5. Click **Save settings** to persist the configuration.

Selecting a row no longer silently enters edit mode. The explicit Modify button loads the selected row into the editor, helping avoid accidental updates.

## Files changed

- `src/main_window.h`: new control ID and HWND member.
- `src/main_window.cpp`: creates/layouts the button, handles clicks, and stops automatically entering edit mode when a list row selection changes.
- `src/localization.cpp`: English, Traditional Chinese, and Simplified Chinese labels.

The source has not been compiled or runtime-tested on Windows in this environment. Build using `build-windows10.cmd` from a Visual Studio Native Tools Command Prompt.

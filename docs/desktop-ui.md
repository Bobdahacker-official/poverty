# Desktop interaction and UI design

The desktop uses the existing Nuklear software-rendered interface, Catppuccin
palettes, DejaVu typography, and Lucide icon atlas. The file manager, monitor,
browser, and context menus share rounded surfaces, compact control padding,
accent-colored primary actions, muted secondary labels, and consistent spacing.

## Design references

- [Nuklear layout API](https://immediate-mode-ui.github.io/Nuklear/Layouting.html):
  proportional toolbars, scrollable content groups, and fixed-height controls.
- [Nuklear contextual menus](https://github.com/Immediate-Mode-UI/Nuklear/wiki/Contextual):
  file actions stay beside the selected item; menus close on selection or Escape.
- [GNOME header bars](https://developer.gnome.org/hig/patterns/containers/header-bars.html):
  navigation sits at the start of toolbars and secondary actions are grouped.
- [GNOME browsing](https://developer.gnome.org/hig/patterns/nav/browsing.html):
  consistent back/forward navigation and keyboard access to locations.
- [GNOME tabs](https://developer.gnome.org/hig/patterns/nav/tabs.html):
  a visible tab strip, descriptive labels, a distinct active tab, close controls,
  and standard new/close/switch shortcuts.
- [GNOME menus](https://developer.gnome.org/hig/patterns/controls/menus.html):
  action labels use verbs and related desktop actions remain together.

## State and filesystem behavior

Each browser tab has a fixed slot containing its document, JavaScript heap,
resources, source, address, and navigation history. Tab changes select the existing
context rather than parsing HTML again. Nuklear scroll keys include the tab slot;
they reset on navigation and slot reuse. The tab limit is eight.

Files keeps full paths for search results, so opening, renaming, copying, and
deleting a result acts on its actual containing folder. Text-field clipboard
actions and file clipboard actions are separate. Copied folders are recursive;
failed copies attempt to remove only the destination they created. A failed move
keeps the cut clipboard available. Existing destination items are never silently
replaced. Copy and search recursion are bounded to protect the kernel stack.

Regular ext4 copies stream through a 4 KiB buffer and preserve file mode bits.
Cross-directory moves update the directory entries, the moved directory's parent,
and parent link counts in the same transaction. Inode ancestry checks reject
moving a directory beneath itself, including paths containing dot components.

The monitor samples cumulative userspace timer ticks and matches individual
samples by PID. Reaped processes do not make the aggregate CPU counter decrease.
The kernel row represents the built-in desktop applications; userspace CPU is
identified explicitly because the scheduler does not distinguish kernel work
from idle time. Kernel memory is shown in the system total, not invented as an
independent allocation for each built-in application.

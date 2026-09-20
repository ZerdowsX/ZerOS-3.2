# Nugget OS

A hobby x86_64 kernel built from scratch: bootloader, memory management,
filesystem, TCP/IP network stack, and a graphical desktop with a real
window manager and a handful of working apps.

## What's implemented

- **`.socks` - an independent program format**: programs can now live as
  their own compiled file under `software/<name>/<name>.socks`, entirely
  separate from the kernel binary, instead of being baked directly into
  it. A small header (magic, version, offsets to init/draw/handle_key/
  handle_click, code size, name) is followed by raw machine code, loaded
  by the kernel at a fixed address and run directly - programs are still
  privileged/kernel-mode for now (real per-process isolation is a much
  bigger separate project), but they're genuinely independent files:
  drop a new folder with a `.socks` file in it under `software/` and it
  shows up in the Start menu next time it's opened; delete the folder
  and it's gone, no kernel rebuild needed. Programs never call kernel
  functions directly (those addresses shift every time the kernel is
  rebuilt) - they go through a fixed-address API table the kernel fills
  in at boot, the same idea as a syscall table or a DLL import table.
  `software-src/build_socks.py` compiles a `.c` file into a `.socks` file
  using the same gcc used to build the kernel itself, just linked at a
  different address - no new compiler needed. Notepad has been ported as
  the proof of concept (`software-src/notepad/notepad.c`), simplified for
  now (autosaves as you type instead of a Save/Save-As dialog).
  **Verified working**: booted, opened the Start menu, confirmed the
  discovered "notepad" entry appears above the built-in programs (in
  the right position, sized correctly for however many programs are
  found), clicked it, confirmed in the serial log that `notepad.socks`
  actually loaded from disk, and confirmed the window's content matches
  the compiled program's own drawing exactly (a distinctive
  gray-toolbar/white-body/black-text pattern that only that program's
  code produces) - not a fallback or something faked.
- **Intel e1000 network driver** - a real, third NIC option (tried after
  RTL8139, before PCNet) covering the 8254x family: what QEMU/VMware
  emulate by default, and what a lot of real business desktops/laptops
  and servers from the mid-2000s to early 2010s actually shipped with
  (a handful of common device IDs are matched, including 82574L which
  was extremely common on real motherboards). Unlike our older
  I/O-port-based RTL8139/PCNet drivers, this one uses real
  memory-mapped registers and descriptor rings, matching how actual
  modern-ish NICs work. **Verified working**: booted against QEMU's
  `-device e1000` and got a full DHCP handshake (DISCOVER→OFFER→ACK)
  over it, proving both TX and RX genuinely work.
  **Known limitation, still being tracked down**: something after that
  first successful DHCP exchange stops the card from generating
  interrupts for later sends - concretely, resolving DNS (which needs a
  fresh ARP request/reply) times out, even though the send call to the
  driver itself is confirmed happening. DHCP and anything else that
  only needs broadcast traffic works fine right now; regular browsing
  over a real e1000 card does not yet. Worth a dedicated, focused
  debugging pass rather than more guessing.
- **Shortcuts**: right-click any app in the Start menu → "SET AS
  SHORTCUT" drops a small `.lnk` marker file on the Desktop that launches
  that app when opened. A ZerBrowser shortcut is created by default at
  install time.
- **FPS/redraw counter is off by default** (it was a diagnostic tool for
  chasing down dragging lag, not meant to clutter the taskbar
  permanently) - toggle it with `fps on` / `fps off` / `fps` in the
  terminal.
- **Visual overhaul (user-supplied art)**: real, anti-aliased PNG icons
  (folder, disk, notepad, calculator, paint, terminal, browser, a plain
  text-file icon, and separate empty/full Recycle Bin icons that actually
  switch based on real contents) replace the old hand-drawn flat-color
  pixel art, rendered with proper alpha blending for clean edges. Window
  title bars and the taskbar now use a glossy green vertical gradient
  (light at top, dark at bottom) instead of a flat fill. The boot splash
  has a new logo and a green gradient progress bar that tracks real boot
  stages (filesystem, networking, desktop ready). New wallpaper, cropped
  and resized to fit 1024x768 without distortion.

- **This round's additions** (window manager, Paint, file management, and
  networking all got a big pass):
  - **Maximize/restore button** on every window, next to minimize/close.
    Remembers the pre-maximize position/size to restore to. Paint's canvas
    resizes (preserving existing content) when maximized/restored, same as
    it already did for manual resize.
  - **Paint got real tools**: Line, Rectangle, Ellipse, and Text, alongside
    the existing Brush/Eraser/Fill. Shapes show a live preview while
    dragging. One-level Undo (toggle - hitting it again redoes). **Save /
    Save As**, writing real `.bmp` files via a from-scratch uncompressed
    24-bit BMP encoder/decoder (`bmp.c`) - and opening a `.bmp` from
    Explorer loads it back into Paint.
  - **Recursive folder delete** - deleting a non-empty folder used to be
    refused; now it recurses through everything inside first.
  - **File clipboard**: Copy on a file, Paste in another folder (files
    only - no recursive folder copy yet).
  - **Properties dialog**: right-click → Properties shows name/type/size
    for any file or folder.
  - **Set as Wallpaper**: right-click a `.bmp` → the desktop background
    switches to it immediately, **and now persists across reboots** (the
    chosen path is remembered in a small `wallpaper.cfg` file and
    reapplied at the next boot). A `Wallpapers` folder is created at
    install time for keeping wallpaper images in, though it starts empty -
    see the bugs section for why I didn't auto-populate it with a
    "restore the default" copy.
  - **Real drag-and-drop on the desktop**: pick up an icon and drop it on
    the Recycle Bin (deletes it) or on a folder icon (moves it there) -
    a genuine mouse-held-down drag, not a right-click substitute. A quick
    click still opens things as before; the code tells click from drag by
    whether the mouse moved past a small threshold before release.
    Scoped to the desktop for now - Explorer items still use the
    right-click menu for the same operations.
  - **TCP retransmission**: segments that need an ACK (data, SYN, FIN) are
    now remembered and resent if nothing comes back within ~3 seconds,
    up to 5 tries before giving up - previously there was no recovery at
    all if a packet got dropped.
  - **DHCP client**: a real DISCOVER→OFFER→REQUEST→ACK handshake at boot,
    tried first; falls back automatically to the static 10.0.2.15/24
    config (which matches QEMU/VirtualBox NAT defaults) if no DHCP server
    answers in time. Verified end to end in QEMU - full handshake
    completes and the desktop still loads normally afterward.

- **Bootloader** (`boot/boot.asm`): Multiboot2 header (requests a linear
  framebuffer from GRUB), sets up page tables identity-mapping the first
  4 GiB, jumps into 64-bit long mode.
- **Core kernel**: GDT, IDT with full exception + IRQ handling, PIC remapping,
  PIT timer, CMOS RTC clock.
- **Memory management**: bitmap physical frame allocator, a simple free-list
  heap (`kmalloc`/`kfree`).
- **Graphics**: linear framebuffer, a bitmap font renderer, a custom mouse
  cursor (converted from a user-provided PNG, background removed via its
  alpha channel), and a boot splash screen (a user JPG logo, decoded to raw
  pixels at build time and blitted on a black background while the rest of
  the kernel loads).
- **Window manager** (`kernel/wm.c`): Windows 9x-styled UI (beveled gray
  panels, navy titlebars, teal desktop). Draggable AND resizable windows
  (drag the bottom-right corner grip) with proper z-order (click to bring
  to front), minimize/close buttons, a taskbar (Start button, per-window
  buttons, a live clock, and a network status icon), and a Start menu with
  Shutdown/Restart. The whole screen is double-buffered now - every frame
  is drawn to an off-screen buffer and blitted to the real framebuffer in
  one shot, instead of writing directly to video memory piece by piece
  (which is what caused the flicker/tearing when dragging windows before).
- **Apps**, each a real window type with its own rendering and input
  handling:
  - **Notepad** - actually typable; keyboard input is routed to the
    focused window.
  - **Calculator** - four-function arithmetic using fixed-point integer
    math (the kernel is built with SSE disabled, so no `double`/`float`
    anywhere - see "Bugs found" below). Now also takes keyboard input,
    not just mouse clicks.
  - **Paint** - freehand mouse-drag drawing onto a per-window pixel canvas.
  - **Explorer** - lists real files from NuggetFS with icons.
- **Icons**: hand-designed 16x16 Windows-9x-style pixel art (folder, disk,
  notepad, calculator, paint, generic file, browser), embedded directly in
  the kernel.
- **Font**: now has real lowercase letters (a-z) instead of silently
  folding everything to uppercase - Notepad and the browser show text as
  actually typed/received.
- **Input**: PS/2 keyboard + mouse, both IRQ-driven, mouse tracks real
  screen resolution.
- **Storage**: ATA PIO disk driver + **NuggetFS**, a custom filesystem
  (superblock, bitmap, inodes) with real hierarchical directories -
  create/rename/delete/move work at any nesting depth via a path-based API
  (`Documents/notes.txt`, etc.). Files up to ~4 MiB now (direct blocks plus
  one level of indirect blocks) and no ".."/"." path syntax.
- **Networking**: dual NIC support - RTL8139 (what QEMU emulates) and
  AMD PCNet/Am79C970A (what VirtualBox emulates by default; VirtualBox
  doesn't support RTL8139 at all). The kernel tries RTL8139 first, falls
  back to PCNet automatically. Ethernet + ARP + IPv4, and a real TCP state
  machine that handles both directions: incoming connections (handshake,
  data, FIN, several at once - the port-8080 echo server) *and* outgoing
  connections (`tcp_connect`, used by the browser). A blocking ARP
  resolver runs before the first outgoing SYN so it doesn't silently
  vanish while the peer's MAC is still unknown. The taskbar's network
  icon reflects whether either NIC came up.
- **ZerBrowser**: a minimal web browser app. Type an address into the bar -
  either a raw IP (`93.184.216.34/`) or a hostname (`example.com/`), hit
  Enter or click GO. Non-IP addresses go through a real DNS resolver first
  (UDP + hand-rolled DNS query/response parsing, hardcoded to the virtual
  network's own DNS proxy at 10.0.2.3 - the standard convention both
  QEMU's usermode networking and VirtualBox's NAT engine use). Back *and*
  Forward buttons with real history/redo stacks. Actually parses the HTML
  now instead of showing raw tag soup: `<script>`/`<style>` content is
  stripped, `<h1>`-`<h6>` render bigger, `<br>`/`<p>`/`<div>` become real
  line breaks, common entities (`&amp;` etc.) are decoded, and `<a href>`
  links render in blue and are genuinely clickable - click one and it
  navigates there. Relative links (`/path`) resolve against the current
  host; `http://` prefixes are stripped since there's no HTTPS. Also
  handles `<hr>`, `<li>` (with a `-` bullet), `<img>` (shows `[IMAGE: alt
  text]` since there's no image decoder), and skips the invisible content
  of `<title>`/`<head>`/`<select>`/`<option>` instead of leaking it into
  the page text. One real-world catch found while testing: several sites
  (including example.com) now sit behind a CDN (Cloudflare, in the cases
  checked) that rejects plain HTTP before the request ever reaches the
  actual site, replying with a short "Upgrade Required" - that's the CDN
  enforcing HTTPS, not a bug in the browser, and it'll show up as a short
  message rather than a crash. Plain-HTTP sites without that kind of front
  end work as expected.
- **Terminal**: a real command-line shell alongside the GUI apps. Supports
  `ls`/`dir`, `cd`, `pwd`, `cat`/`type`, `mkdir`, `touch`, `rm`/`del`,
  `echo`, `cls`/`clear`, `help` - all backed by the same NuggetFS path API
  Explorer uses, so it sees and changes the exact same files. Verified:
  `ls` correctly lists real directory contents, `help` prints the command
  list, command echo/prompt works.
- **Installer**: first boot (or any boot where NuggetFS isn't set up)
  shows a Welcome/disk-select/format wizard in the OS's own visual style
  instead of silently auto-formatting. Writes a marker file after
  installing so later boots skip straight to the desktop. Also creates
  default folders: Documents, Downloads, Desktop, Recycle Bin.
- **Desktop icons & Explorer, now backed by real folders**: the desktop
  itself is just the contents of a "Desktop" folder on disk, plus two
  fixed icons (My Computer -> root, Recycle Bin -> the Recycle Bin
  folder). Right-click the desktop or inside Explorer for a context menu:
  New Folder, New Text File, Rename (inline, type over the name and press
  Enter), Delete. Explorer navigates into folders on click, with a ".."
  entry to go back up. Deleting something moves it to the Recycle Bin
  (a real move, not a copy) rather than erasing it outright; inside the
  Recycle Bin the menu changes to Restore / Delete Forever, and
  right-clicking the Recycle Bin icon itself offers Empty.
- **Notepad now saves**: a small toolbar (SAVE / SAVE AS) sits above the
  text area, showing the current file's path (or "(UNSAVED)"). SAVE AS
  drops into an inline path field - type e.g. `Documents/todo.txt` and
  press Enter. Opening a `.txt` file from Explorer or the desktop loads it
  straight into a new Notepad window with its path already set, so SAVE
  just writes back to the same place.

## Bugs found and fixed along the way

- **A second AI reviewed NuggetFS and kernel_main.c independently, and
  caught two genuinely significant bugs we'd missed all session**:
  overwriting an existing file with *smaller* new content never freed
  the old trailing blocks from the previous, larger version - they leaked
  permanently, since deleting a file only ever frees blocks up to its
  *current* size. And the TCP server never reset its client handle to
  NULL on disconnect, so it could only ever serve one connection for the
  entire lifetime of a boot - anyone else trying to connect afterward
  would just be ignored forever. Merged both fixes by hand (adapting them
  to our current architecture - indirect blocks, cached batch writes -
  rather than copying the reviewed code verbatim), along with a handful
  of smaller but real robustness improvements: checking disk-read return
  values before trusting the data, returning a clear error instead of
  silently truncating an over-long path component (which could
  previously misresolve to the wrong file), rolling back a newly
  allocated inode if adding its directory entry fails (previously a
  permanent leaked slot), sizing the free-block bitmap to safely cover
  the whole disk instead of an estimate that could under-count it in
  edge cases, and freeing/nulling the in-memory bitmap before
  re-allocating on repeated format/mount calls instead of leaking it.
  Caught and corrected three of my own mistakes while adapting these
  patches by hand (return-type mismatches introduced during the merge -
  a `void` function that tried to return a value, and a `bool` function
  that returned nothing), and verified the merged code compiles clean
  and boots to a working desktop with normal file create/list/delete
  afterward.

- **Shutdown looked like a hang** - it wasn't actually broken, just doing
  the old-school Windows 9x thing: show "it is now safe to turn off your
  computer" and halt forever, waiting for a physical power switch. Added
  a real attempt at powering off first, via the well-known "quick
  shutdown" ACPI ports that QEMU, Bochs, and VirtualBox's virtual
  chipsets respond to - falls back to the old halt-and-wait only if none
  of those apply (e.g. real hardware without a matching virtual chipset).

- **Desktop icon labels lost their first letter** - "MY COMPUTER" showed
  as "Y COMPUTER", "RECYCLE BIN" as "ECYCLE BIN". The label draws centered
  under its icon, and for icons near the left edge of the screen with a
  long name, that centering math put the start of the text at a negative
  x-coordinate - i.e., partway off the left edge of the screen entirely.
  Fixed by clamping the label's position to stay fully on-screen (both
  edges) instead of centering blindly.

- **Renaming a file/folder in Explorer looked completely broken, but the
  actual rename logic was fine** - the bug was that nothing ever *showed*
  it was happening. The inline "you're renaming this now, type the new
  name" display only checked `renaming_window == -1` (the desktop) -
  there was no equivalent check anywhere in Explorer's drawing code, so
  choosing Rename on an Explorer item silently entered rename mode with
  zero visual feedback: no edit box, no cursor, nothing to indicate
  typing was even going anywhere. The backend (`nfs_rename_path`) was
  never the problem. Fixed by adding the same "show the live edit buffer
  instead of the stored name" check Explorer's item-drawing loop that
  desktop icons already had.
- **Entering folders in Explorer had gotten flakier after adding
  Explorer drag-and-drop** - clicking an item now has to tell a "quick
  click" apart from "the start of a drag" by how far the mouse moves
  before release, and that threshold was only ~6 pixels. Real mouse/
  trackpad hardware moves more than that from ordinary hand tremor
  during an intentional single click, especially compared to a scripted
  test click landing on an exact pixel - so an ordinary click could
  misfire as "a drag with nowhere valid to drop," silently failing to
  open the folder. Raised the threshold to ~12 pixels, which is still
  small enough to recognize a real drag immediately but should stop
  ordinary clicks from being second-guessed.

- **Fast window drags left a trail of ghost title bars behind** - a real
  visual bug from the redraw-throttling work: the "which rows actually
  changed" tracker was being reset at the *start* of every single
  `wm_update` call, including ones where the throttle skipped the actual
  redraw+present. So if several mouse-move events got coalesced into one
  redraw (which throttling does on purpose), only the *last* event's
  small movement was remembered - the wider path the window travelled
  through in between never got copied to the real screen, leaving stale
  title-bar images at each in-between position. Fixed by only resetting
  the tracked region *after* it's actually been used for a real
  redraw+present, so it correctly accumulates across however many frames
  got coalesced. Verified with a deliberately huge, fast single-jump drag
  (worst case for this bug): only one title bar ends up on screen,
  exactly at the drop point, and dragging speed is unaffected.
- **Explorer items couldn't be dragged anywhere** - only desktop icons
  had real drag-and-drop; Explorer always treated a click-and-hold as
  "open this," so there was no way to drag a file out of it. Extended
  the same click-vs-drag logic to Explorer's file list, with the drop
  targets now including the Recycle Bin, folder icons on the desktop,
  folder items inside any open Explorer window, or an Explorer window's
  general content area (moves into whatever folder it's showing).
  Verified end-to-end: dragged a file out of an Explorer window and
  confirmed it was gone from its original folder afterward.

- **The framebuffer wasn't using a fast memory-caching mode, and this
  time it got fixed properly** - earlier I'd deliberately avoided this
  (see the note further down) since getting CPU memory-type
  configuration wrong can hang a real machine hard enough to need a
  power cycle. To be clear for anyone worried about this: that's the
  actual worst case - a misconfigured cache type doesn't damage
  hardware, CPUs don't have a "burn the silicon" failure mode for this
  kind of software mistake. Implemented it narrowly and carefully: set
  up one currently-unused PAT slot (the one normally used for
  Write-Through, which nothing here selects) as Write-Combining, then
  marked only the specific 2MiB page(s) covering the framebuffer with it
  - nothing else about memory typing changes, and the global MTRRs are
  never touched. Verified step by step: boots cleanly with a log message
  confirming success, wallpaper/UI colors are pixel-correct (no
  corruption from a caching mismatch), and the full-screen framebuffer
  copy dropped from ~30ms to ~10ms - a genuine ~3x improvement for
  anything that isn't a windowed drag (Paint strokes, resizing, etc.,
  which weren't covered by the earlier dirty-rectangle work).
- **`alloc_inode` had the exact same rescan-from-the-start bug as the
  data-block allocator (fixed earlier), and it was worse**: creating any
  new file or folder rescanned inodes starting from #1 every time, and
  each candidate cost a *real disk read* (unlike the block bitmap, which
  is just an in-memory check). Fixed with the same kind of search cursor
  - remembers where the last allocation left off instead of starting
  over each time.
- **Freeing blocks on delete had the mirror-image bug**: deleting a file
  flushed the free-space bitmap to disk after *every single block*
  freed, so deleting something large meant hundreds of redundant bitmap
  writes, the same wasted-effort pattern as the allocation-side bug
  fixed earlier - just never applied to the delete path. Fixed the same
  way: free all the blocks first, flush once at the end.
- Verified both filesystem fixes together with a real sequence through
  the terminal (mkdir, several touches, an rm, another touch, then ls) -
  the deleted file was correctly gone and every other file and folder
  was intact, so the cursor-based allocation isn't handing out
  duplicate or wrong inodes.

- **Desktop icons had the exact same disk-read-every-frame bug as
  Explorer**, and it explained something specific: creating more files on
  the desktop made *every* window's dragging slower, not just Explorer's.
  `draw_desktop_icons()` called `nfs_list_path("Desktop", ...)`
  unconditionally on every single redraw regardless of which window was
  being dragged, so more desktop files meant more per-frame disk work no
  matter what was on screen. Fixed the same way as Explorer: only re-read
  the Desktop folder when something could have actually changed it.
- **The clock was reading the CMOS RTC (a hardware I/O port) far more
  often than it needed to** - once per `wm_update` call, which can happen
  very often during a fast drag, when the displayed time only changes
  once a minute. I/O port access is inherently slow, and under
  virtualization each one can trigger a full VM exit (a guest-to-host
  context switch), making this a real, avoidable cost. Throttled it to
  check at most once a second.
- **Every character of every string drawn did a linear scan through all
  ~75 font glyphs** to find the right one - window titles, button labels,
  file names, the clock, the FPS counter, all of it, every single frame.
  Replaced with a direct 128-entry lookup table built once at first use,
  making glyph lookup O(1) instead of O(n) per character.
- Measured the combined effect of all three fixes with the same drag
  test used throughout this pass: general dragging went from ~13-22
  redraws/sec to ~35-48, and Explorer specifically (previously the worst
  case at ~1-3) now holds steady at ~29-36 - in line with, or better
  than, the ~30/sec target.

- **The exact same disk-read-per-frame bug was also in `draw_desktop_icons`**
  - it called `nfs_list_path("Desktop", ...)` unconditionally on *every*
  redraw, not just when an Explorer window was open. This explains two
  things at once: why even non-Explorer windows weren't reaching a great
  frame rate, and why "creating files on the desktop makes it laggier" -
  more desktop items meant more work re-read from disk on every single
  frame, every drag, everywhere. Fixed the same way as Explorer: cached,
  only re-read when something could have actually changed the folder.
- **The real fix for reaching a solid, high frame rate**: even after
  caching the wallpaper and skipping unnecessary disk reads, every drag
  frame was still re-drawing the *entire* wallpaper and *every* window
  into the back buffer, even though only one window was actually moving
  or being drawn into - filling millions of pixels nothing needed touched.
  Added real dirty-rectangle rendering: while dragging a window or drawing
  in Paint, only the rows of the screen that could have actually changed
  (the window's old and new position, or just its own bounds for Paint)
  get redrawn and copied to video memory at all - the wallpaper and any
  unrelated windows are left completely untouched for that frame, since
  nothing about them changed. Checked carefully for ghosting (an
  overlapping second window plus desktop icons, dragged across each other)
  and found none - the old position is correctly cleared every time.
  Redraws/sec during the same drag test went from ~20-22 to consistently
  hitting the throttle ceiling itself (~43-50, since we raised that
  ceiling too once the underlying work got fast enough to benefit) -
  Explorer specifically, which was the original complaint, went from
  ~1-3 all the way up to the same ~45-50 as everything else.

- **Explorer was uniquely, dramatically laggier than every other app when
  dragging** (~1-3 redraws/sec vs ~20 for Notepad/Calculator/Paint) - a
  really useful bug report, since the *difference* between apps pointed
  straight at the cause. `draw_explorer` was calling `nfs_list_path`
  (a real directory read through the NuggetFS/ATA layers) on *every single
  redraw*, so dragging its window meant re-reading the folder's contents
  from disk dozens of times a second, even though nothing about the
  folder had changed. Every other app only touches in-memory pixel
  buffers when drawing, which is why they weren't affected. Fixed with a
  per-window cache: each Explorer window now only re-reads its folder
  when the path actually changes, or when something explicitly could have
  altered it (new file/folder, delete, rename, paste, a drag-and-drop
  move, or a save from Notepad/Paint/the terminal all invalidate it).
  Verified with the same drag test: Explorer went from ~1-3 redraws/sec
  to ~20-21, right in line with everything else.

- **The real numbers behind the dragging lag** - added an actual redraws/sec
  counter (visible in the taskbar as "R:XX", and logged to serial) plus
  per-step timing, instead of continuing to guess from how things "felt".
  That turned up two concrete costs: the wallpaper was being re-packed
  pixel-by-pixel *every frame* even though it never changes (~40ms for a
  1024x768 image), and copying the finished frame to real video memory
  took ~30ms *regardless of how much actually changed* - together those
  two accounted for nearly the entire per-frame cost. Fixed by caching the
  wallpaper's packed pixels once instead of recomputing them every frame,
  and by only copying the *rows that actually changed* to video memory
  during a window drag (the wallpaper and other windows don't move, so
  there's nothing to gain by re-sending them) instead of the whole screen
  every time. Measured before/after with the same drag test: redraws/sec
  went from ~13-14 to ~21-22, and the video-memory-copy step dropped from
  a consistent ~30ms to ~0-2ms.
- Copying data to the real framebuffer is still slower than it really
  should be for straightforward memory writes, which points at the video
  memory not being mapped with a fast caching mode (write-combining) -
  fixing that properly means configuring the CPU's PAT/MTRRs for that
  specific memory region, which I deliberately didn't do blind: get it
  wrong and it's the kind of mistake that can lock up real hardware in a
  way that's hard to recover from remotely. Worth a careful, dedicated
  pass if more speed is wanted.

- **The actual cause of the dragging/Paint lag** turned out to be upstream
  of the pixel-drawing code entirely: PS/2 mice can (and do, especially
  under emulation) fire mouse-move interrupts far more often than any
  screen could usefully show, and every single one that moved something
  triggered a *full* wallpaper+windows+taskbar redraw plus a whole
  framebuffer copy - so we were doing that expensive full-screen work
  many times more often than the display could even show, starving the
  actual drag/paint logic of CPU time and making it feel jerky no matter
  how fast the pixel-filling itself was (the earlier `fb_fill_rect`/blit
  optimization was a real improvement but couldn't fix this on its own).
  Fixed by throttling the redraw+present step to roughly 50/sec *during
  continuous operations* (dragging, resizing, an active Paint stroke) -
  window position and other logic still update on every single event, so
  nothing lags behind the mouse; only the expensive visual redraw is
  coalesced. One-off events (a click, opening a window) still redraw
  immediately, since there's no flood of those to throttle. Verified with
  a rapid 40-step simulated drag: the window landed exactly on the
  expected pixel, so throttling the redraw didn't cost any positional
  accuracy.

- **Window dragging and Paint drawing were visibly laggy, even in QEMU and
  on real hardware** - every dirty frame redraws the wallpaper, every
  window, and the taskbar from scratch (no dirty-rectangle tracking yet),
  and the core pixel-drawing functions were making this worse than it
  needed to be: `fb_fill_rect` called `fb_put_pixel` - with its own bounds
  check and branch - once per pixel, and the wallpaper/Paint canvas blits
  did the same thing with a `fb_pack_color` call on top for every single
  pixel of a 1024x768 image or a full Paint canvas. Rewrote `fb_fill_rect`
  to clip once and write each row directly, and added two new blit paths:
  `fb_blit_rgb888` (packs colors per row for raw RGB source images like
  the wallpaper) and `fb_blit_packed` (a straight `memcpy` per row for
  buffers already in device-native format, like a Paint canvas, since no
  conversion is needed at all). Verified visually afterward - wallpaper,
  icons, and a drawn Paint stroke all still render correctly. Full
  dirty-rectangle tracking (only redrawing what actually changed instead
  of the whole screen every frame) would help further, especially for
  dragging, and is a reasonable next step if this isn't enough on its own.

- **NuggetFS's 40 KiB file size limit silently broke Paint's new Save
  feature**: a typical saved canvas (~170 KiB as a BMP) or a full-screen
  wallpaper (~2.3 MiB) is way over the old direct-blocks-only limit, so
  `nfs_write_path` was just returning an error - Save looked like it
  worked but didn't. Fixed properly by adding single-indirect-block
  support to NuggetFS (an inode can now point at one block that's itself
  full of up to 1024 more block pointers), raising the limit to ~4 MiB.
- **Same bug class as the earlier 256-inode formatting issue, again**:
  the new indirect-block code was doing a full read-modify-write of the
  indirect block on *every single data block* of a write instead of once
  for the whole operation - for a several-hundred-block file that's
  hundreds of redundant disk round-trips. Fixed by caching the indirect
  block in memory for the duration of one write and flushing it once at
  the end.
- **`alloc_data_block` rescanned the free-space bitmap from bit 0 on every
  single call**, and flushed the bitmap to disk after every single
  allocation too - fine for one block at a time, but writing a large file
  needs hundreds of allocations, turning an O(n) pass into an O(n^2) one
  plus hundreds of avoidable disk writes for bitmap bookkeeping. Fixed
  with a persistent search cursor (continues where the last search left
  off) and by deferring the bitmap flush to once per write instead of
  once per block.
- Even after those fixes, writing a full ~2.3 MiB wallpaper-sized file
  is still slow with this simple polling ATA driver - noticeably slower
  than I'd like. I'd planned to auto-generate a "restore the default
  dandelion wallpaper" BMP during install, but doing that unconditionally
  risked hanging first-time setup for everyone, so I removed it rather
  than ship something that risky. Saving realistic Paint-sized canvases
  (tens to ~170 KiB) works fine and was verified end to end.

- **Multiboot2 pointer loss** (`boot/boot.asm`): `rsp` was reset to
  `stack_top` *after* the multiboot2 info pointer had already been pushed
  onto the old 32-bit stack, silently corrupting it. This had been quietly
  breaking physical memory map parsing since the very first boot. Fixed by
  stashing the pointer in a dedicated memory location instead of the stack.
- **ATA IRQ storm**: polled PIO disk I/O left IRQ14/15 unmasked, so the
  drive's INTRQ line flooded the PIC after every command. Fixed by masking
  those IRQs since we don't use interrupt-driven disk I/O.
- **SSE register return with SSE disabled**: the calculator originally used
  `double` for arithmetic, but this kernel is compiled with
  `-mno-sse -mno-sse2` (no FPU/SSE state is saved on interrupts, so using
  it would corrupt registers across IRQs). The x86-64 ABI returns
  floating-point values in an SSE register regardless, so this failed to
  compile. Rewrote the calculator to use fixed-point `int64_t` arithmetic
  instead (scaled by 10000 for four decimal digits).
- **PCNet descriptor struct was 12 bytes instead of the required 16**: the
  first received packet (ARP) worked fine since it used descriptor slot 0,
  but the *next* packet (the TCP SYN) landed in slot 1 - and because our
  struct was 4 bytes short, our array's slot 1 didn't line up with where
  the hardware actually wrote it, so the driver never saw it. Fixed by
  padding the descriptor to the full 16 bytes the Am79C970A spec requires,
  plus a `_Static_assert` so this can't silently regress again.
- **Missing font glyphs**: the calculator's `+ - * / = .` labels and any
  lowercase-typed text in Notepad rendered as nothing, because the bitmap
  font simply had no entries for them. Added the missing glyphs, folded
  lowercase to uppercase for display, and added a fallback placeholder box
  for any other unmapped-but-printable character so typing never silently
  vanishes again.
- **Outgoing SYN silently dropped when ARP wasn't cached yet**: opening a
  connection *to* somewhere (needed for the browser) sends the SYN before
  anything has told the NIC the destination's MAC address. `ip_send`
  correctly fires off an ARP request in that case, but was returning
  `false` and the caller just discarded the packet instead of retrying -
  so the very first outgoing connection attempt to any new address always
  failed silently. Fixed by resolving ARP synchronously (a bounded polling
  wait, interrupts stay on) before sending the initial SYN.
- **Same ARP bug, missed in the new DNS code**: the DNS resolver sends its
  query over UDP, which goes through the exact same `ip_send` path as
  TCP - and needs the same ARP-resolved-first treatment. I fixed the TCP
  side of this earlier but didn't carry the fix over to `dns_resolve()`
  when I added it, so the very first DNS query (to the resolver, which
  hadn't been ARPed yet) silently vanished every time - "DNS LOOKUP
  FAILED" on literally any hostname. Fixed the same way: resolve ARP for
  the DNS server before sending the query.
- **DNS resolver hardcoded to a public server (8.8.8.8) that some NAT
  setups won't actually forward**: worked fine in my own QEMU testing,
  but failed with the same "DNS LOOKUP FAILED" in VirtualBox even after
  the ARP fix above - because outbound UDP:53 to the real internet isn't
  guaranteed to be let through by every NAT configuration, even when
  regular TCP traffic works fine. Switched to querying 10.0.2.3 instead -
  the DNS proxy address both QEMU's and VirtualBox's virtual networks
  provide by convention, which resolves using the host's own DNS setup
  rather than requiring the guest to reach the internet's DNS directly.
- **Installer painfully slow / looked hung on the progress bar**: formatting
  called `inode_write()` once per inode (256 times) to zero the inode
  table, and each of those calls does a full block *read-modify-write* -
  hundreds of redundant disk round-trips to zero a handful of blocks.
  Fixed by zeroing the inode table directly, one block at a time (down
  from 256 read+write cycles to a handful of plain writes). Also: ATA
  wait-loops were bounded by a raw iteration count rather than real time,
  so how long a "timeout" actually took depended on how fast each I/O port
  access happened to be under whatever hypervisor was running it -
  switched them to wall-clock bounds (1 real second) so behavior is
  consistent everywhere. And the disk write path was doing a cache-flush
  wait after *every single sector* instead of once per multi-sector write -
  now flushes once per write call.

- **`SYSTEM.CFG` could be deleted, silently wiping the whole disk on next
  boot**: the installer decides whether to run by checking for this one
  marker file - if it's missing, it reformats. Since it was just a normal,
  visible file, deleting it (by hand via Explorer/Terminal, or by mistake)
  meant the *next* boot would treat the disk as brand new and format it,
  destroying Documents/Downloads/everything. Fixed three ways: the file is
  now hidden from `ls`/Explorer listings entirely, both the context-menu
  Delete and the terminal's `rm`/`del` explicitly refuse to touch it by
  name, and as a last line of defense, the installer now checks for real
  user folders (Documents/Desktop/Downloads) before reformatting - if
  they're already there but the marker isn't, it just quietly recreates
  the marker instead of wiping the disk.

## Known limitations (intentional, for this pass)

- **No USB** - PS/2 covers keyboard/mouse; a real USB stack (UHCI/EHCI/xHCI)
  is a project on the same scale as everything else here combined.
- **~4 MiB per file** (v1 limitation - one level of indirect blocks, no
  double-indirect), and no ISO9660 reader - inserted
  ISO images can't be browsed yet (that's a whole separate filesystem
  format to implement, distinct from NuggetFS).
- **No "This PC" multi-drive view** - there's only the one real disk, so
  Explorer shows it as a single "LOCAL DISK (C:)"; a genuine multi-drive
  view only makes sense once there's more than one actual volume to show.
- **Drag-and-drop now works from Explorer too**, not just the desktop -
  drop targets are the Recycle Bin, desktop folder icons, folder items
  inside any open Explorer window, or an Explorer window's general
  content area.
- **File clipboard doesn't handle folders** - Copy/Paste works for files
  only; pasting a copied folder isn't supported yet (no recursive copy).
- **Restore always goes back to Desktop**, not wherever the item
  originally lived - there's no metadata tracking "where it came from".
- **Writing very large files (multi-hundred KB and up) is slow** - the
  simple polling ATA driver just isn't fast at hundreds of sequential
  block writes. Fine for normal file sizes; something like a full-screen
  wallpaper BMP (~2.3 MiB) will take a noticeably long time to save.
- **No Cyrillic in the font** - only ASCII (now including lowercase).
- **Network status icon doesn't detect a live/dropped link** - it reflects
  whether the driver initialized at boot, not real-time cable/adapter
  state. A proper fix means polling the NIC's link-status register
  continuously, which needs care to get right per chip (RTL8139 vs
  PCNet) - didn't want to guess at register semantics I couldn't verify
  without a way to actually simulate "cable unplugged" in testing.
- **Installer doesn't make the OS independently bootable from a real hard
  disk** - it sets up NuggetFS (the data filesystem) on first run, which
  is a very different thing from writing a bootloader/MBR to a target
  disk so the machine boots Nugget OS without the install media. That's
  a separate, much bigger project if it's ever wanted.
- **Single static IP**, no DHCP.

## Building

Requires `nasm`, `gcc`, `ld`. For a bootable ISO you'll also need
`grub-mkrescue`, `xorriso`, `mtools`; to test, `qemu-system-x86_64`.

```bash
make                              # builds kernel + nugget-os.iso
qemu-img create -f raw disk.img 32M   # one-time: filesystem disk
make run                          # boot it in QEMU
```

## Project layout

```
boot/           - bootloader (asm, multiboot2, long mode, embedded splash logo)
kernel/         - GDT, IDT, timer, VGA/serial console, heap, framebuffer,
                  bitmap font, icons, mouse cursor, RTC clock, splash screen,
                  window manager + apps (kernel/wm.c)
kernel/int/     - interrupt handling (ISR/IRQ asm stubs + C dispatch)
kernel/mm/      - physical memory manager, heap allocator
drivers/ps2/    - PS/2 keyboard + mouse
drivers/ata/    - ATA PIO disk driver
drivers/net/    - PCI enumeration, RTL8139 NIC driver
fs/             - NuggetFS filesystem
net/            - Ethernet/ARP/IPv4 + TCP
include/        - all headers
linker.ld, Makefile, grub.cfg
```

## Next steps (rough priority order)

1. File-open association (double-click a file in Explorer -> opens in Notepad).
2. Subdirectories + bigger files in NuggetFS.
3. TCP retransmission timers.
4. USB (start with UHCI).


OS Created with Claude

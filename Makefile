# Quilon — top-level convenience wrapper.
#
# The shell scripts (config.sh, headers.sh, build.sh, iso.sh, qemu.sh,
# create_disk.sh, clean.sh) remain the source of truth for the build. This
# Makefile only provides standard targets and a cross-compiler pre-flight check
# so newcomers don't have to memorize the script chain.
#
#   make            build the kernel + libc (-> sysroot/boot/quilon.kernel)
#   make iso        build the bootable ISO (quilon.iso)
#   make disk       build user programs into the FAT16 disk image (disk.img)
#   make run        build everything and boot under QEMU (GUI)
#   make smoke      headless QEMU boot smoke test (see tools/smoke_test.sh)
#   make ktest      headless in-OS kernel regression tests (user/ktest)
#   make kbdtest    headless keyboard test: shift, arrows, Ctrl-C (17.0.1)
#   make wmtest     headless desktop test: open/close a terminal 3x via the mouse
#   make test       run the host unit-test suite (uses host gcc, no toolchain)
#   make clean      remove build artifacts
#
# The cross toolchain (i686-elf-*) is required for everything EXCEPT `make test`,
# which compiles the kernel sources with the host gcc.

HOST ?= $(shell ./default-host.sh)

.PHONY: all build headers iso disk ext2disk run smoke ktest wmtest kbdtest powertest termtest eventtest appstest test clean check-toolchain

all: build

# `make disk WALLPAPER=img.bmp` (ROADMAP3 18.5) replaces the demo wallpaper;
# WALLPAPER=none leaves it off.  The user/ sub-make runs in another
# directory, so hand it an absolute path.
ifneq ($(filter-out none,$(WALLPAPER)),)
override WALLPAPER := $(abspath $(WALLPAPER))
endif
export WALLPAPER

# Fail early with a clear message if the cross toolchain is missing, instead of
# letting the first compiler invocation deep inside a sub-make fail obscurely.
check-toolchain:
	@command -v $(HOST)-gcc >/dev/null 2>&1 || { \
	  echo "error: '$(HOST)-gcc' not found on PATH."; \
	  echo "       Build the i686-elf cross toolchain first (see BUILD.md)."; \
	  echo "       'make test' does not need it (it uses the host gcc)."; \
	  exit 1; }

headers: check-toolchain
	@./headers.sh

build: check-toolchain
	@./build.sh

iso: check-toolchain
	@./iso.sh

disk: check-toolchain
	@./create_disk.sh

# ext2 image with the same user programs (section 16.2).  Needs host mke2fs.
# The kernel probes ext2 before FAT16, so booting with ext2.img attached in
# place of disk.img exercises the ext2 driver.
ext2disk: check-toolchain
	@$(MAKE) -C user ext2disk

run: check-toolchain
	@./qemu.sh

smoke: check-toolchain
	@./tools/smoke_test.sh

# In-OS kernel regression tests (user/ktest, REVIEW.md): boots headless, types
# `exec ktest.elf` into the ring-3 shell and requires "KTEST: ALL PASS".
ktest: check-toolchain
	@SMOKE_KEYS='exec ktest.elf\n' SMOKE_SENTINEL='KTEST: ALL PASS' \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-180} ./tools/smoke_test.sh

# Keyboard regression (ROADMAP3 17.0.1), typed as real PS/2 scancodes:
# shifted symbols reach the shell; an arrow key's escape sequence is swallowed
# by the line editor (not inserted as "[D"); Ctrl-C at the prompt drops the
# line; Ctrl-C at a nested shell's prompt does NOT kill it (shells ignore
# SIGINT; it exits 0 on `exit`); Ctrl-C while kprobe blocks reading the
# keyboard kills it with SIGINT (code -2) -- the keyboard-IRQ route.
KBD_KEYS = Hello(World)!\n{until:Unknown command: Hello(World)!}ab{key:left}c\n{until:Unknown command: abc }xyz{key:ctrl-c}q\n{until:Unknown command: q }exec shell.elf\n{until:launching 'shell.elf'}{wait:2}{key:ctrl-c}{wait:1}exit\n{until:'shell.elf' exited (code 0)}exec kprobe.elf\n{until:launching 'kprobe.elf'}{wait:2}{key:ctrl-c}
# Second phase, the desktop route: startx, open a Terminal and click into it
# (new windows are not focused yet -- ROADMAP3 18.3), Ctrl-C at its prompt,
# then Ctrl-C kprobe running in it.  The WM ignores SIGINT, gets 0x03 as a key,
# and gterm asks the kernel (intr) to SIGINT its shell's foreground job.
KBD_WM_KEYS = startx\n{until:[wm] screen}{wait:2}$(OPEN_TERM){wait:4}{mouse:-3000,-3000}{mouse:300,300}{click}{wait:1}xyz{key:ctrl-c}{wait:1}exec kprobe.elf\n{wait:3}{key:ctrl-c}
kbdtest: check-toolchain
	@SMOKE_KEYS="$(KBD_KEYS)" SMOKE_SENTINEL="'kprobe.elf' exited (code -2)" \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-90} ./tools/smoke_test.sh
	@SMOKE_KEYS="$(KBD_WM_KEYS)" SMOKE_SENTINEL="terminated by signal 2" \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-90} ./tools/smoke_test.sh

# Opening an app on the desktop (ROADMAP3 18.4): the Apps button (30,786),
# then its menu item -- Terminal (60,715), Clock (60,737) -- on the 1280x800
# screen (wm.h WM_MENU_* layout; test_wm.c checks the Terminal spot).
OPEN_MENU  = {mouse:-3000,-3000}{mouse:30,786}{click}{wait:1}{mouse:-3000,-3000}
OPEN_TERM  = $(OPEN_MENU){mouse:60,715}{click}
OPEN_CLOCK = $(OPEN_MENU){mouse:60,737}{click}
OPEN_FILES = $(OPEN_MENU){mouse:60,759}{click}

# Files + widget kit (ROADMAP3 18.5), on the FAT16 disk: open Files (content
# at (40,80); a 26-px toolbar, then 16-px rows: README.TXT is row 9 -> y 258,
# HELLO.ELF row 4 -> y 178, after COUNTER.ELF and the demo DESKTOP.BMP).
# Double-click README.TXT -> a Textview; click the Files title bar to bring it
# forward; double-click HELLO.ELF -> it runs in a
# Terminal window that stays open after the program exits, until a key.
APPS_KEYS = startx\n{until:[wm] screen}{wait:2}$(OPEN_FILES){wait:3}{mouse:-3000,-3000}{mouse:100,258}{dblclick}{wait:3}{mouse:-3000,-3000}{mouse:100,70}{click}{wait:1}{mouse:-3000,-3000}{mouse:100,178}{dblclick}{wait:4}q{wait:2}
appstest: check-toolchain
	@SMOKE_KEYS='$(APPS_KEYS)' SMOKE_SENTINEL="[wm] closed 'HELLO.ELF'" \
	    SMOKE_ALSO="[wm] opened '/README.TXT' in textview" \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-90} ./tools/smoke_test.sh

# Desktop regression (REVIEW.md E): startx, then open and close a Terminal three
# times with the mouse (Apps menu -> Terminal, close box at (670,70) on the
# 1280x800 screen).  Each closed terminal's shell must see EOF and exit -- it
# used to spin forever as an unreaped orphan.
WM_CYCLE = $(OPEN_TERM){wait:4}{mouse:-3000,-3000}{mouse:670,70}{click}{wait:4}
#
# Then window operations (ROADMAP3 18.3), on a fresh boot: open a Terminal
# (cascade origin: content (40,80) 640x384), drag its resize grip (bottom-right
# corner, 680,464) by +100,+64 -> 740x448; double-click the title bar ->
# maximized to 1280x752 (800 - 28 taskbar - 20 title bar); the restore button
# (1252,10) -> back to 740x448; the minimize button (734,70); Alt-Tab restores
# it.  "restored" only logs for a window that was minimized.
WM_OPS_KEYS = startx\n{until:[wm] screen}{wait:2}$(OPEN_TERM){wait:4}{mouse:-3000,-3000}{mouse:676,460}{down}{mouse:50,32}{mouse:50,32}{up}{wait:2}{mouse:-3000,-3000}{mouse:300,70}{dblclick}{wait:2}{mouse:-3000,-3000}{mouse:1252,10}{click}{wait:2}{mouse:-3000,-3000}{mouse:734,70}{click}{wait:2}{key:alt-tab}{wait:2}
#
# Then the taskbar window list (ROADMAP3 18.4), on a fresh boot: open a
# Terminal, then a Clock from the Apps menu (the Clock has the focus).  The
# Terminal's taskbar button (first, at 100,786) is clicked three times:
# raise + focus, minimize (it's focused), restore.
WM_TASKBAR_KEYS = startx\n{until:[wm] screen}{wait:2}$(OPEN_TERM){wait:4}$(OPEN_CLOCK){wait:2}{mouse:-3000,-3000}{mouse:100,786}{click}{wait:1}{click}{wait:1}{click}{wait:2}
wmtest: check-toolchain
	@SMOKE_KEYS='startx\n{until:[wm] screen}{wait:2}$(WM_CYCLE)$(WM_CYCLE)$(WM_CYCLE)' \
	    SMOKE_SENTINEL='exited (code 0)' SMOKE_SENTINEL_COUNT=3 \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-90} ./tools/smoke_test.sh
	@SMOKE_KEYS='$(WM_OPS_KEYS)' SMOKE_SENTINEL="[wm] restored 'Terminal'" \
	    SMOKE_ALSO="[wm] resized 'Terminal' to 740x448" SMOKE_ALSO_COUNT=2 \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-90} ./tools/smoke_test.sh
	@SMOKE_KEYS='$(WM_TASKBAR_KEYS)' SMOKE_SENTINEL="[wm] restored 'Terminal'" \
	    SMOKE_ALSO="[wm] launched 'Clock'" \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-90} ./tools/smoke_test.sh

# `exit` in a terminal window closes the window -- but only the terminal's own
# shell: open a Terminal, click into it, `exec shell.elf` + `exit` (the nested
# shell returns to its parent; the window must stay), run hello.elf in it to
# prove that, then `exit` the terminal's shell.  Three "exited (code 0)" lines
# (nested shell, hello, terminal shell) and the WM's close line are required.
TERM_KEYS = startx\n{until:[wm] screen}{wait:2}$(OPEN_TERM){wait:4}{mouse:-3000,-3000}{mouse:300,300}{click}{wait:1}exec shell.elf\n{wait:3}exit\n{wait:2}exec hello.elf\n{wait:3}exit\n
termtest: check-toolchain
	@SMOKE_KEYS='$(TERM_KEYS)' SMOKE_SENTINEL="[wm] closed 'Terminal'" \
	    SMOKE_ALSO='exited (code 0)' SMOKE_ALSO_COUNT=3 \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-90} ./tools/smoke_test.sh

# Richer input events (ROADMAP3 18.2): open a Terminal, then a Clock (on top
# of it).  FAST-click the Terminal (press+release quicker than a WM loop
# iteration -- only the kernel's latched button edges catch it), which raises
# and focuses it; Alt-Tab then cycles focus to the backmost window, now the
# Clock; Alt-F4 closes the focused window.  So the Clock must close and the
# Terminal must not.  (A lost click leaves the Terminal backmost, so Alt-Tab
# picks it and Alt-F4 closes the Terminal instead; a missing Alt-Tab or
# Alt-F4 closes nothing.)  The boot log must show the wheel was negotiated.
EVENT_KEYS = startx\n{until:[wm] screen}{wait:2}$(OPEN_TERM){wait:3}$(OPEN_CLOCK){wait:2}{mouse:-3000,-3000}{mouse:100,300}{fastclick}{wait:1}{key:alt-tab}{wait:1}{key:alt-f4}{wait:3}
eventtest: check-toolchain
	@SMOKE_KEYS='$(EVENT_KEYS)' SMOKE_SENTINEL="[wm] closed 'Clock'" \
	    FAIL_PATTERN="PANIC|Kernel panic|Exception|Triple fault|General Protection|closed 'Terminal'" \
	    SMOKE_ALSO='scroll wheel enabled' \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-90} ./tools/smoke_test.sh

# Power off (shell `poweroff`, desktop Power button): QEMU must exit on its own.
# Desktop: Power button at (1240,786); the dialog's "Power off" button at
# (580,420) -- both on the 1280x800 screen (wm.h WM_POWER_* layout).
POWER_WM_KEYS = startx\n{until:[wm] screen}{wait:2}{mouse:-3000,-3000}{mouse:1240,786}{click}{wait:2}{mouse:-3000,-3000}{mouse:580,420}{click}
powertest: check-toolchain
	@SMOKE_KEYS='poweroff\n' SMOKE_SENTINEL='powering off' SMOKE_EXPECT_EXIT=1 \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-60} ./tools/smoke_test.sh
	@SMOKE_KEYS='$(POWER_WM_KEYS)' SMOKE_SENTINEL='powering off' SMOKE_EXPECT_EXIT=1 \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-60} ./tools/smoke_test.sh

# Host unit tests — deliberately NOT gated on the cross toolchain.
test:
	@./test.sh

clean:
	@./clean.sh

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

.PHONY: all build headers iso disk ext2disk run smoke ktest wmtest kbdtest test clean check-toolchain

all: build

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
KBD_WM_KEYS = startx\n{until:[wm] screen}{wait:2}{mouse:-3000,-3000}{mouse:55,786}{click}{wait:4}{mouse:-3000,-3000}{mouse:300,300}{click}{wait:1}xyz{key:ctrl-c}{wait:1}exec kprobe.elf\n{wait:3}{key:ctrl-c}
kbdtest: check-toolchain
	@SMOKE_KEYS="$(KBD_KEYS)" SMOKE_SENTINEL="'kprobe.elf' exited (code -2)" \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-90} ./tools/smoke_test.sh
	@SMOKE_KEYS="$(KBD_WM_KEYS)" SMOKE_SENTINEL="terminated by signal 2" \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-90} ./tools/smoke_test.sh

# Desktop regression (REVIEW.md E): startx, then open and close a Terminal three
# times with the mouse (taskbar button at (55,786), close box at (670,70) on the
# 1280x800 screen).  Each closed terminal's shell must see EOF and exit -- it
# used to spin forever as an unreaped orphan.
WM_CYCLE = {mouse:-3000,-3000}{mouse:55,786}{click}{wait:4}{mouse:-3000,-3000}{mouse:670,70}{click}{wait:4}
wmtest: check-toolchain
	@SMOKE_KEYS='startx\n{until:[wm] screen}{wait:2}$(WM_CYCLE)$(WM_CYCLE)$(WM_CYCLE)' \
	    SMOKE_SENTINEL='exited (code 0)' SMOKE_SENTINEL_COUNT=3 \
	    SMOKE_TIMEOUT=$${SMOKE_TIMEOUT:-90} ./tools/smoke_test.sh

# Host unit tests — deliberately NOT gated on the cross toolchain.
test:
	@./test.sh

clean:
	@./clean.sh

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
#   make test       run the host unit-test suite (uses host gcc, no toolchain)
#   make clean      remove build artifacts
#
# The cross toolchain (i686-elf-*) is required for everything EXCEPT `make test`,
# which compiles the kernel sources with the host gcc.

HOST ?= $(shell ./default-host.sh)

.PHONY: all build headers iso disk run smoke test clean check-toolchain

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

run: check-toolchain
	@./qemu.sh

smoke: check-toolchain
	@./tools/smoke_test.sh

# Host unit tests — deliberately NOT gated on the cross toolchain.
test:
	@./test.sh

clean:
	@./clean.sh

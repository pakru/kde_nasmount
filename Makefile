# nasmount — conventional `make` / `make install` front end for the CMake build.
#
# `install`/`uninstall` delegate to install.sh/uninstall.sh rather than
# reimplementing them: those scripts gate installation on the test suite
# passing, detect and remove a previous org.kde.nasmount (transient-design)
# install, refresh Dolphin's and System Settings' KCM cache, and enable the
# session supervisor. A bare `cmake --install` would skip all of that.

BUILD_DIR := build
DIST_DIR ?= dist
PREFIX ?= /usr
JOBS ?= $(shell nproc)

.PHONY: all build configure test install uninstall clean deb rpm packages dist-clean

all: build

configure:
	cmake -S . -B $(BUILD_DIR) \
	      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	      -DCMAKE_INSTALL_PREFIX=$(PREFIX) \
	      -DNASMOUNT_PACKAGE_FAMILY=source \
	      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# Not `sudo make install`: install.sh itself elevates only for the `cmake
# --install` step, and refuses that step as root (see the script). Run it
# plain; it will prompt for a password only when it actually needs one.
install:
	./install.sh

uninstall:
	./uninstall.sh

# Native packages. These deliberately do *not* build on the host: AGENTS.md
# requires each package to be produced on its matching target distribution, and
# build-deb.sh/build-rpm.sh refuse to run anywhere else. The helper runs the
# same pinned images, dependency lists, and build scripts that CI uses, so a
# failure here is a failure CI would have had.
#
# Requires podman or docker; override with NASMOUNT_CONTAINER_ENGINE=. On the
# matching distribution you can skip the container entirely and run
# ./packaging/build-deb.sh (or build-rpm.sh) against an empty directory.
#
# Each target clears only its own subdirectory of $(DIST_DIR), because the
# build scripts require an empty output directory and would otherwise refuse
# every run after the first.
deb:
	rm -rf $(DIST_DIR)/deb
	./packaging/build-in-container.sh deb $(DIST_DIR)/deb

rpm:
	rm -rf $(DIST_DIR)/rpm
	./packaging/build-in-container.sh rpm $(DIST_DIR)/rpm

packages: deb rpm

dist-clean:
	rm -rf $(DIST_DIR)

clean:
	rm -rf $(BUILD_DIR)

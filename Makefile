# SimCity 3DS Port — Build & Test Helpers

DOCKER_IMAGE ?= devkitpro/devkitarm
WORK_DIR     ?= $(PWD)
DBGK_HOST_TEST ?= 0
# Upstream must be the repository root; the build mounts it and CMake appends
# /static-recomp. The old .../PC/scssr/... path never existed on this machine.
UPSTREAM     ?= /home/seyon/dev/Games/PC/SimCity-SNES-Static-Recomp
ROM_PATH     ?= $(HOME)/SimCity(USA).sfc
# DS was abandoned: the core is 2.6x over the ARM9 3.5 MB limit
# (docs/3DS-PORT-STUDY.md). Default to the platform we actually build.
PLATFORM     ?= 3DS

# devkitPro ships the toolchain files under /opt/devkitpro/cmake, NOT under
# $(DEVKITARM)/cmake/Platform. The old path does not exist in the image.
ifeq ($(PLATFORM),3DS)
  TOOLCHAIN ?= /opt/devkitpro/cmake/3DS.cmake
else
  TOOLCHAIN ?= /opt/devkitpro/cmake/NDS.cmake
endif

.PHONY: all docker-build docker-clean docker-run check doctor setup \
        ds-build ds-test-headless ds-rom-size \
        3ds-build 3ds-build-native 3ds-test-headless 3ds-rom-size \
        docker-run doctor clean three three-build three-upload three-restart three-debug three-custom three-ci

all: docker-build

# ── Build inside Docker ──────────────────────────────────────────────

docker-build:
	@echo "Building SimCity$(PLATFORM) for $(PLATFORM) inside $(DOCKER_IMAGE)..."
	@echo "  toolchain: $(TOOLCHAIN)"
	docker run --rm \
		-v "$(UPSTREAM):/upstream:ro" \
		-v "$(WORK_DIR):/work" \
		-w /work \
		$(DOCKER_IMAGE) \
		bash -c '\
			set -e -o pipefail; \
			export DEVKITARM=/opt/devkitpro/devkitARM; \
			export PATH=$$DEVKITARM/bin:$$PATH; \
			mkdir -p build; cd build; \
			cmake .. \
				-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) \
				-DCMAKE_BUILD_TYPE=Release \
				-DUPSTREAM_SRC_DIR=/upstream/static-recomp \
				-DPLATFORM=$(PLATFORM) \
				-DDBGK_HOST_TEST=$(DBGK_HOST_TEST) \
				-DDS3_USE_NATIVE_RENDER=$(DS3_USE_NATIVE_RENDER); \
			make -j$$(nproc) 2>&1 | tail -100'

# Build with GPU-native tile renderer (replaces CPU PPU render).
docker-build-native:
	@echo "Building SimCity3DS with GPU-native renderer inside $(DOCKER_IMAGE)..."
	$(MAKE) docker-build DS3_USE_NATIVE_RENDER=1

# ── Quick native build (requires devkitARM installed) ───────────────
ds-build:
	@if ! command -v arm-none-eabi-gcc &>/dev/null; then \
		echo "arm-none-eabi-gcc not found. Use 'make docker-build' or install devkitARM."; \
		exit 1; \
	fi
	mkdir -p build && cd build && \
		cmake .. -DCMAKE_BUILD_TYPE=Release -DPLATFORM=DS && \
		make -j$(nproc)

3ds-build:
	@if ! command -v arm-none-eabi-gcc &>/dev/null; then \
		echo "arm-none-eabi-gcc not found. Use 'make docker-build' or install devkitARM."; \
		exit 1; \
	fi
	mkdir -p build && cd build && \
		cmake .. -DCMAKE_BUILD_TYPE=Release -DPLATFORM=3DS && \
		make -j$(nproc)

# Build with GPU-native tile renderer (replaces CPU PPU render with GPU tilemap rendering).
# Use: make 3ds-build-native [DS3_USE_NATIVE_RENDER=1]
3ds-build-native:
	@if ! command -v arm-none-eabi-gcc &>/dev/null; then \
		echo "arm-none-eabi-gcc not found. Use 'make docker-build-native' or install devkitARM."; \
		exit 1; \
	fi
	mkdir -p build && cd build && \
		cmake .. -DCMAKE_BUILD_TYPE=Release -DPLATFORM=3DS -DDS3_USE_NATIVE_RENDER=1 && \
		make -j$(nproc)

# ── Run headless test inside Docker ──────────────────────────────────

ds-test-headless:
	@echo "Running headless test inside $(DOCKER_IMAGE)..."
	docker run --rm \
		-v "$(UPSTREAM):/upstream:ro" \
		-v "$(WORK_DIR):/work" \
		-v "$(ROM_PATH):/rom:ro" \
		-w /work/build \
		$(DOCKER_IMAGE) \
		bash -c '\
			set -e -o pipefail; \
			export DEVKITARM=/opt/devkitpro/devkitARM; \
			export PATH=$$DEVKITARM/bin:$$PATH; \
			./simcity-headless-test /rom 300' 

3ds-test-headless:
	@echo "Running 3DS headless test inside $(DOCKER_IMAGE)..."
	docker run --rm \
		-v "$(UPSTREAM):/upstream:ro" \
		-v "$(WORK_DIR):/work" \
		-v "$(ROM_PATH):/rom:ro" \
		-w /work/build \
		$(DOCKER_IMAGE) \
		bash -c '\
			set -e -o pipefail; \
			export DEVKITARM=/opt/devkitpro/devkitARM; \
			export PATH=$$DEVKITARM/bin:$$PATH; \
			./simcity-headless-test /rom 300' 

# ── Check ROM size of output .nds / .3dsx ────────────────────────────

ds-rom-size: ds-build
	@ls -la build/*.nds 2>/dev/null || echo "No .nds produced yet. Run 'make ds-build'."

3ds-rom-size: 3ds-build
	@ls -la build/*.3dsx 2>/dev/null || echo "No .3dsx produced yet. Run 'make 3ds-build'."

# ── Autonomous 3DS toolchain (three.py) ──────────────────────────────

# Full pipeline: build → upload → restart → debug
three: three-build three-upload three-restart three-debug

# Individual phases
three-build:
	@echo "=== Building ==="
	python3 tools/three.py --host $(THREE_HOST) --port $(THREE_PORT) --ftp-port $(THREE_FTP_PORT) --build

three-upload: three-build
	@echo "=== Uploading ==="
	python3 tools/three.py --host $(THREE_HOST) --port $(THREE_PORT) --ftp-port $(THREE_FTP_PORT) --upload

three-restart: three-upload
	@echo "=== Restarting ==="
	python3 tools/three.py --host $(THREE_HOST) --port $(THREE_PORT) --ftp-port $(THREE_FTP_PORT) --restart

three-debug: three-restart
	@echo "=== Debug commands ==="
	python3 tools/three.py --host $(THREE_HOST) --port $(THREE_PORT) --ftp-port $(THREE_FTP_PORT) --debug --cmd "prof" --cmd "state" --cmd "info"

# Full pipeline with custom debug commands
three-custom:
	@python3 tools/three.py --host $(THREE_HOST) --port $(THREE_PORT) --ftp-port $(THREE_FTP_PORT) --build --upload --restart --debug $(THREE_CMDS)

# CI mode: JSON output, exit codes
three-ci:
	@python3 tools/three.py --host $(THREE_HOST) --port $(THREE_PORT) --ftp-port $(THREE_FTP_PORT) --build --upload --restart --debug --cmd "prof" --json

# Configuration (override via env or make THREE_HOST=...)
THREE_HOST ?= 10.0.0.130
THREE_PORT ?= 5002
THREE_FTP_PORT ?= 5000
THREE_CMDS ?= --cmd "prof" --cmd "state"

# ── Emulator run ─────────────────────────────────────────────────────

docker-run:
	@echo "Launching melonDS with the built .nds..."
	docker run --rm \
		-v "$(WORK_DIR)/build:/roms" \
		-v "$(HOME)/.melonDS:/config" \
		--device /dev/snd \
		$(DOCKER_IMAGE) \
		bash -c " \
			if [ -f /roms/simcity.nds ]; then \
				echo 'Running melonDS...'; \
				echo 'Use melonDS manually: melonDS /path/to/simcity.nds'; \
			else \
				echo 'No .nds found. Run make docker-build first.'; \
				exit 1; \
			fi"

# ── Doctor ───────────────────────────────────────────────────────────

doctor:
	@echo "=== Docker ===" && docker --version 2>&1 || true
	@echo "=== devkitpro image ==="
	@docker images $(DOCKER_IMAGE) 2>/dev/null || echo "  NOT INSTALLED"
	@echo "=== Upstream source ===" && \
	if [ -d "$(UPSTREAM)/static-recomp" ]; then \
		echo "  OK: $(UPSTREAM)/static-recomp exists"; \
	else \
		echo "  MISSING: $(UPSTREAM)/static-recomp"; \
	fi
	@echo "=== ROM ===" && \
	if [ -f "$(ROM_PATH)" ]; then \
		echo "  Found: $(ROM_PATH) ($$(stat -c%s "$(ROM_PATH)" 2>/dev/null || stat -f%z "$(ROM_PATH)" 2>/dev/null) bytes)"; \
	else \
		echo "  MISSING: $(ROM_PATH) — please place the exact SimCity(USA).sfc here."; \
	fi
	@echo "=== Host arm-none-eabi toolchain ===" && \
	(command -v arm-none-eabi-gcc &>/dev/null && echo "  Found at $$(which arm-none-eabi-gcc)" || \
	 echo "  Not found — will use Docker build.")
	@echo "=== Emulator host policy ===" && \
	EMU=$$(command -v azahar 2>/dev/null; command -v citra 2>/dev/null; ls -d ~/Applications/azahar.AppImage 2>/dev/null); \
	if [ -n "$$EMU" ]; then \
		echo "  VIOLATION: 3DS emulator found on this host:"; echo "$$EMU" | sed 's/^/    /'; \
		echo "  Azahar must run on the Steam Deck ONLY. This host is for building"; \
		echo "  and source work; it has the RAM/CPU headroom to stall a timed run."; \
		exit 1; \
	else \
		echo "  OK: no 3DS emulator on this host (correct)"; \
	fi; \
	if timeout 5 bash -c 'echo >/dev/tcp/deck@steamdeck/22' 2>/dev/null || \
	   ssh -o BatchMode=yes -o ConnectTimeout=5 deck@steamdeck true 2>/dev/null; then \
		echo "  OK: Steam Deck reachable — sanctioned emulator runner"; \
	else \
		echo "  WARN: Steam Deck unreachable — emulator runs are blocked until it is"; \
	fi

# ── Clean ────────────────────────────────────────────────────────────

clean:
	rm -rf build/ build3ds/ objs3ds/ *.nds *.3dsx

setup:
	@echo "Ensure Docker is running and the devkitpro image is pulled:"
	@docker pull $(DOCKER_IMAGE) 2>&1 | tail -3
	@echo ""
	@echo "Place SimCity(USA).sfc at: $(ROM_PATH)"
	@echo "Then run: make docker-build            # 3DS (default)"
	@echo "     or: make docker-build PLATFORM=DS # DS (blocked: over memory budget)"
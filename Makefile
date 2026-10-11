# SimCity DS/3DS Port — Build & Test Helpers

DOCKER_IMAGE ?= devkitpro/devkitarm
WORK_DIR     ?= $(PWD)
UPSTREAM     ?= /home/seyon/dev/Games/PC/scssr/SimCity-SNES-Static-Recomp
ROM_PATH     ?= $(HOME)/SimCity(USA).sfc
PLATFORM     ?= DS

.PHONY: all docker-build docker-clean docker-run check doctor setup \
        ds-build ds-test-headless ds-rom-size \
        3ds-build 3ds-test-headless 3ds-rom-size

all: docker-build

# ── Build inside Docker ──────────────────────────────────────────────

docker-build:
	@echo "Building SimCityDS for $(PLATFORM) inside $(DOCKER_IMAGE)..."
	docker run --rm \
		-v "$(UPSTREAM):/upstream:ro" \
		-v "$(WORK_DIR):/work" \
		-w /work \
		$(DOCKER_IMAGE) \
		bash -c "\
			export DEVKITARM=/opt/devkitpro/devkitARM && \
			export PATH=\$DEVKITARM/bin:\$PATH && \
			mkdir -p build && cd build && \
			cmake .. \
				-DCMAKE_TOOLCHAIN_FILE=\$$DEVKITARM/cmake/Platform/Nintendo$(if $(filter 3DS,$(PLATFORM)),3DS,DS).cmake \
				-DCMAKE_BUILD_TYPE=Release \
				-DUPSTREAM_SRC_DIR=/upstream/static-recomp \
				-DPLATFORM=$(PLATFORM) && \
			make -j\$(nproc) 2>&1 | tail -100"

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

# ── Run headless test inside Docker ──────────────────────────────────

ds-test-headless:
	@echo "Running headless test inside $(DOCKER_IMAGE)..."
	docker run --rm \
		-v "$(UPSTREAM):/upstream:ro" \
		-v "$(WORK_DIR):/work" \
		-v "$(ROM_PATH):/rom:ro" \
		-w /work/build \
		$(DOCKER_IMAGE) \
		bash -c " \
			export DEVKITARM=/opt/devkitpro/devkitARM && \
			export PATH=\$DEVKITARM/bin:\$PATH && \
			./simcity-headless-test /rom 300"

3ds-test-headless:
	@echo "Running 3DS headless test inside $(DOCKER_IMAGE)..."
	docker run --rm \
		-v "$(UPSTREAM):/upstream:ro" \
		-v "$(WORK_DIR):/work" \
		-v "$(ROM_PATH):/rom:ro" \
		-w /work/build \
		$(DOCKER_IMAGE) \
		bash -c " \
			export DEVKITARM=/opt/devkitpro/devkitARM && \
			export PATH=\$DEVKITARM/bin:\$PATH && \
			./simcity-headless-test /rom 300"

# ── Check ROM size of output .nds / .3dsx ────────────────────────────

ds-rom-size: ds-build
	@ls -la build/*.nds 2>/dev/null || echo "No .nds produced yet. Run 'make ds-build'."

3ds-rom-size: 3ds-build
	@ls -la build/*.3dsx 2>/dev/null || echo "No .3dsx produced yet. Run 'make 3ds-build'."

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

# ── Clean ────────────────────────────────────────────────────────────

clean:
	rm -rf build/ build3ds/ objs3ds/ *.nds *.3dsx

setup:
	@echo "Ensure Docker is running and the devkitpro image is pulled:"
	@docker pull $(DOCKER_IMAGE) 2>&1 | tail -3
	@echo ""
	@echo "Place SimCity(USA).sfc at: $(ROM_PATH)"
	@echo "Then run: make docker-build PLATFORM=DS"
	@echo "       or: make docker-build PLATFORM=3DS"
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0

SHELL := /bin/bash
.DEFAULT_GOAL := build
# Craft changes a shared prefix. Compiler parallelism is controlled by JOBS.
.NOTPARALLEL:

PYTHON ?= python3
BUILD_DIR ?= $(CURDIR)/build-macos
PREFIX ?= $(BUILD_DIR)/craft-root
JOBS ?= 4
DEVELOPER_DIR ?= $(shell xcode-select -p 2>/dev/null)
export DEVELOPER_DIR
TAG ?=
export TAG

PACKAGE_DIR ?= $(BUILD_DIR)/package
ASSETS_DIR ?= $(BUILD_DIR)/release-assets
DEPENDENCIES ?= $(BUILD_DIR)/dependencies.json
TEST_BUILD ?= $(PREFIX)/build/kwi/kwi/work/build
RESULTS_DIR ?= $(BUILD_DIR)/results
APP ?= $(BUILD_DIR)/stage/Kwi.app
ARGS ?=
EXPECTED_COMMIT ?=
export EXPECTED_COMMIT

CRAFT = "$(PYTHON)" scripts/macos/craft.py --prefix "$(PREFIX)"
CTEST = "$(PREFIX)/dev-utils/bin/ctest" --test-dir "$(TEST_BUILD)" --output-on-failure --no-tests=error

.PHONY: all help doctor prepare dependencies configure build check lint test-tools test-native test test-gui \
        sources record-dependencies bundle run package release-assets verify-release validate-release release ci \
        workloads smoke startup benchmark memory compatibility performance presentation screenshot ci-changes upstream clean update-cask install-release sync-cask

all: build

help: ## List commands and configuration
	@awk 'BEGIN {FS = ":.*## "; print "Usage: make [target] [VARIABLE=value]\n"} /^[a-zA-Z_-]+:.*## / {printf "  %-22s %s\n", $$1, $$2}' Makefile
	@printf '\nVariables: PYTHON, DEVELOPER_DIR, JOBS, BUILD_DIR, PREFIX, APP, PACKAGE_DIR,\n           ASSETS_DIR, DEPENDENCIES, TEST_BUILD, RESULTS_DIR, TAG, EXPECTED_COMMIT, ARGS\n'

doctor: ## Verify the native Mac toolchain
	@test "$$(uname -s)" = Darwin && test "$$(uname -m)" = arm64 || { echo 'Builds require an Apple Silicon Mac'; exit 1; }
	@test -x "$(DEVELOPER_DIR)/usr/bin/xcodebuild" || { echo 'Set DEVELOPER_DIR to full Xcode Contents/Developer'; exit 1; }
	@"$(PYTHON)" -c 'import sys; assert sys.version_info >= (3, 11), "Python 3.11 or later is required"'
	@xcodebuild -version
	@sw_vers

prepare: doctor ## Prepare the pinned Craft prefix
	$(CRAFT) --jobs "$(JOBS)" prepare

dependencies: prepare ## Install pinned application dependencies
	$(CRAFT) run --install-deps kwi/kwi

configure: dependencies ## Configure the application
	$(CRAFT) run --configure kwi/kwi

build: configure ## Compile and install into the local prefix (default)
	$(CRAFT) run --make --install --post-install --qmerge --post-qmerge kwi/kwi

test-tools: ## Test the Python build, packaging and measurement helpers
	"$(PYTHON)" -m unittest discover -s tests/macos -v

lint: ## Check whitespace
	git diff --check

check: test-tools lint ## Run host-independent checks

test-native: build ## Build and run headless native tests
	@mkdir -p "$(BUILD_DIR)"
	$(CTEST) -LE gui --output-junit "$(BUILD_DIR)/native-tests.xml" $(ARGS)

test: check test-native ## Run tooling and headless native tests

test-gui: workloads build ## Run GUI suites in a focused Mac session
	$(CTEST) -L gui $(ARGS)

sources: ## Download and checksum corresponding dependency sources
	"$(PYTHON)" scripts/macos/source_bundle.py

record-dependencies: build ## Audit installed dependencies and save their provenance
	"$(PYTHON)" scripts/macos/record-dependencies.py --prefix "$(PREFIX)" --output "$(DEPENDENCIES)"

bundle: build ## Deploy and sign a standalone development app
	"$(PYTHON)" scripts/macos/bundle.py --prefix "$(PREFIX)" --output "$(APP)" $(ARGS)

run: bundle ## Build, deploy and open Kwi
	open "$(APP)"

package: doctor sources test-native record-dependencies ## Build, test and verify the distributable app and sources
	@mkdir -p "$(BUILD_DIR)"
	@set -euo pipefail; "$(PYTHON)" scripts/macos/package.py --prefix "$(PREFIX)" --output "$(PACKAGE_DIR)" 2>&1 | tee "$(BUILD_DIR)/package.log"

release-assets: package ## Assemble checksummed release downloads
	"$(PYTHON)" scripts/macos/package.py assets --package-dir "$(PACKAGE_DIR)" --dependencies "$(DEPENDENCIES)" --output "$(ASSETS_DIR)"

verify-release: ## Verify an existing release asset directory and its source commit
	"$(PYTHON)" scripts/macos/package.py verify --output "$(ASSETS_DIR)"

validate-release: ## Check TAG against the project version and checked-out commit
	"$(PYTHON)" scripts/macos/package.py validate-tag

release: validate-release verify-release ## Create or refresh a draft from existing verified assets
	"$(PYTHON)" scripts/macos/package.py draft --output "$(ASSETS_DIR)"

ci: check release-assets ## Run the complete CI build, tests and packaging pipeline

update-cask: ## Update Homebrew's version and checksum after publishing TAG
	"$(PYTHON)" scripts/macos/update-cask.py --tag "$(TAG)" $(ARGS)

sync-cask: ## Open a GitHub-signed cask update PR for the latest stable release
	"$(PYTHON)" scripts/macos/update-cask.py --pull-request

install-release: ## Install or update the latest published app, without building
	bash install.sh $(ARGS)

ci-changes: ## Classify CI changes; docs and distribution skip the native build
	@set -euo pipefail; \
	build=true; base="$${CI_BASE:-}"; head="$${CI_HEAD:-HEAD}"; \
	if [ "$${CI_EVENT:-}" = cask ]; then \
	  [[ "$${GITHUB_REF_NAME:-}" =~ ^automation/homebrew-v[0-9]+\.[0-9]+\.[0-9]+$$ ]] || { echo 'Cask checks require an automation branch' >&2; exit 1; }; \
	  git merge-base --is-ancestor HEAD^ origin/main || { echo 'Cask parent must already be on main' >&2; exit 1; }; \
	  [ "$$(git diff --name-only --no-renames HEAD^ HEAD --)" = Casks/kwi-mac.rb ] || { echo 'Cask checks require exactly one cask file change' >&2; exit 1; }; \
	fi; \
	if [ "$${CI_EVENT:-}" != workflow_dispatch ] && [ "$${CI_IS_TAG:-false}" != true ] \
	   && [ -n "$$base" ] && [ "$$base" != 0000000000000000000000000000000000000000 ]; then \
	  changed=$$(git diff --name-only --no-renames "$$base" "$$head" --); \
	  build=false; \
	  while IFS= read -r path; do \
	    case "$$path" in \
	      ''|*.md|*.md.license|docs/*|assets/readme/*|install.sh|Casks/*|scripts/macos/update-cask.py|tests/macos/test_installer.py|tests/macos/test_cask.py) ;; \
	      *) build=true; break ;; \
	    esac; \
	  done <<< "$$changed"; \
	fi; \
	printf 'build=%s\n' "$$build"; \
	if [ -n "$${GITHUB_OUTPUT:-}" ]; then printf 'build=%s\n' "$$build" >> "$$GITHUB_OUTPUT"; fi

workloads: ## Generate deterministic GUI/performance fixtures
	"$(PYTHON)" scripts/macos/workloads.py --output "$(CURDIR)/build-macos/workloads"

smoke: ## Smoke-test an existing APP with an isolated profile
	"$(PYTHON)" scripts/macos/smoke.py --app "$(APP)" --output "$(RESULTS_DIR)/smoke.json" --isolated-profile $(ARGS)

startup: ## Measure startup of an existing APP (requires no running Kwi)
	"$(PYTHON)" scripts/macos/startup.py --app "$(APP)" --output "$(RESULTS_DIR)/startup" --child-helper "$(TEST_BUILD)/bin/test-macos-startup-child" $(ARGS)

benchmark: ## Benchmark an existing APP; supply workload/options in ARGS
	"$(PYTHON)" scripts/macos/benchmark.py --app "$(APP)" --terminal kwi --output "$(RESULTS_DIR)/benchmark" $(ARGS)

memory: ## Run the isolated app memory campaign
	"$(PYTHON)" scripts/macos/memory.py --app "$(APP)" --output "$(RESULTS_DIR)/memory" --metrics-tool "$(TEST_BUILD)/bin/test-native-process.app/Contents/MacOS/test-native-process" $(ARGS)

compatibility: ## Check CLI, presets and activation of an existing APP
	"$(PYTHON)" scripts/macos/compatibility.py --app "$(APP)" --output "$(RESULTS_DIR)/compatibility" $(ARGS)

performance: build workloads ## Run focused native performance checks; select methods in ARGS
	"$(PYTHON)" scripts/macos/run-gui-test.py --executable "$(TEST_BUILD)/bin/test-macos-performance.app/Contents/MacOS/test-macos-performance" --output "$(RESULTS_DIR)/performance" $(ARGS)

presentation: ## Analyze performance presentation traces
	"$(PYTHON)" scripts/macos/presentation.py --directory "$(RESULTS_DIR)/performance" $(ARGS)

screenshot: build ## Capture a private demo window for the README
	@set -euo pipefail; mkdir -p "$(RESULTS_DIR)"; \
	capture=$$(mktemp -d "$(RESULTS_DIR)/readme.XXXXXX"); \
	"$(PYTHON)" scripts/macos/run-gui-test.py --executable "$(TEST_BUILD)/bin/test-macos-interaction.app/Contents/MacOS/test-macos-interaction" --output "$$capture/session" --timeout 45 -- readmeScreenshot; \
	mkdir -p assets/readme; \
	cp "$$capture/session/readme.png" assets/readme/screenshot.png

upstream: ## Fetch KDE master and report unmerged changes
	@set -euo pipefail; \
	git remote get-url upstream >/dev/null 2>&1 || git remote add upstream https://invent.kde.org/utilities/kwi.git; \
	git fetch --no-tags upstream master; \
	base=$$(git merge-base HEAD FETCH_HEAD); \
	count=$$(git rev-list --count HEAD..FETCH_HEAD); \
	mkdir -p "$(BUILD_DIR)/upstream-report"; \
	git log --format='%h %s' HEAD..FETCH_HEAD > "$(BUILD_DIR)/upstream-report/commits.txt"; \
	git diff --stat "$$base" FETCH_HEAD > "$(BUILD_DIR)/upstream-report/changes.txt"; \
	{ printf '## KDE Kwi upstream\n\nUnmerged commits: %s\nLast common commit: %s\n\n```text\n' "$$count" "$$base"; \
	  cat "$(BUILD_DIR)/upstream-report/commits.txt"; \
	  printf '\n```\nReview and merge upstream/master on a sync branch.\n'; \
	} > "$(BUILD_DIR)/upstream-report/summary.md"; \
	cat "$(BUILD_DIR)/upstream-report/summary.md"; \
	if [ -n "$${GITHUB_STEP_SUMMARY:-}" ]; then cat "$(BUILD_DIR)/upstream-report/summary.md" >> "$$GITHUB_STEP_SUMMARY"; fi

clean: doctor ## Remove application build output; retain dependencies and downloads
	$(CRAFT) run --clean kwi/kwi

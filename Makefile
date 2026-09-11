# ==========================================
# KONFIGURASI DIREKTORI & BERKAS
# ==========================================
PROJECT_DIR ?= .
PATCH_DIR   ?= .
PATCH_FILE  ?= $(PATCH_DIR)/filepatch.patch
PATCH_STRIP ?= auto
PATCH_WHITESPACE ?= warn
AUTO_COMMIT ?= 0
COMMIT_MSG  ?= Menerapkan patch otomatis via Makefile

PATCH_PATH := $(abspath $(PATCH_FILE))
PROJECT_PATH := $(abspath $(PROJECT_DIR))

# ==========================================
# TARGET
# ==========================================
.PHONY: all apply apply-safe rollback reverse clean status help validate \
        check-directory check-git check-patch check-worktree validate-patch \
        debug commit

all: help

apply: check-directory check-git check-patch check-worktree validate-patch apply-safe

apply-safe:
	@set -e; \
	cd "$(PROJECT_PATH)"; \
	if ! git diff --cached --quiet; then \
		echo "❌ Index memiliki perubahan staged; hentikan agar commit tidak tercampur."; exit 1; \
	fi; \
	if ! git diff --quiet; then \
		echo "❌ Worktree memiliki perubahan tracked; hentikan agar apply/rollback deterministik."; \
		echo "   File untracked tetap aman dan tidak akan disentuh."; exit 1; \
	fi; \
	STRIP=""; \
	case "$(PATCH_STRIP)" in \
		auto) for P in 1 0 2 3; do if git apply -p$$P --check --whitespace="$(PATCH_WHITESPACE)" "$(PATCH_PATH)" >/dev/null 2>&1; then STRIP="$$P"; break; fi; done ;; \
		0|1|2|3) STRIP="$(PATCH_STRIP)" ;; \
		p0|p1|p2|p3) STRIP="$${PATCH_STRIP#p}" ;; \
		*) echo "❌ PATCH_STRIP harus auto, 0..3, atau p0..p3"; exit 2 ;; \
	esac; \
	if [ -z "$$STRIP" ]; then echo "❌ Patch tidak cocok pada -p0 sampai -p3"; exit 1; fi; \
	echo "🔧 Menggunakan -p$$STRIP"; \
	git apply -p$$STRIP --check --whitespace="$(PATCH_WHITESPACE)" "$(PATCH_PATH)"; \
	git apply -p$$STRIP --index --whitespace="$(PATCH_WHITESPACE)" "$(PATCH_PATH)"; \
	echo "✅ Patch diterapkan dan di-stage."; \
	git diff --cached --check; \
	git diff --cached --stat
	@echo ""
	@echo "AUTO_COMMIT=$(AUTO_COMMIT)"
	@if [ "$(AUTO_COMMIT)" = "1" ]; then $(MAKE) --no-print-directory commit; else echo "ℹ️  Commit otomatis nonaktif. Jalankan 'make commit' setelah meninjau diff."; fi

commit: check-directory check-git
	@set -e; \
	cd "$(PROJECT_PATH)"; \
	if git diff --cached --quiet; then echo "⚠️  Tidak ada perubahan staged."; exit 0; fi; \
	git diff --cached --check; \
	git diff --cached --stat; \
	git commit -m "$(COMMIT_MSG)"; \
	echo "✅ Commit berhasil: $$(git rev-parse --short HEAD)"

check-directory:
	@if [ ! -d "$(PROJECT_PATH)" ]; then echo "❌ Direktori project tidak ditemukan: $(PROJECT_PATH)"; exit 1; fi

check-git:
	@cd "$(PROJECT_PATH)" && git rev-parse --show-toplevel >/dev/null 2>&1 || { echo "❌ PROJECT_DIR bukan repository Git: $(PROJECT_PATH)"; exit 1; }

check-patch:
	@if [ ! -f "$(PATCH_PATH)" ]; then echo "❌ File patch tidak ditemukan: $(PATCH_PATH)"; exit 1; fi
	@if [ ! -s "$(PATCH_PATH)" ]; then echo "❌ File patch kosong: $(PATCH_PATH)"; exit 1; fi
	@echo "✅ Patch ditemukan: $(PATCH_PATH)"

check-worktree:
	@cd "$(PROJECT_PATH)" && \
	if ! git diff --cached --quiet; then echo "❌ Ada perubahan staged. Commit/stash dahulu."; exit 1; fi; \
	if ! git diff --quiet; then echo "❌ Ada perubahan tracked yang belum di-stage. Commit/stash dahulu."; exit 1; fi; \
	echo "✅ Worktree tracked bersih; untracked tidak disentuh."

validate-patch: check-directory check-git check-patch check-worktree
	@set -e; \
	cd "$(PROJECT_PATH)"; \
	STRIP=""; \
	case "$(PATCH_STRIP)" in \
		auto) for P in 1 0 2 3; do if git apply -p$$P --check --whitespace="$(PATCH_WHITESPACE)" "$(PATCH_PATH)" >/dev/null 2>&1; then STRIP="$$P"; break; fi; done ;; \
		0|1|2|3) STRIP="$(PATCH_STRIP)" ;; \
		p0|p1|p2|p3) STRIP="$${PATCH_STRIP#p}" ;; \
		*) echo "❌ PATCH_STRIP tidak valid"; exit 2 ;; \
	esac; \
	if [ -z "$$STRIP" ]; then echo "❌ Patch tidak valid pada -p0 sampai -p3"; exit 1; fi; \
	echo "✅ Patch valid dengan -p$$STRIP"

validate: validate-patch

rollback:
	@set -e; \
	cd "$(PROJECT_PATH)"; \
	if git diff --cached --quiet; then echo "ℹ️  Tidak ada perubahan staged; rollback tidak melakukan apa pun."; exit 0; fi; \
	if ! git diff --quiet; then echo "❌ Worktree memiliki perubahan tracked; rollback dibatalkan agar tidak menimpa pekerjaan lokal."; exit 1; fi; \
	STRIP=""; \
	case "$(PATCH_STRIP)" in \
		auto) for P in 1 0 2 3; do if git apply -R -p$$P --check --index --whitespace="$(PATCH_WHITESPACE)" "$(PATCH_PATH)" >/dev/null 2>&1; then STRIP="$$P"; break; fi; done ;; \
		0|1|2|3) STRIP="$(PATCH_STRIP)" ;; \
		p0|p1|p2|p3) STRIP="$${PATCH_STRIP#p}" ;; \
		*) echo "❌ PATCH_STRIP tidak valid"; exit 2 ;; \
	esac; \
	if [ -z "$$STRIP" ]; then echo "❌ Reverse patch tidak cocok; tidak ada perubahan dilakukan."; exit 1; fi; \
	git apply -R -p$$STRIP --check --index --whitespace="$(PATCH_WHITESPACE)" "$(PATCH_PATH)"; \
	git apply -R -p$$STRIP --index --whitespace="$(PATCH_WHITESPACE)" "$(PATCH_PATH)"; \
	echo "✅ Patch di-reverse dengan -p$$STRIP. Tidak ada file untracked yang dihapus."

reverse: rollback

clean:
	@echo "ℹ️  Target clean V2 bersifat non-destruktif dan tidak menghapus file apa pun."
	@echo "   Tidak ada git clean, rm, atau find -delete yang dijalankan."

status:
	@echo "Project : $(PROJECT_PATH)"
	@echo "Patch   : $(PATCH_PATH)"
	@cd "$(PROJECT_PATH)" && { git status --short; echo "--- PATCH STAT ---"; git apply --stat "$(PATCH_PATH)" 2>/dev/null || true; }

debug:
	@echo "Patch : $(PATCH_PATH)"
	@echo "Strip : $(PATCH_STRIP)"
	@echo "Whitespace : $(PATCH_WHITESPACE)"
	@echo "Auto commit : $(AUTO_COMMIT)"
	@echo "--- git status ---"
	@cd "$(PROJECT_PATH)" && git status --short 2>/dev/null || true
	@echo "--- strip checks ---"
	@cd "$(PROJECT_PATH)" && for P in 0 1 2 3; do echo "[p$$P]"; git apply -p$$P --check --whitespace="$(PATCH_WHITESPACE)" "$(PATCH_PATH)" 2>&1 || true; done

help:
	@echo "FieldRadio patch automation V2"
	@echo ""
	@echo "make validate PATCH_FILE=filepatch.patch"
	@echo "make apply PATCH_FILE=filepatch.patch"
	@echo "make apply PATCH_STRIP=auto"
	@echo "make apply PATCH_STRIP=p1"
	@echo "make commit COMMIT_MSG='Fix ...'"
	@echo "make rollback PATCH_FILE=filepatch.patch"
	@echo "make debug PATCH_FILE=filepatch.patch"
	@echo ""
	@echo "AUTO_COMMIT=$(AUTO_COMMIT) (default 0; gunakan AUTO_COMMIT=1 untuk commit otomatis)"

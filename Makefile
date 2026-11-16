#
# LunaBerry — a small, modular Lua 5.5.1+ engine with LunaJIT and LunaAOT.
#
#   make            build engine, lunavm/lunaberry, lunac, lunajit, lunaaot
#   make test       run the C API + CLI + JIT/AOT/Berry smoke tests
#   make jit        build and run the JIT demo
#   make native     AOT-compile examples/app_lunaberry.lua (LLVM backend)
#   make berry      AOT-compile the same app to a Berry/BeASM module
#   make clean      remove build/ and dist/
#
# Windows/MinGW and Unix are both supported. No third-party dependencies.
#

CC       ?= gcc
AR       ?= ar
LDLIBS   ?= -lm

ifeq ($(OS),Windows_NT)
EXE := .exe
SHLIB := lunaberry.dll
IMPLIB := -Wl,--out-implib,build/liblunaberry.dll.a
PIC :=
LDLIBS +=
else
EXE :=
SHLIB := liblunaberry.so
IMPLIB :=
PIC := -fPIC
LDLIBS += -ldl
endif

# Optimization ladder for the engine itself (build time).
#   make VM_LADDER=10        engine built with the level-10 flag set
#   make VM_OPT="-O3"        engine built with custom flags
# The runtime CLIs take -O/--opt-level for the shim/wrapper they generate.
VM_LADDER ?=
ifneq ($(VM_LADDER),)
VM_OPT := $(shell sh tools/ladder_flags.sh $(VM_LADDER))
else
VM_OPT ?= -O2
endif

# Section GC only pays off on PE/COFF if the unwind tables are gone: the
# .pdata/.xdata pair relocates back to its .text section, so the linker must
# keep both, and --gc-sections can then never collect a single function.
# Luna uses longjmp for errors, so dropping the unwind info is safe.
# -fno-jump-tables: switch besar (dispatch opcode VM Lua, llex, lparser) pada
# PE/COFF menaruh jump table di .rdata yang digabung linker; satu tabel yang
# hidup bisa menahan seluruh blok .rdata, sehingga --gc-sections tidak bisa
# membuang case block yang mati.  Diukur 2026-11-15 (tier slim): -1.024 B exe,
# -8.092 B .a, tanpa penurunan kecepatan (849 ms vs 843 ms).
GCSECT   := -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-jump-tables
# Perataan fungsi/jump/label ke-1: default gcc menyisipkan padding 16 B
# sebelum tiap fungsi; dihapus -> core lunavm 314.382 -> 304.142 B (-3,3 %)
# tanpa perubahan kecepatan (827 ms vs 837 ms, interleaved best-of-9).
ALIGN    := -falign-functions=1 -falign-jumps=1 -falign-labels=1
CFLAGS   ?= $(VM_OPT) $(GCSECT) $(ALIGN) -fno-ident -std=c99 -Wall -Wextra -I. -Ivendor/lua $(PIC) $(TIER_DEF)
LUAFLAGS ?= $(VM_OPT) $(GCSECT) $(ALIGN) -fno-ident -std=c99 -Ivendor/lua -w $(PIC)
LDFLAGS  ?= -Wl,--gc-sections

# PGO (profile-guided optimization) -- dua tahap, dipakai lewat tier target:
#   make PGO=gen tier-core   -> instrumentasi; lalu jalankan workload supaya
#                                .gcda terkumpul di .pgo/<tier>/
#   make PGO=use tier-core   -> build ulang memakai profil itu
# PGO_DIR, BUILD dan TIER harus SAMA antara tahap gen dan use: nama file
# .gcda diturunkan dari path object, jadi path yang beda = profil tak cocok.
PGO ?=
ifneq ($(PGO),)
PGO_DIR ?= $(CURDIR)/.pgo/$(TIER)
ifeq ($(PGO),gen)
  PGO_CFLAGS := -fprofile-generate -fprofile-dir=$(PGO_DIR)
else ifeq ($(PGO),use)
  # -fno-profile-values WAJIB: tanpa itu gcc menyisipkan referensi
  # __gcov_indirect_call_profiler_v4, jadi liblunaberry.a tak bisa di-link
  # tanpa -lgcov (link Berry host pakai ld.exe langsung pun gagal) dan
  # library statis jadi tidak bisa dipakai consumer tanpa surprise.
  PGO_CFLAGS := -fprofile-use -fprofile-correction -fprofile-dir=$(PGO_DIR) -Wno-missing-profile -fno-profile-values
else
  $(error PGO hanya boleh 'gen' atau 'use')
endif
CFLAGS   += $(PGO_CFLAGS)
LUAFLAGS += $(PGO_CFLAGS)
LDFLAGS  += $(PGO_CFLAGS)
endif

BUILD ?= build
DIST  ?= dist

# ---------------------------------------------------------------------------
# Build tiers (size vs. features)
#
#   make tier-slim   interpreter only: no JIT/AOT backends, -Os, stripped
#   make tier-core   interpreter + JIT(CC backend), -O2
#   make tier-full   JIT(CC/LLVM) + AOT(LLVM/Berry), -O2  (default build)
#
#   Varian ukuran ekstra (VM di -Os -> exe ~12-19% lebih kecil, VM ~13-15%
#   lebih lambat; lihat tier-core-tiny / tier-full-tiny / tier-slim-fast):
#   make tier-slim-fast   slim tier tapi VM di -O2 ( lebih cepat, lebih besar)
#
# Each tier builds into its own directory (build-slim / build-core / build-full)
# so the sizes can be compared side by side.
# ---------------------------------------------------------------------------
TIER ?=
TIER_DEF :=
LUNA_SRCS := luna.c luna-common.c luna-opt.c luna-inline.c \
             luna-jit.c luna-llvm.c luna-berry.c
PLAT_SRCS := platform/berry/luna_berry.c
CLIS      := vm lc lj la
# Strip SEMUA CLI (bukan hanya slim): linker MinGW menyimpan COFF symbol table
# di dalam file .exe, jadi `strip -s` memangkas ~29-30% ukuran file tanpa
# biaya runtime (core lunavm 452.493 -> 317.966 B).  Set STRIP_EXES=0 untuk
# menyimpan simbol (debug/symbolisasi).
STRIP_EXES ?= 1
STRIP     ?= strip
ifneq ($(STRIP_EXES),)
# no '|| true': a silent no-op strip ships an unstripped binary
STRIP_RULE = $(STRIP) -s $@
else
STRIP_RULE = @:
endif

ifeq ($(TIER),slim)
  TIER_DEF := -DLUNA_TIER_SLIM
  VM_OPT := -Os              # GCSECT carries -fno-asynchronous-unwind-tables/-fno-jump-tables
  LUNA_SRCS := luna.c luna-common.c luna-opt.c luna-inline.c luna-stubs.c
  PLAT_SRCS :=
  CLIS := vm
else ifeq ($(TIER),core)
  TIER_DEF := -DLUNA_TIER_CORE
  VM_OPT := -O2
  LUNA_SRCS := luna.c luna-common.c luna-opt.c luna-inline.c luna-jit.c \
               luna-stubs.c
  PLAT_SRCS :=
  CLIS := vm lc lj
endif

LUA_SRC := $(wildcard vendor/lua/*.c)
LUA_OBJ := $(patsubst vendor/lua/%.c,$(BUILD)/lua/%.o,$(LUA_SRC))

LUNA_OBJ := $(patsubst %.c,$(BUILD)/obj/%.o,$(LUNA_SRCS))
LUNA_OBJ += $(patsubst %.c,$(BUILD)/obj/%.o,$(PLAT_SRCS))

LIB := $(BUILD)/liblunaberry.a
VM  := $(BUILD)/lunavm$(EXE)
LB  := $(BUILD)/lunaberry$(EXE)
LC  := $(BUILD)/lunac$(EXE)
LJ  := $(BUILD)/lunajit$(EXE)
LA  := $(BUILD)/lunaaot$(EXE)
BEASM := $(BUILD)/beasm$(EXE)

.PHONY: all lib shared test jit native berry clean install dist \
        tier-slim tier-core tier-full tier-slim-fast tier-core-tiny \
        tier-full-tiny size-report dist-copy dist-pgo \
        tier-slim-pgo tier-core-pgo tier-full-pgo

ALL_TARGETS := $(LIB) $(VM) $(LB)
ifneq (,$(filter lc,$(CLIS)))
ALL_TARGETS += $(LC)
endif
ifneq (,$(filter lj,$(CLIS)))
ALL_TARGETS += $(LJ)
endif
ifneq (,$(filter la,$(CLIS)))
ALL_TARGETS += $(LA)
endif
ifeq ($(TIER),)
ALL_TARGETS += $(BEASM)
endif

all: $(ALL_TARGETS)

lib: $(LIB)

shared: $(BUILD)/$(SHLIB)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/lua/%.o: vendor/lua/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LUAFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/berry/%.o: platform/berry/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(LUA_OBJ) $(LUNA_OBJ)
	@mkdir -p $(BUILD)
	$(AR) rcs $@ $^

$(BUILD)/obj/luna-shared.o: luna.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DLUNA_BUILD_SHARED -c $< -o $@

$(BUILD)/$(SHLIB): $(LUA_OBJ) $(BUILD)/obj/luna-shared.o
	@mkdir -p $(BUILD)
	$(CC) $(LDFLAGS) -shared -o $@ $^ $(IMPLIB) $(LDLIBS)

$(VM): $(BUILD)/obj/luna-vm.o $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)
	$(STRIP_RULE)

$(LB): $(VM)
	cp -f $< $@

$(LC): $(BUILD)/obj/lunac.o $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)
	$(STRIP_RULE)

$(LJ): $(BUILD)/obj/luna-jit-cli.o $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)
	$(STRIP_RULE)

$(LA): $(BUILD)/obj/luna-aot-cli.o $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)
	$(STRIP_RULE)

$(BEASM): tools/beasm/beasm.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -o $@ $<

# Host tool: mengukur CPU time (user+kernel) satu proses via GetProcessTimes.
# Dipakai bench/cpu_micro.sh -- metrik yang kebal beban mesin bersama, karena
# wall-clock di host ini dikontaminasi build agent lain.
CPUTIME := tools/cputime$(EXE)

$(CPUTIME): tools/cputime.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -o $@ $<

$(BUILD)/test_luna$(EXE): tests/test_luna.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)

test: all $(BUILD)/test_luna$(EXE)
	@echo "== C API =="
	@$(BUILD)/test_luna$(EXE)
	@echo "== interpreter =="
	@$(VM) tests/test.lua
	@echo "== compiler =="
	@$(LC) -o $(BUILD)/test.lbc tests/test.lua
	@$(VM) $(BUILD)/test.lbc
	@echo "== JIT (CC) =="
	@$(LJ) --backend cc --target lunavm --run -o $(BUILD)/jit_test examples/fib.lua
	@echo "== AOT (LLVM) =="
	@$(LA) --backend llvm --target both --o3 -o $(BUILD)/aot_test examples/fib.lua
	@$(BUILD)/aot_test$(EXE)
	@echo "== Berry =="
	@$(LA) --backend berry --target both -o $(BUILD)/berry_test examples/fib.lua
	@echo "== Optimization ladder =="
	@sh tests/test_opt.sh
	@echo "== ok =="

jit: all
	$(LJ) --backend llvm --target both --run --stats examples/fib.lua

native: all
	$(LA) --backend llvm --target both --emit-ir --o3 examples/app_lunaberry.lua

berry: all
	$(LA) --backend berry --target both examples/app_lunaberry.lua

bench: all
	sh bench/opt_ladder.sh

bench-backends: all
	sh bench/backends.sh

bench-ladder: all
	sh bench/ladder_all.sh

bench-vm: all
	sh bench/ladder_vm.sh

# Build the three size tiers side by side.
tier-slim:
	$(MAKE) TIER=slim BUILD=build-slim all

tier-core:
	$(MAKE) TIER=core BUILD=build-core all

tier-full:
	$(MAKE) TIER=full BUILD=build-full all

# Varian "-tiny": tier core/full di -Os -> exe ~12% lebih kecil, VM ~16%
# lebih lambat (terukur 2026-11-15, beban fib+sieve).  Pakai kalau ukuran
# lebih penting daripada throughput.
tier-slim-fast:
	$(MAKE) TIER=slim BUILD=build-slim-fast VM_OPT="-O2" all

tier-core-tiny:
	$(MAKE) TIER=core BUILD=build-core-tiny VM_OPT="-Os" all

tier-full-tiny:
	$(MAKE) TIER=full BUILD=build-full-tiny VM_OPT="-Os" all

# PGO end-to-end (gen -> train -> use).  Objek HARUS dihapus di antara dua
# fase: make tidak akan rebuild .o yang lebih baru dari sumber, jadi fase
# 'use' akan memakai objek instrumented yang lama kalau tidak dihapus.
# Terukur 2026-11-16: core lunavm 304.142 -> 273.934 B (-9,9 %) dan 847 ->
# 804 ms (-5,1 %); slim 236.046 -> 237.070 B (+0,4 %) dan 976 -> 951 ms.
tier-slim-pgo:
	$(MAKE) TIER=slim BUILD=build-slim-pgo PGO=gen all
	sh bench/pgo_train.sh build-slim-pgo/lunavm$(EXE) slim
	rm -rf build-slim-pgo/lua build-slim-pgo/obj build-slim-pgo/liblunaberry.a
	$(MAKE) TIER=slim BUILD=build-slim-pgo PGO=use all

tier-core-pgo:
	$(MAKE) TIER=core BUILD=build-core-pgo PGO=gen all
	sh bench/pgo_train.sh build-core-pgo/lunavm$(EXE) core
	rm -rf build-core-pgo/lua build-core-pgo/obj build-core-pgo/liblunaberry.a
	$(MAKE) TIER=core BUILD=build-core-pgo PGO=use all

tier-full-pgo:
	$(MAKE) TIER=full BUILD=build-full-pgo PGO=gen all
	sh bench/pgo_train.sh build-full-pgo/lunavm$(EXE) full
	rm -rf build-full-pgo/lua build-full-pgo/obj build-full-pgo/liblunaberry.a
	$(MAKE) TIER=full BUILD=build-full-pgo PGO=use all

# Print the artifact sizes of every tier that has been built.
size-report:
	@for d in build-slim build-core build-full build-slim-fast build-core-tiny build-full-tiny build; do \
	  if [ -f "$$d/lunavm$(EXE)" ]; then \
	    printf '%-12s lunavm %8s bytes' "$$d" "$$(wc -c < "$$d/lunavm$(EXE)")"; \
	    if [ -f "$$d/liblunaberry.a" ]; then \
	      printf '   lib %8s bytes' "$$(wc -c < "$$d/liblunaberry.a")"; \
	    fi; \
	    printf '\n'; \
	  fi; \
	done

clean:
	-rm -rf $(BUILD) $(DIST)

install: all
	@mkdir -p $(DESTDIR)/bin $(DESTDIR)/lib $(DESTDIR)/include
	cp -f $(VM) $(LB) $(LC) $(LJ) $(LA) $(BEASM) $(DESTDIR)/bin/
	cp -f $(LIB) $(DESTDIR)/bin/
	cp -f $(LIB) $(DESTDIR)/lib/
	cp -f luna.h luna-jit.h luna-llvm.h luna-berry.h luna-opt.h luna-inline.h $(DESTDIR)/include/
	cp -f platform/berry/luna_berry.h $(DESTDIR)/include/luna_berry.h

dist: clean all test dist-copy

# Salin artefak build saat ini ke $(DIST) (dipisah dari 'dist' supaya bisa
# dipakai ulang oleh dist-pgo tanpa memicu 'clean').
dist-copy:
	@mkdir -p $(DIST)/bin $(DIST)/lib $(DIST)/include
	cp -f $(VM) $(LB) $(LC) $(LJ) $(LA) $(DIST)/bin/
	@[ -f $(BEASM) ] && cp -f $(BEASM) $(DIST)/bin/ || true
	cp -f $(LIB) $(DIST)/bin/
	cp -f $(LIB) $(DIST)/lib/
	cp -f luna.h luna-jit.h luna-llvm.h luna-berry.h luna-opt.h luna-inline.h $(DIST)/include/
	cp -f platform/berry/luna_berry.h $(DIST)/include/luna_berry.h
	@echo "LunaBerry distribution is in $(DIST)/"

# Dist WITH PGO (tier full + profile-guided optimization).
# Terukur 2026-11-16: lunavm 329.742 -> 297.998 B (-9,6 %), tanpa perubahan
# kecepatan pada beban yang sama.
dist-pgo:
	$(MAKE) TIER=full BUILD=build-full-pgo PGO=gen all
	sh bench/pgo_train.sh build-full-pgo/lunavm$(EXE) full
	rm -rf build-full-pgo/lua build-full-pgo/obj build-full-pgo/liblunaberry.a
	$(MAKE) TIER=full BUILD=build-full-pgo PGO=use all
	$(MAKE) TIER=full BUILD=build-full-pgo DIST=dist dist-copy

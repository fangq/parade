# Parade - portable, deterministic text layout core

CC      ?= cc
CFLAGS  ?= -O2
PD_CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Iinclude -fPIC

# optional complex-script shaping through HarfBuzz: make HARFBUZZ=1 (use its own BUILD dir)
HARFBUZZ ?= 0
ifeq ($(HARFBUZZ),1)
PD_CFLAGS += -DPD_WITH_HARFBUZZ $(shell pkg-config --cflags harfbuzz)
LDLIBS += $(shell pkg-config --libs harfbuzz)
endif
# optional real-time collaboration through yrs: make SYNC=yrs (its C library built in YRS_DIR, see
# README); adds pd_sync and test_sync
SYNC    ?=
YRS_DIR ?= build/yrs/src
ifeq ($(SYNC),yrs)
PD_CFLAGS += -DPD_WITH_SYNC -isystem $(YRS_DIR)/tests-ffi/include
LDLIBS += $(YRS_DIR)/target/release/libyrs.a -lpthread -ldl
endif
LDLIBS  += -lm
AR      ?= ar

# address-space cap for test runs, in KB (default 2 GB); 0 disables
MEMLIMIT_KB ?= 2097152
ORACLE_MAX_MEM ?= 2048
ulimit_cmd = $(if $(filter 0,$(MEMLIMIT_KB)),true,ulimit -v $(MEMLIMIT_KB))

SRC     := src/pd_font.c src/pd_raster.c src/pd_cff.c src/pd_unidata.c src/pd_text.c src/pd_bidi.c src/pd_shape.c src/pd_hyph.c src/pd_para.c src/pd_break.c src/pd_json.c src/pd_zlib.c src/pd_doc.c src/pd_doc_io.c \
           src/pd_doc_layout.c src/pd_layout.c src/pd_pdf.c src/pd_conv.c src/pd_markup.c src/pd_html.c \
           src/pd_markdown.c src/pd_latex.c src/pd_rtf.c src/pd_docx.c src/pd_math.c src/pd_emf.c src/pd_omml.c
ifeq ($(SYNC),yrs)
SRC     += src/pd_sync.c
endif
BUILD   ?= build
OBJ     := $(SRC:src/%.c=$(BUILD)/%.o)
LIB     := $(BUILD)/libparade.a
SO      := $(BUILD)/libparade.so
TESTS   := $(BUILD)/test_parade $(BUILD)/test_doc $(BUILD)/test_layout $(BUILD)/test_pdf $(BUILD)/test_convert
ifeq ($(SYNC),yrs)
TESTS   += $(BUILD)/test_sync
TOOLS_SYNC := $(BUILD)/pd_compact
endif
BENCH   := $(BUILD)/bench_parade

all: $(LIB) $(SO) $(TESTS) $(BENCH) $(BUILD)/pd_dump $(BUILD)/pd_conv $(TOOLS_SYNC)

$(BUILD)/%.o: src/%.c include/parade.h include/parade_doc.h include/parade_layout.h include/parade_convert.h \
           include/parade_sync.h \
           src/pd_internal.h src/pd_doc_internal.h src/pd_json.h src/pd_conv.h | $(BUILD)
	$(CC) $(PD_CFLAGS) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	rm -f $@ && $(AR) rcs $@ $^

$(SO): $(OBJ)
	$(CC) -shared -o $@ $^ $(LDLIBS)

$(BUILD)/test_%: tests/test_%.c $(LIB)
	$(CC) $(PD_CFLAGS) $(CFLAGS) $< $(LIB) -o $@ $(LDLIBS)

# the relay's log compaction (tools/parade_relay.py --compactor)
$(BUILD)/pd_compact: tools/pd_compact.c | $(BUILD)
	$(CC) $(PD_CFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS)

$(BENCH): bench/bench_parade.c $(LIB)
	$(CC) $(PD_CFLAGS) $(CFLAGS) $< $(LIB) -o $@ $(LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

test: $(TESTS)
	$(ulimit_cmd) && for t in $(TESTS); do ./$$t || exit 1; done

bench: $(BENCH)
	$(ulimit_cmd) && ./$(BENCH)

# cross-check the font reader against fontTools on every installed font;
# fontTools runs in a worker capped at ORACLE_MAX_MEM MB, and a font that
# exceeds the cap is reported as skipped instead of exhausting memory
oracle: $(SO)
	fc-list : file | grep -iE '\.(ttf|otf)' | sed 's/: *$$//' | sort -u | \
	    python3 tools/font_oracle.py --max-mem $(ORACLE_MAX_MEM) - > build/oracle.txt; \
	    rc=$$?; grep -v ' 0 mismatches' build/oracle.txt; \
	    grep -c ' 0 mismatches' build/oracle.txt | sed 's/$$/ fonts match fontTools/'; exit $$rc

$(BUILD)/pd_dump: tools/pd_dump.c $(LIB)
	$(CC) $(PD_CFLAGS) $(CFLAGS) $< $(LIB) -o $@ $(LDLIBS)

$(BUILD)/pd_conv: tools/pd_conv.c $(LIB)
	$(CC) $(PD_CFLAGS) $(CFLAGS) $< $(LIB) -o $@ $(LDLIBS)

# converters checked by other software: LibreOffice opens our DOCX/RTF/HTML,
# LuaLaTeX compiles our LaTeX, python-docx/lxml/mistune parse the rest, and
# LibreOffice's own DOCX/RTF output is read back (MEMLIMIT-capped, timed out)
conv-check: $(BUILD)/pd_conv $(BUILD)/pd_dump $(BUILD)/test_convert
	$(ulimit_cmd) && ./$(BUILD)/pd_dump > /dev/null
	$(ulimit_cmd) && PARADE_CONV_OUT=$(BUILD)/conv ./$(BUILD)/test_convert > /dev/null
	python3 tools/conv_check.py $(BUILD)

# page check: a sample document laid out and drawn page by page
pages: $(BUILD)/pd_dump
	$(ulimit_cmd) && ./$(BUILD)/pd_dump > $(BUILD)/pages.json
	python3 tools/render_pages.py --max-mem $(ORACLE_MAX_MEM) $(BUILD)/pages.json $(BUILD)/pages.svg

# visual check: build/parade_view.svg (+ .png when inkscape is present);
# the renderer caps itself at ORACLE_MAX_MEM MB, inkscape at 4 GB
view: $(SO)
	python3 tools/render.py --max-mem $(ORACLE_MAX_MEM) -o build/parade_view.svg
	-command -v inkscape >/dev/null && (ulimit -v 4194304; inkscape build/parade_view.svg \
	    --export-type=png --export-filename=build/parade_view.png --export-dpi=110 >/dev/null 2>&1) && \
	    echo "wrote build/parade_view.png"

# ---- Free Pascal / Lazarus ----
# LAZDIR: a Lazarus tree with lazbuild (e.g. LAZDIR=/path/to/Lazarus42/); empty = lazbuild on PATH
LAZDIR ?=
LAZBUILD = $(if $(LAZDIR),$(LAZDIR)lazbuild --lazarusdir=$(LAZDIR) $(LAZPCP),lazbuild)
LAZPCP ?=
FPC ?= fpc
XVFB_DISPLAY ?= :97
REVIEW_DOC ?=

$(BUILD)/pascal/lib/libparade.a: $(LIB)
	mkdir -p $(BUILD)/pascal/lib $(BUILD)/pascal/units
	cp $(LIB) $@

# ABI (C vs Pascal record layouts) and API tests, no GUI needed
pascal: $(BUILD)/pascal/lib/libparade.a
	python3 tools/gen_abi.py
	$(CC) $(PD_CFLAGS) pascal/tests/abi_c.c -o $(BUILD)/pascal/abi_c
	$(FPC) -O2 -Fupascal -Fl$(BUILD)/pascal/lib -FU$(BUILD)/pascal/units -FE$(BUILD)/pascal pascal/tests/abi_test.pas
	$(FPC) -O2 -Fupascal -Fl$(BUILD)/pascal/lib -FU$(BUILD)/pascal/units -FE$(BUILD)/pascal pascal/tests/api_test.pas
	./$(BUILD)/pascal/abi_c > $(BUILD)/pascal/abi_c.txt
	./$(BUILD)/pascal/abi_test > $(BUILD)/pascal/abi_pas.txt
	diff $(BUILD)/pascal/abi_c.txt $(BUILD)/pascal/abi_pas.txt && echo "ABI match"
	$(ulimit_cmd) && ./$(BUILD)/pascal/api_test

# the editor control driven headless on a private Xvfb; renders PNGs into build/pascal
pascal-edit: $(BUILD)/pascal/lib/libparade.a $(BUILD)/pd_dump
	$(ulimit_cmd) && ./$(BUILD)/pd_dump > $(BUILD)/pages.json
	$(LAZBUILD) pascal/tests/edit_test.lpi
	Xvfb $(XVFB_DISPLAY) -screen 0 1280x1024x24 >/dev/null 2>&1 & echo $$! > $(BUILD)/pascal/xvfb.pid; sleep 2; \
	    ($(ulimit_cmd) && DISPLAY=$(XVFB_DISPLAY) timeout -s KILL 300 ./$(BUILD)/pascal/edit_test $(BUILD)/sample.pdoc $(REVIEW_DOC) \
	    < /dev/null); rc=$$?; kill `cat $(BUILD)/pascal/xvfb.pid`; exit $$rc

# two editors sharing a document through tools/parade_relay.py, headless under Xvfb (SYNC=yrs build in
# build-sync/, the relay needs python3 with aiohttp and PyJWT)
pascal-sync:
	$(MAKE) SYNC=yrs BUILD=build-sync build-sync/pascal/lib/libparade.a build-sync/pd_compact
	cp $(YRS_DIR)/target/release/libyrs.a build-sync/pascal/lib/
	$(LAZBUILD) pascal/tests/sync_test.lpi
	Xvfb $(XVFB_DISPLAY) -screen 0 1400x1000x24 >/dev/null 2>&1 & echo $$! > build-sync/pascal/xvfb.pid; sleep 2; \
	    ($(ulimit_cmd) && DISPLAY=$(XVFB_DISPLAY) timeout -s KILL 300 ./build-sync/pascal/sync_test tools/parade_relay.py build-sync/pd_compact \
	    < /dev/null); rc=$$?; kill `cat build-sync/pascal/xvfb.pid`; exit $$rc

# the relay in Pascal (pascal/paraderelay.pas: SQLdb + fphttpserver, compaction in-process), no Python
# needed: build-sync/pascal/parade_relay; RELAY=pascal runs pascal-sync and the relay tests against it
pascal-relay:
	$(MAKE) SYNC=yrs BUILD=build-sync build-sync/pascal/lib/libparade.a
	cp $(YRS_DIR)/target/release/libyrs.a build-sync/pascal/lib/
	mkdir -p build-sync/pascal/relay-units
	$(FPC) -O2 -Fupascal -Flbuild-sync/pascal/lib -FUbuild-sync/pascal/relay-units -obuild-sync/pascal/parade_relay \
	    pascal/relay/parade_relay.lpr
	$(ulimit_cmd) && RELAY_BIN=build-sync/pascal/parade_relay python3 tools/test_relay.py

pascal-sync-native: pascal-relay
	$(LAZBUILD) pascal/tests/sync_test.lpi
	Xvfb $(XVFB_DISPLAY) -screen 0 1400x1000x24 >/dev/null 2>&1 & echo $$! > build-sync/pascal/xvfb.pid; sleep 2; \
	    ($(ulimit_cmd) && DISPLAY=$(XVFB_DISPLAY) timeout -s KILL 300 ./build-sync/pascal/sync_test build-sync/pascal/parade_relay \
	    < /dev/null); rc=$$?; kill `cat build-sync/pascal/xvfb.pid`; exit $$rc

# the same with the relay inside the test program, as an editor hosting a document runs it
pascal-sync-inproc:
	$(MAKE) SYNC=yrs BUILD=build-sync build-sync/pascal/lib/libparade.a
	cp $(YRS_DIR)/target/release/libyrs.a build-sync/pascal/lib/
	$(LAZBUILD) pascal/tests/sync_test.lpi
	Xvfb $(XVFB_DISPLAY) -screen 0 1400x1000x24 >/dev/null 2>&1 & echo $$! > build-sync/pascal/xvfb.pid; sleep 2; \
	    ($(ulimit_cmd) && DISPLAY=$(XVFB_DISPLAY) timeout -s KILL 300 ./build-sync/pascal/sync_test inproc \
	    < /dev/null); rc=$$?; kill `cat build-sync/pascal/xvfb.pid`; exit $$rc

pascal-demo: $(BUILD)/pascal/lib/libparade.a
	$(LAZBUILD) pascal/demo/paradedemo.lpi

# Unicode conformance: official test files (fetched once into build/ucd)
UCD_URL ?= https://www.unicode.org/Public/15.1.0/ucd
UCD_FILES = auxiliary/LineBreakTest.txt auxiliary/GraphemeBreakTest.txt BidiCharacterTest.txt LineBreak.txt \
            auxiliary/GraphemeBreakProperty.txt emoji/emoji-data.txt BidiMirroring.txt UnicodeData.txt \
            EastAsianWidth.txt DerivedCoreProperties.txt BidiBrackets.txt

$(BUILD)/ucd/.fetched:
	mkdir -p $(BUILD)/ucd
	cd $(BUILD)/ucd && for f in $(UCD_FILES); do curl -sSf -O "$(UCD_URL)/$$f" || exit 1; done
	touch $@

$(BUILD)/conformance: tests/conformance.c $(LIB)
	$(CC) $(PD_CFLAGS) $(CFLAGS) -Isrc $< $(LIB) -o $@ $(LDLIBS)

conformance: $(BUILD)/conformance $(BUILD)/ucd/.fetched
	$(ulimit_cmd) && ./$(BUILD)/conformance $(BUILD)/ucd

# regenerate src/pd_unidata.c from the UCD
unidata: $(BUILD)/ucd/.fetched
	python3 tools/gen_unidata.py $(BUILD)/ucd

# PDF through external readers: Ghostscript parses it, poppler extracts its text and renders it
pdf-check: $(BUILD)/test_pdf $(BUILD)/pd_dump
	$(ulimit_cmd) && ./$(BUILD)/test_pdf && ./$(BUILD)/pd_dump --pdf $(BUILD)/sample.pdf > /dev/null
	gs -q -dNOPAUSE -dBATCH -sDEVICE=nullpage $(BUILD)/test_pdf.pdf && gs -q -dNOPAUSE -dBATCH -sDEVICE=nullpage $(BUILD)/sample.pdf
	pdftotext $(BUILD)/sample.pdf - | grep -q "Figure 1: a here-or-top float" && echo "text extraction ok"
	pdftoppm -r 60 -png $(BUILD)/test_pdf.pdf $(BUILD)/test_pdf && echo "rendered $(BUILD)/test_pdf-*.png"

# rasterizer vs fontTools: outline areas and coverage (TrueType and CFF)
oracle-raster: $(SO)
	python3 tools/raster_oracle.py --max-mem $(ORACLE_MAX_MEM) \
	    /usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf \
	    /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc

# ---- fuzzing ----
FUZZ_TARGETS := font doc import para math
FUZZ_RUNS ?= 2000
FUZZ_SEED ?= 1
FUZZ_TIME ?= 60
FUZZ_SAN := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
FUZZ_SRC := $(SRC) fuzz/fuzz_common.h

# seed corpora: committed text seeds plus the converter and sample outputs;
# import seeds carry a leading format byte (0 text, 1 HTML, 2 md, 3 RTF, 4 DOCX, 5 JData)
$(BUILD)/fuzz/seeds/.done: $(BUILD)/pd_dump $(BUILD)/test_convert
	rm -rf $(BUILD)/fuzz/seeds && mkdir -p $(addprefix $(BUILD)/fuzz/seeds/,$(FUZZ_TARGETS))
	$(ulimit_cmd) && ./$(BUILD)/pd_dump > /dev/null && PARADE_CONV_OUT=$(BUILD)/conv ./$(BUILD)/test_convert > /dev/null
	for t in $(FUZZ_TARGETS); do [ ! -d fuzz/seeds/$$t ] || cp fuzz/seeds/$$t/* $(BUILD)/fuzz/seeds/$$t/; done
	cp $(BUILD)/sample.pdoc $(BUILD)/conv/rich.pdoc $(BUILD)/conv/rich.md $(BUILD)/conv/rich.html $(BUILD)/fuzz/seeds/doc/
	cp tests/data/*.png /usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf $(BUILD)/fuzz/seeds/font/
	set -e; s=$(BUILD)/fuzz/seeds/import; \
	    for x in txt:0 html:1 md:2 rtf:3 docx:4 pdoc:5; do \
	        e=$${x%:*}; printf "\\$$(printf %03o $${x#*:})" > $$s/rich.$$e; cat $(BUILD)/conv/rich.$$e >> $$s/rich.$$e; \
	    done; printf '\377' > $$s/detect.html; cat $(BUILD)/conv/rich.html >> $$s/detect.html
	touch $@

# replay + deterministic mutations under ASan/UBSan with the stand-in driver (any compiler; CI)
$(BUILD)/fuzz/smoke_%: fuzz/fuzz_%.c fuzz/driver.c $(FUZZ_SRC)
	mkdir -p $(BUILD)/fuzz
	$(CC) $(PD_CFLAGS) $(FUZZ_SAN) -Ifuzz $< fuzz/driver.c $(SRC) -o $@ -lm

fuzz-smoke: $(addprefix $(BUILD)/fuzz/smoke_,$(FUZZ_TARGETS)) $(BUILD)/fuzz/seeds/.done
	set -e; for t in $(FUZZ_TARGETS); do \
	    ASAN_OPTIONS=hard_rss_limit_mb=2048 UBSAN_OPTIONS=print_stacktrace=1 \
	    ./$(BUILD)/fuzz/smoke_$$t -runs=$(FUZZ_RUNS) -seed=$(FUZZ_SEED) $(BUILD)/fuzz/seeds/$$t; done

# coverage-guided fuzzing with libFuzzer (clang): make fuzz FUZZ=doc FUZZ_TIME=600
# clang 14's sanitizer runtime crashes at startup on kernels with 32-bit mmap
# randomization (Linux 6.5+, about 1 run in 4): run it without ASLR
FUZZ ?= doc
NOASLR := $(if $(shell command -v setarch),setarch $(shell uname -m) -R)
$(BUILD)/fuzz/lf_%: fuzz/fuzz_%.c $(FUZZ_SRC)
	mkdir -p $(BUILD)/fuzz
	clang $(PD_CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -Ifuzz $< $(SRC) -o $@ -lm

fuzz: $(BUILD)/fuzz/lf_$(FUZZ) $(BUILD)/fuzz/seeds/.done
	mkdir -p $(BUILD)/fuzz/corpus/$(FUZZ) $(BUILD)/fuzz/crash
	$(NOASLR) ./$(BUILD)/fuzz/lf_$(FUZZ) -max_total_time=$(FUZZ_TIME) -rss_limit_mb=2048 -timeout=10 -max_len=65536 \
	    -artifact_prefix=$(BUILD)/fuzz/crash/$(FUZZ)- $(BUILD)/fuzz/corpus/$(FUZZ) $(BUILD)/fuzz/seeds/$(FUZZ)

# address and undefined-behaviour sanitizers
# sanitizer build in its own directory, so the normal build stays loadable
asan:
	ASAN_OPTIONS=hard_rss_limit_mb=2048 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) test BUILD=build-asan MEMLIMIT_KB=0 \
	    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDLIBS="-lm -fsanitize=address,undefined"

clean:
	rm -rf build build-asan

pretty:
	astyle \
	    --style=attach \
	    --indent=spaces=4 \
	    --indent-modifiers \
	    --indent-switches \
	    --indent-preproc-block \
	    --indent-preproc-define \
	    --indent-col1-comments \
	    --pad-oper \
	    --pad-header \
	    --align-pointer=type \
	    --align-reference=type \
	    --add-brackets \
	    --convert-tabs \
	    --close-templates \
	    --lineend=linux \
	    --preserve-date \
	    --suffix=none \
	    --formatted \
	    --break-blocks \
	    "include/*.h" "src/*.c" "src/*.h" "tests/*.c" "bench/*.c" "fuzz/*.c" "fuzz/*.h"

.PHONY: all test bench fuzz fuzz-smoke conv-check conformance unidata oracle oracle-raster pdf-check view pages pascal pascal-edit pascal-sync pascal-relay pascal-sync-native pascal-sync-inproc pascal-demo asan clean pretty

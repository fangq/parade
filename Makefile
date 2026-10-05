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
LDLIBS  += -lm
AR      ?= ar

# address-space cap for test runs, in KB (default 2 GB); 0 disables
MEMLIMIT_KB ?= 2097152
ORACLE_MAX_MEM ?= 2048
ulimit_cmd = $(if $(filter 0,$(MEMLIMIT_KB)),true,ulimit -v $(MEMLIMIT_KB))

SRC     := src/pd_font.c src/pd_raster.c src/pd_cff.c src/pd_unidata.c src/pd_text.c src/pd_bidi.c src/pd_shape.c src/pd_hyph.c src/pd_para.c src/pd_break.c src/pd_json.c src/pd_zlib.c src/pd_doc.c src/pd_doc_io.c \
           src/pd_doc_layout.c src/pd_layout.c src/pd_pdf.c src/pd_conv.c src/pd_markup.c src/pd_html.c \
           src/pd_markdown.c src/pd_latex.c src/pd_rtf.c src/pd_docx.c src/pd_math.c
BUILD   ?= build
OBJ     := $(SRC:src/%.c=$(BUILD)/%.o)
LIB     := $(BUILD)/libparade.a
SO      := $(BUILD)/libparade.so
TESTS   := $(BUILD)/test_parade $(BUILD)/test_doc $(BUILD)/test_layout $(BUILD)/test_pdf $(BUILD)/test_convert
BENCH   := $(BUILD)/bench_parade

all: $(LIB) $(SO) $(TESTS) $(BENCH) $(BUILD)/pd_dump $(BUILD)/pd_conv

$(BUILD)/%.o: src/%.c include/parade.h include/parade_doc.h include/parade_layout.h include/parade_convert.h \
           src/pd_internal.h src/pd_doc_internal.h src/pd_json.h src/pd_conv.h | $(BUILD)
	$(CC) $(PD_CFLAGS) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	rm -f $@ && $(AR) rcs $@ $^

$(SO): $(OBJ)
	$(CC) -shared -o $@ $^ $(LDLIBS)

$(BUILD)/test_%: tests/test_%.c $(LIB)
	$(CC) $(PD_CFLAGS) $(CFLAGS) $< $(LIB) -o $@ $(LDLIBS)

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
	    ($(ulimit_cmd) && DISPLAY=$(XVFB_DISPLAY) timeout -s KILL 300 ./$(BUILD)/pascal/edit_test $(BUILD)/sample.pdoc \
	    < /dev/null); rc=$$?; kill `cat $(BUILD)/pascal/xvfb.pid`; exit $$rc

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
	    "include/*.h" "src/*.c" "src/*.h" "tests/*.c" "bench/*.c"

.PHONY: all test bench conv-check conformance unidata oracle oracle-raster pdf-check view pages pascal pascal-edit pascal-demo asan clean pretty

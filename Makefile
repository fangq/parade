# Parade - portable, deterministic text layout core

CC      ?= cc
CFLAGS  ?= -O2
PD_CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Iinclude -fPIC
LDLIBS  += -lm
AR      ?= ar

# address-space cap for test runs, in KB (default 2 GB); 0 disables
MEMLIMIT_KB ?= 2097152
ORACLE_MAX_MEM ?= 2048
ulimit_cmd = $(if $(filter 0,$(MEMLIMIT_KB)),true,ulimit -v $(MEMLIMIT_KB))

SRC     := src/pd_font.c src/pd_raster.c src/pd_cff.c src/pd_para.c src/pd_break.c src/pd_json.c src/pd_doc.c src/pd_doc_io.c \
           src/pd_doc_layout.c src/pd_layout.c
BUILD   ?= build
OBJ     := $(SRC:src/%.c=$(BUILD)/%.o)
LIB     := $(BUILD)/libparade.a
SO      := $(BUILD)/libparade.so
TESTS   := $(BUILD)/test_parade $(BUILD)/test_doc $(BUILD)/test_layout
BENCH   := $(BUILD)/bench_parade

all: $(LIB) $(SO) $(TESTS) $(BENCH)

$(BUILD)/%.o: src/%.c include/parade.h include/parade_doc.h include/parade_layout.h src/pd_internal.h src/pd_doc_internal.h \
           src/pd_json.h | $(BUILD)
	$(CC) $(PD_CFLAGS) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

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

.PHONY: all test bench oracle oracle-raster view pages asan clean pretty

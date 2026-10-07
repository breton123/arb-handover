CC      ?= gcc
BUILD   ?= build
CFLAGS  ?= -std=c17 -Wall -Wextra -Wpedantic -Werror -Wshadow -O3
CFLAGS  += -Iinclude
LDFLAGS ?=
LDLIBS  ?= -lpthread

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  CFLAGS  += -D_GNU_SOURCE -march=native -mtune=native
  LINUX   := 1
  LDLIBS  += -lcrypto
endif

ifdef SANITIZE
  CFLAGS  := $(filter-out -O3,$(CFLAGS)) -O1 -g -fno-omit-frame-pointer
  CFLAGS  += -fsanitize=address,undefined
  LDFLAGS += -fsanitize=address,undefined
endif

NET_CORE_SRC := src/net/ring.c src/net/stats.c src/net/sink.c src/net/race.c src/net/capture.c src/net/capio.c
NET_CORE_OBJ := $(patsubst src/net/%.c,$(BUILD)/%.o,$(NET_CORE_SRC))
SHRED_OBJ    := $(BUILD)/shred_race.o
STATE_OBJ    := $(BUILD)/ingress_tx.o $(BUILD)/ingress_decode.o \
                $(BUILD)/ingress_resolve.o $(BUILD)/deps_class.o $(BUILD)/pump.o \
                $(BUILD)/compact.o $(BUILD)/cert.o $(BUILD)/overlay.o \
                $(BUILD)/token.o $(BUILD)/apply.o $(BUILD)/publish.o \
                $(BUILD)/oracle.o $(BUILD)/oracle_case.o $(BUILD)/oracle_soak.o \
                $(BUILD)/shred_complete.o $(BUILD)/shred_sha256.o \
                $(BUILD)/shred_rs.o $(BUILD)/shred_merkle.o \
                $(BUILD)/shred_fec.o $(BUILD)/shred_stream.o \
                $(BUILD)/shred_sig.o $(BUILD)/shred_assemble.o \
                $(BUILD)/wire_pipeline.o $(BUILD)/wire_classify.o \
                $(BUILD)/wire_lut_file.o $(BUILD)/shred_txframe.o \
                $(BUILD)/shred_entry.o $(BUILD)/shred_prefix.o \
                $(BUILD)/tx_view.o $(BUILD)/tx_resolve.o \
                $(BUILD)/tx_bind.o $(BUILD)/tx_pumpstate.o \
                $(BUILD)/boot.o $(BUILD)/authvec.o $(BUILD)/live.o \
                $(BUILD)/lut_parse.o $(BUILD)/lut_cache.o

.PHONY: all test bench clean

all: $(BUILD)/test_ring $(BUILD)/test_capture $(BUILD)/test_race \
     $(BUILD)/test_shred_parse $(BUILD)/test_shred_race \
     $(BUILD)/test_pump_vectors $(BUILD)/test_overlay $(BUILD)/test_apply \
     $(BUILD)/test_pump_corpus $(BUILD)/test_token $(BUILD)/test_oracle \
     $(BUILD)/test_pump_oracle $(BUILD)/test_pump_oracle002 \
     $(BUILD)/test_decode $(BUILD)/test_wire $(BUILD)/test_assemble \
     $(BUILD)/test_rs $(BUILD)/test_merkle $(BUILD)/test_fec \
     $(BUILD)/test_stream $(BUILD)/test_classify $(BUILD)/test_lut \
     $(BUILD)/test_lut_cache \
     $(BUILD)/test_txframe $(BUILD)/test_pumpstate $(BUILD)/test_authstate \
     $(BUILD)/test_catchup $(BUILD)/test_livestate \
     $(BUILD)/test_capio \
     $(BUILD)/rx_bench $(BUILD)/shred_bench $(BUILD)/state_bench \
     $(BUILD)/state_pipeline_bench $(BUILD)/wire_to_state_bench \
     $(BUILD)/wire_to_state002 $(BUILD)/wire_to_state003 \
     $(BUILD)/rs_bench $(BUILD)/live_truth $(BUILD)/of_ys_cover \
     $(BUILD)/txframe_cover $(BUILD)/pumpstate_001 $(BUILD)/authstate_001 \
     $(BUILD)/live_state_001 $(BUILD)/shred_continuity
ifdef LINUX
all: $(BUILD)/test_rx $(BUILD)/rx_sink
endif

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/net/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shred_race.o: src/shred/race.c include/shred/race.h include/shred/shred.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ingress_tx.o: src/ingress/tx.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ingress_decode.o: src/ingress/decode.c include/ingress/decode.h \
                          include/ingress/tx.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ingress_resolve.o: src/ingress/resolve.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shred_complete.o: src/shred/complete.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shred_sha256.o: src/shred/sha256.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shred_rs.o: src/shred/rs.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shred_merkle.o: src/shred/merkle.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shred_fec.o: src/shred/fec.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shred_stream.o: src/shred/stream.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shred_sig.o: src/shred/sig.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shred_assemble.o: src/shred/assemble.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shred_txframe.o: src/shred/txframe.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shred_entry.o: src/shred/entry.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shred_prefix.o: src/shred/prefix.c include/shred/prefix.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shred_ingest.o: src/shred/ingest.c include/shred/ingest.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tx_view.o: src/tx/view.c include/tx/view.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tx_resolve.o: src/tx/resolve.c include/tx/resolve.h include/wire/lut_cache.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tx_bind.o: src/tx/bind.c include/tx/bind.h include/ingress/tx.h \
                   include/tx/view.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tx_pumpstate.o: src/tx/pumpstate.c include/tx/pumpstate.h \
                        include/wire/lut_cache.h include/transitions/apply.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/boot.o: src/state/boot.c include/state/boot.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/authvec.o: src/state/authvec.c include/state/authvec.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/live.o: src/state/live.c include/state/live.h include/state/compact.h \
                include/wire/lut_cache.h include/state/authvec.h \
                include/deps/class.h include/transitions/apply.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/wire_classify.o: src/wire/classify.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/wire_lut_file.o: src/wire/lut_file.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/lut_parse.o: src/wire/lut_parse.c include/wire/lut_parse.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/lut_cache.o: src/wire/lut_cache.c include/wire/lut_cache.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/wire_pipeline.o: src/wire/pipeline.c include/deps/class.h \
                         include/transitions/apply.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/deps_class.o: src/deps/class.c include/deps/class.h \
                      include/ingress/tx.h include/state/compact.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pump.o: src/protocols/pump/pump.c | $(BUILD)
	$(CC) $(CFLAGS) -std=gnu17 -Wno-pedantic -c $< -o $@

$(BUILD)/compact.o: src/state/compact.c include/state/compact.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/cert.o: src/state/cert.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/overlay.o: src/transitions/overlay.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/token.o: src/transitions/token.c include/state/compact.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/apply.o: src/transitions/apply.c include/transitions/apply.h \
                 include/deps/class.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/publish.o: src/publish/publish.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/oracle.o: src/oracle/oracle.c include/transitions/apply.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/oracle_case.o: src/oracle/case.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/oracle_soak.o: src/oracle/soak.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_ring: tests/net/test_ring.c $(NET_CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_capture: tests/net/test_capture.c $(NET_CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_race: tests/net/test_race.c $(NET_CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_shred_parse: tests/shred/test_parse.c | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

$(BUILD)/test_shred_race: tests/shred/test_race.c $(SHRED_OBJ) \
                          $(BUILD)/shred_ingest.o $(BUILD)/shred_prefix.o | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/rx_bench: bench/rx/main.c $(NET_CORE_OBJ) | $(BUILD)
ifdef LINUX
	$(CC) $(CFLAGS) -std=gnu17 $(LDFLAGS) $^ src/net/udp.c src/net/feed.c -o $@ $(LDLIBS)
else
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)
endif

$(BUILD)/shred_bench: bench/shred/main.c $(SHRED_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_pump_vectors: tests/state/test_pump_vectors.c $(BUILD)/pump.o | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(BUILD)/test_overlay: tests/state/test_overlay.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_apply: tests/state/test_apply.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/state_bench: bench/state/main.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/state_pipeline_bench: bench/state_pipeline/main.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_pump_corpus: tests/state/test_pump_corpus.c $(BUILD)/pump.o | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(BUILD)/test_token: tests/state/test_token.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_oracle: tests/state/test_oracle.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_pump_oracle: tests/state/test_pump_oracle.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_pump_oracle002: tests/state/test_pump_oracle002.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_decode: tests/ingress/test_decode.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_wire: tests/wire/test_wire.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_classify: tests/wire/test_classify.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_lut: tests/wire/test_lut.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_lut_cache: tests/wire/test_lut_cache.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/live_truth: bench/live_truth/main.c $(STATE_OBJ) $(NET_CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/of_ys_cover: bench/of_ys_cover/main.c $(STATE_OBJ) $(NET_CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/txframe_cover: bench/txframe_cover/main.c $(BUILD)/shred_txframe.o \
                        $(BUILD)/shred_entry.o $(BUILD)/shred_prefix.o \
                        $(BUILD)/shred_race.o $(BUILD)/shred_ingest.o \
                        $(NET_CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/shred_continuity: bench/shred_continuity/main.c $(STATE_OBJ) \
                           $(NET_CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_assemble: tests/shred/test_assemble.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_rs: tests/shred/test_rs.c $(BUILD)/shred_rs.o | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(BUILD)/test_merkle: tests/shred/test_merkle.c $(BUILD)/shred_merkle.o \
                      $(BUILD)/shred_sha256.o | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(BUILD)/test_fec: tests/shred/test_fec.c $(BUILD)/shred_fec.o \
                   $(BUILD)/shred_rs.o $(BUILD)/shred_merkle.o \
                   $(BUILD)/shred_sha256.o | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_stream: tests/shred/test_stream.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_txframe: tests/shred/test_txframe.c $(BUILD)/shred_txframe.o \
                       $(BUILD)/shred_entry.o | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(BUILD)/test_pumpstate: tests/tx/test_pumpstate.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_catchup: tests/state/test_catchup.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_authstate: tests/state/test_authstate.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_livestate: tests/state/test_livestate.c $(STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/pumpstate_001: bench/pumpstate/main.c $(STATE_OBJ) $(NET_CORE_OBJ) \
                        $(BUILD)/shred_race.o $(BUILD)/shred_ingest.o | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/authstate_001: bench/authstate/main.c $(STATE_OBJ) $(NET_CORE_OBJ) \
                        $(BUILD)/shred_race.o $(BUILD)/shred_ingest.o | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

ifdef LINUX
$(BUILD)/live_state_001: apps/live_state/main.c $(STATE_OBJ) $(NET_CORE_OBJ) \
                        $(BUILD)/shred_race.o $(BUILD)/shred_ingest.o \
                        $(BUILD)/udp.o $(BUILD)/feed.o | $(BUILD)
	$(CC) $(CFLAGS) -std=gnu17 $(LDFLAGS) $^ -o $@ $(LDLIBS)
else
$(BUILD)/live_state_001: apps/live_state/main.c $(STATE_OBJ) $(NET_CORE_OBJ) \
                        $(BUILD)/shred_race.o $(BUILD)/shred_ingest.o | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)
endif

$(BUILD)/rs_bench: bench/rs/main.c $(BUILD)/shred_rs.o | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(BUILD)/test_capio: tests/net/test_capio.c $(NET_CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/wire_to_state_bench: bench/wire_to_state/main.c $(STATE_OBJ) $(NET_CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/wire_to_state002: bench/wire_to_state002/main.c $(STATE_OBJ) $(NET_CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/wire_to_state003: bench/wire_to_state003/main.c $(STATE_OBJ) $(NET_CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

ifdef LINUX
$(BUILD)/udp.o: src/net/udp.c | $(BUILD)
	$(CC) $(CFLAGS) -std=gnu17 -c $< -o $@

$(BUILD)/feed.o: src/net/feed.c | $(BUILD)
	$(CC) $(CFLAGS) -std=gnu17 -c $< -o $@

$(BUILD)/test_rx: tests/net/test_rx.c $(NET_CORE_OBJ) $(SHRED_OBJ) $(BUILD)/udp.o $(BUILD)/feed.o | $(BUILD)
	$(CC) $(CFLAGS) -std=gnu17 $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/rx_sink: apps/rx_sink/main.c $(NET_CORE_OBJ) $(SHRED_OBJ) $(BUILD)/udp.o $(BUILD)/feed.o | $(BUILD)
	$(CC) $(CFLAGS) -std=gnu17 $(LDFLAGS) $^ -o $@ $(LDLIBS)
endif

test: all
	$(BUILD)/test_ring
	$(BUILD)/test_capture
	$(BUILD)/test_race
	$(BUILD)/test_shred_parse
	$(BUILD)/test_shred_race
	$(BUILD)/test_pump_vectors
	$(BUILD)/test_overlay
	$(BUILD)/test_apply
	$(BUILD)/test_pump_corpus
	$(BUILD)/test_token
	$(BUILD)/test_oracle
	$(BUILD)/test_pump_oracle
	$(BUILD)/test_pump_oracle002
	$(BUILD)/test_decode
	$(BUILD)/test_wire
	$(BUILD)/test_assemble
	$(BUILD)/test_rs
	$(BUILD)/test_merkle
	$(BUILD)/test_fec
	$(BUILD)/test_stream
	$(BUILD)/test_txframe
	$(BUILD)/test_pumpstate
	$(BUILD)/test_authstate
	$(BUILD)/test_catchup
	$(BUILD)/test_livestate
	$(BUILD)/test_classify
	$(BUILD)/test_lut
	$(BUILD)/test_lut_cache
	$(BUILD)/test_capio
	$(BUILD)/of_ys_cover --self-test
	$(BUILD)/pumpstate_001 --self-test
	$(BUILD)/authstate_001 --self-test
	$(BUILD)/live_state_001 --self-test
	python tools/anchor/test_anchor.py
	python tools/lut/test_account.py
ifdef LINUX
	$(BUILD)/test_rx
endif

bench: $(BUILD)/rx_bench $(BUILD)/shred_bench $(BUILD)/state_bench \
       $(BUILD)/state_pipeline_bench $(BUILD)/wire_to_state_bench \
       $(BUILD)/wire_to_state002 $(BUILD)/wire_to_state003 $(BUILD)/rs_bench \
       $(BUILD)/live_truth $(BUILD)/pumpstate_001 $(BUILD)/authstate_001
	$(BUILD)/rx_bench --mode ring --count 200000
	$(BUILD)/shred_bench
	$(BUILD)/state_bench
	$(BUILD)/state_pipeline_bench
	$(BUILD)/wire_to_state_bench
	$(BUILD)/wire_to_state002 --self-test
	$(BUILD)/rs_bench
	$(BUILD)/live_truth --self-test
	$(BUILD)/of_ys_cover --self-test
	$(BUILD)/txframe_cover --self-test
	$(BUILD)/pumpstate_001 --self-test
	$(BUILD)/authstate_001 --self-test
ifdef LINUX
	$(BUILD)/rx_bench --mode udp --count 20000 --port 39921
endif

clean:
	rm -rf $(BUILD)

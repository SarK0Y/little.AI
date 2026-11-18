CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra
BUILD   := build
SRC     := src/sat.c src/groups.c

.PHONY: build test bench bench-rarest clean

build: $(BUILD)/3sat

$(BUILD)/3sat: src/main.c $(SRC) src/sat.h src/groups.h src/result.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/main.c $(SRC)

$(BUILD)/test_sat: tests/test_sat.c $(SRC) src/sat.h src/groups.h src/result.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -Isrc -o $@ tests/test_sat.c $(SRC)

test: $(BUILD)/test_sat
	./$(BUILD)/test_sat

# make bench N=5000 RATIO=4.0 SEED=1
N ?= 5000
RATIO ?= 4.0
SEED ?= 1
bench: build
	python3 tools/gen_cnf.py $(N) $(RATIO) $(SEED) > $(BUILD)/bench.cnf
	time ./$(BUILD)/3sat $(BUILD)/bench.cnf --out $(BUILD)/bench.json

$(BUILD)/bench_rarest: tools/bench_rarest.c $(SRC) src/sat.h src/groups.h src/result.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ tools/bench_rarest.c $(SRC)

# make bench-rarest ARGS="1000 3.5 10 200000 9"
# make bench-rarest ARGS="tests/fixtures/small_sat.cnf 100000 20"
ARGS ?= 1000 3.5 10 200000 9
bench-rarest: $(BUILD)/bench_rarest
	./$(BUILD)/bench_rarest $(ARGS)

clean:
	rm -rf $(BUILD)

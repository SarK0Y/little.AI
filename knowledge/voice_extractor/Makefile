CXX      ?= c++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic
LDFLAGS  ?=

TARGETS := voice_extractor voice_reconstruct \
           voice_template_benchmark voice_template_recovery \
           voice_template_reconstruct

SOURCE   := voice_extractor.cpp
OBJECTS  := $(SOURCE:.cpp=.o)

BENCHMARK_OBJECT := voice_template_benchmark.o
RECOVERY_OBJECT  := voice_template_recovery.o
RECONSTRUCT_OBJECT := voice_template_reconstruct.o

INPUT  ?= template.wav
OUTDIR ?= benchmark

.PHONY: all clean benchmark run-benchmark recovery run-recovery \
        run-all-recoveries reconstruct run-reconstruct run-all-reconstructs

all: $(TARGETS)

voice_extractor: $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ -lm

voice_reconstruct: template_reconstruct.o
	$(CXX) $(LDFLAGS) -o $@ $^ -lm

voice_template_benchmark: $(BENCHMARK_OBJECT)
	$(CXX) $(LDFLAGS) -o $@ $^ -lsndfile -lm

benchmark: voice_template_benchmark

run-benchmark: voice_template_benchmark
	mkdir -p $(OUTDIR)
	./voice_template_benchmark $(INPUT) $(OUTDIR)

voice_template_recovery: $(RECOVERY_OBJECT)
	$(CXX) $(LDFLAGS) -o $@ $^ -lsndfile -lfftw3 -lm

recovery: voice_template_recovery

run-recovery: voice_template_recovery
	./voice_template_recovery $(INPUT) $(OUTDIR)/contaminated_sir_-12.wav $(OUTDIR)/recovery_-12

run-all-recoveries: voice_template_recovery
	@for sir in 12 6 0 -6 -12; do \
		echo "Recovering SIR $$sir dB"; \
		./voice_template_recovery \
			$(INPUT) \
			$(OUTDIR)/contaminated_sir_$$sir.wav \
			$(OUTDIR)/recovery_$$sir || exit $$?; \
	done

voice_template_reconstruct: $(RECONSTRUCT_OBJECT)
	$(CXX) $(LDFLAGS) -o $@ $^ -lsndfile -lfftw3 -lm

reconstruct: voice_template_reconstruct

run-reconstruct: voice_template_reconstruct
	./voice_template_reconstruct $(INPUT) $(OUTDIR)/contaminated_sir_-12.wav $(OUTDIR)/template_reconstruct_-12

run-all-reconstructs: voice_template_reconstruct
	@for sir in 12 6 0 -6 -12; do \
		echo "Reconstructing SIR $$sir dB"; \
		./voice_template_reconstruct \
			$(INPUT) \
			$(OUTDIR)/contaminated_sir_$$sir.wav \
			$(OUTDIR)/template_reconstruct_$$sir || exit $$?; \
	done

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	$(RM) $(TARGETS) $(OBJECTS) \
		template_reconstruct.o \
		$(BENCHMARK_OBJECT) $(RECOVERY_OBJECT) $(RECONSTRUCT_OBJECT)


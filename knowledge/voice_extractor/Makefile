CXX      ?= c++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic
LDFLAGS  ?=

TARGETS := voice_extractor voice_reconstruct \
           voice_template_benchmark voice_template_recovery \
           voice_template_reconstruct voice_music_self_template

SOURCE   := voice_extractor.cpp
OBJECTS  := $(SOURCE:.cpp=.o)

BENCHMARK_OBJECT := voice_template_benchmark.o
RECOVERY_OBJECT  := voice_template_recovery.o
RECONSTRUCT_OBJECT := voice_template_reconstruct.o
MUSIC_SELF_TEMPLATE_OBJECT := voice_music_self_template.o

INPUT  ?= template.wav
OUTDIR ?= benchmark

.PHONY: all clean benchmark run-benchmark recovery run-recovery \
        run-all-recoveries reconstruct run-reconstruct run-all-reconstructs \
        music-self-template run-music-self-template

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


# DB-free music structure / stereo analysis prototype.
# Produces Mid/Side, stereo-unique, frequency-band WAVs, plus a repeat report.
# Dependencies: libsndfile + FFTW3
voice_music_self_template: $(MUSIC_SELF_TEMPLATE_OBJECT)
	$(CXX) $(LDFLAGS) -o $@ $^ -lsndfile -lfftw3 -lm

music-self-template: voice_music_self_template

# Example:
#   make run-music-self-template INPUT=stereo_song.wav
#   make run-music-self-template INPUT=stereo_song.wav PREFIX=analysis REPORT=repeats.json
PREFIX ?= stereo_analysis
REPORT ?= $(PREFIX)_repeats.json

run-music-self-template: voice_music_self_template
	./voice_music_self_template $(INPUT) $(PREFIX) $(REPORT)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	$(RM) $(TARGETS) $(OBJECTS) \
		template_reconstruct.o \
		$(BENCHMARK_OBJECT) $(RECOVERY_OBJECT) $(RECONSTRUCT_OBJECT) \
		voice_music_self_template_entropy.o voice_music_self_template_entropy



# Entropy/fading/correlation acceptance experiment (separate from the older
# self-template scanner so existing targets remain unchanged).
voice_music_self_template_entropy: voice_music_self_template_entropy.o
	$(CXX) $(LDFLAGS) -o $@ $^ -lsndfile -lfftw3 -lm

music-mask-experiment: voice_music_self_template_entropy

# Example options:
#   make run-music-mask-experiment INPUT=song.wav PREFIX=out/song \
#       MUSIC_ARGS="-band-bar 6000 -entropy 0.42 -silence-band 180-260"
MUSIC_ARGS ?=
run-music-mask-experiment: voice_music_self_template_entropy
	./voice_music_self_template_entropy $(INPUT) $(PREFIX) $(REPORT) $(MUSIC_ARGS)

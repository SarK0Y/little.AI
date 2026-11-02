CXX      ?= c++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic
LDFLAGS  ?=

TARGETS := voice_extractor voice_reconstruct \
           voice_template_benchmark voice_template_recovery \
           voice_template_reconstruct voice_template_profile_transfer

SOURCE   := voice_extractor.cpp
OBJECTS  := $(SOURCE:.cpp=.o)

BENCHMARK_OBJECT := voice_template_benchmark.o
RECOVERY_OBJECT  := voice_template_recovery.o
RECONSTRUCT_OBJECT := voice_template_reconstruct.o
PROFILE_TRANSFER_OBJECT := voice_template_profile_transfer.o

INPUT       ?= template.wav
OUTDIR      ?= benchmark
TARGET_WAV  ?= target.wav
PROFILE_JSON ?= voice_profile.json
OUTPUT      ?= profile_transfer.wav

.PHONY: all clean benchmark run-benchmark recovery run-recovery \
        run-all-recoveries reconstruct run-reconstruct run-all-reconstructs \
        profile-transfer run-profile run-transfer

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

# Template-guided vocal-fragment profile transfer.
# Requires libsndfile and FFTW3:
#   sudo apt install libsndfile1-dev libfftw3-dev
voice_template_profile_transfer: $(PROFILE_TRANSFER_OBJECT)
	$(CXX) $(LDFLAGS) -o $@ $^ -lsndfile -lfftw3 -lm

profile-transfer: voice_template_profile_transfer

# Detect matching vocal fragments and write the profile JSON.
# Example:
#   make run-profile INPUT=template.wav TARGET_WAV=target.wav PROFILE_JSON=voice_profile.json
run-profile: voice_template_profile_transfer
	./voice_template_profile_transfer profile \
		$(INPUT) $(TARGET_WAV) $(PROFILE_JSON)

# Apply the saved profile to matched fragments; unmatched sections become silence.
# Example:
#   make run-transfer INPUT=template.wav TARGET_WAV=target.wav \
#       PROFILE_JSON=voice_profile.json OUTPUT=matched_vocals.wav
run-transfer: voice_template_profile_transfer
	./voice_template_profile_transfer transfer \
		$(INPUT) $(TARGET_WAV) $(PROFILE_JSON) $(OUTPUT)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	$(RM) $(TARGETS) $(OBJECTS) \
		template_reconstruct.o \
		$(BENCHMARK_OBJECT) $(RECOVERY_OBJECT) $(RECONSTRUCT_OBJECT) \
		$(PROFILE_TRANSFER_OBJECT)

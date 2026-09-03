#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${C_TEST_OUTPUT:-}" ]; then
	out=$C_TEST_OUTPUT
	mkdir -p "$out"
else
	out=$(mktemp -d "$root/.usbradioplus-tests.XXXXXX")
	trap 'rm -rf -- "$out"' EXIT HUP INT TERM
fi

common="-std=gnu11 -Wall -Wextra -Werror ${C_TEST_CFLAGS:-}"
completed=0

# shellcheck disable=SC2086
cc $common "$root/tests/test_stage_order.c" \
	"$root/src/txagc/agc_core.c" -I"$root/src" -o "$out/stage-order"
"$out/stage-order"
completed=$((completed + 1))
# Compiler and pkg-config flag lists intentionally undergo POSIX word splitting.
# shellcheck disable=SC2046,SC2086
cc $common -DURP_TEST_ALLOCATORS "$root/tests/test_usbradioplus_dsp.c" \
	"$root/src/usbradioplus_dsp.c" -o "$out/dsp" \
	$(pkg-config --cflags --libs samplerate) -lm
"$out/dsp"
completed=$((completed + 1))

# shellcheck disable=SC2086
cc $common -Wno-unused-variable "$root/tools/legacy_ctcss_reference.c" \
	"$root/src/usbradioplus_ctcss.c" -O2 -o "$out/ctcss-reference" -lm
"$out/ctcss-reference"
completed=$((completed + 1))

# shellcheck disable=SC2086
cc $common "$root/tests/test_ctcss_generator.c" \
	"$root/src/usbradioplus_ctcss.c" -o "$out/ctcss-generator" -lm
"$out/ctcss-generator"
completed=$((completed + 1))

# shellcheck disable=SC2086
cc $common "$root/tests/test_hardware_words.c" \
	"$root/src/usbradioplus_hardware.c" -o "$out/hardware-words"
"$out/hardware-words"
completed=$((completed + 1))

# Transport-neutral channel queue policy is compiled independently so both
# audio backends share one fully covered implementation.
# shellcheck disable=SC2086
cc $common "$root/tests/test_channel_shared_core.c" \
	"$root/src/usbradioplus_channel_core.c" -I"$root/src" -o "$out/channel-shared-core" -lm
"$out/channel-shared-core"
completed=$((completed + 1))

# shellcheck disable=SC2086
cc $common "$root/tests/test_native_repeat.c" \
	"$root/src/usbradioplus_repeat.c" -o "$out/native-repeat"
"$out/native-repeat"
completed=$((completed + 1))

# shellcheck disable=SC2086
cc $common "$root/tests/test_micor_squelch.c" -o "$out/micor-squelch"
"$out/micor-squelch"
completed=$((completed + 1))

# shellcheck disable=SC2086
cc $common "$root/tests/test_radio_core.c" "$root/src/usbradioplus_radio.c" \
	-DAST_MODULE_SELF_SYM=test_module_self -DAST_MODULE='"chan_usbradioplus"' \
	-DURP_RADIO_TRACE=0 -I"$root/src" -o "$out/radio-core" -lm
"$out/radio-core"
completed=$((completed + 1))

# Include the tuning utility so its private parsing and terminal helpers can be
# tested without exporting implementation details in the installed program.
# shellcheck disable=SC2086
cc $common -Wno-unused-function -ffunction-sections -fdata-sections \
	-DAST_MODULE_SELF_SYM=test_module_self "$root/tests/test_tune_core.c" \
	-I/usr/include -I"$root/src" -Wl,--gc-sections -o "$out/tune-core"
"$out/tune-core"
completed=$((completed + 1))

# Compile the legacy channel port as one translation unit and retain only the
# pure helpers reached by this focused harness. The modern port is exercised by
# the modern-header jobs in the container matrix.
# shellcheck disable=SC2046,SC2086
cc $common -Wno-unused-function -ffunction-sections -fdata-sections \
	"$root/tests/test_channel_core.c" -I/usr/include -I"$root/src" \
	-Wl,--gc-sections -o "$out/channel-core" \
	$(pkg-config --cflags --libs rnnoise samplerate libavfilter libavutil alsa) -lusb -lm
"$out/channel-core"
completed=$((completed + 1))

if [ -n "${ASL_MODERN_INCLUDEDIR:-}" ]; then
	# shellcheck disable=SC2046,SC2086
	cc $common -Wno-unused-function -ffunction-sections -fdata-sections \
		-DURP_TEST_MODERN "$root/tests/test_channel_core.c" \
		-I"$ASL_MODERN_INCLUDEDIR" -I/usr/include -I"$root/src" \
		-Wl,--gc-sections -o "$out/channel-core-modern" \
		$(pkg-config --cflags --libs rnnoise samplerate libavfilter libavutil alsa \
			portaudio-2.0 libusb-1.0) -lm
	"$out/channel-core-modern"
	completed=$((completed + 1))
fi

# shellcheck disable=SC2046,SC2086
cc $common "$root/tests/test_rnnoise_processor.c" \
	"$root/src/txagc/rnnoise_processor.c" -o "$out/rnnoise-processor" \
	$(pkg-config --cflags --libs rnnoise samplerate) -lm
"$out/rnnoise-processor"
completed=$((completed + 1))

# shellcheck disable=SC2046,SC2086
cc $common "$root/tests/test_rnnoise_failures.c" -o "$out/rnnoise-failures" \
	$(pkg-config --cflags --libs rnnoise samplerate) -lm
"$out/rnnoise-failures"
completed=$((completed + 1))

# Include the processing implementation so private validation can be exercised.
# Other static module functions are retained in the production build and are
# intentionally discarded from this focused executable by section GC.
# shellcheck disable=SC2086
cc $common -Wno-unused-function -ffunction-sections -fdata-sections \
	-DAST_MODULE_SELF_SYM=test_module_self "$root/tests/test_processing_validation.c" \
	"$root/src/txagc/agc_core.c" -I/usr/include -I"$root/src" -Wl,--gc-sections \
	-o "$out/processing-validation" -lm
"$out/processing-validation"
completed=$((completed + 1))

for name in avfilter_bandpass avfilter_ctcss avfilter_emphasis \
	avfilter_equalizer \
	avfilter_deesser \
	avfilter_processor avfilter_permutations; do
	# shellcheck disable=SC2046,SC2086
	cc $common "$root/tests/test_$name.c" \
		"$root/src/txagc/agc_core.c" \
		"$root/src/txagc/avfilter_processor.c" \
		-o "$out/$name" $(pkg-config --cflags --libs libavfilter libavutil) -lm
	"$out/$name"
	completed=$((completed + 1))
done

# This test includes the implementation so private graph-construction helpers
# can be checked without widening the production API.
# shellcheck disable=SC2046,SC2086
cc $common "$root/tests/test_avfilter_internals.c" "$root/src/txagc/agc_core.c" \
	-o "$out/avfilter-internals" $(pkg-config --cflags --libs libavfilter libavutil) -lm
"$out/avfilter-internals"
completed=$((completed + 1))

# Force every FFmpeg graph/frame allocation failure through the public API.
# shellcheck disable=SC2046,SC2086
cc $common "$root/tests/test_avfilter_failures.c" "$root/src/txagc/agc_core.c" \
	"$root/src/txagc/avfilter_processor.c" -o "$out/avfilter-failures" \
	-Wl,--wrap=avfilter_graph_alloc -Wl,--wrap=avfilter_graph_create_filter \
	-Wl,--wrap=avfilter_inout_alloc -Wl,--wrap=avfilter_graph_parse_ptr \
	-Wl,--wrap=avfilter_graph_config -Wl,--wrap=av_audio_fifo_alloc \
	-Wl,--wrap=av_frame_alloc -Wl,--wrap=av_frame_get_buffer \
	-Wl,--wrap=av_buffersrc_add_frame_flags -Wl,--wrap=av_buffersink_get_frame \
	-Wl,--wrap=av_audio_fifo_realloc -Wl,--wrap=av_audio_fifo_write \
	$(pkg-config --cflags --libs libavfilter libavutil) -lm
"$out/avfilter-failures"
completed=$((completed + 1))

expected=23
if [ -n "${ASL_MODERN_INCLUDEDIR:-}" ]; then
	expected=24
fi
test "$completed" -eq "$expected"
echo "All $completed C test executables passed"

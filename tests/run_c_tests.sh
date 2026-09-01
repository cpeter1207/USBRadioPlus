#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$(mktemp -d "$root/.usbradioplus-tests.XXXXXX")
trap 'rm -rf -- "$out"' EXIT HUP INT TERM

common="-std=gnu11 -Wall -Wextra -Werror"
cc $common "$root/tests/test_usbradioplus_dsp.c" \
	"$root/src/usbradioplus_dsp.c" -o "$out/dsp" \
	$(pkg-config --cflags --libs samplerate) -lm
"$out/dsp"

cc $common -Wno-unused-variable "$root/tools/legacy_ctcss_reference.c" \
	"$root/src/usbradioplus_ctcss.c" -o "$out/ctcss-reference" -lm
"$out/ctcss-reference"

cc $common "$root/tests/test_ctcss_generator.c" \
	"$root/src/usbradioplus_ctcss.c" -o "$out/ctcss-generator" -lm
"$out/ctcss-generator"

cc $common "$root/tests/test_hardware_words.c" \
	"$root/src/usbradioplus_hardware.c" -o "$out/hardware-words"
"$out/hardware-words"

cc $common "$root/tests/test_native_repeat.c" \
	"$root/src/usbradioplus_repeat.c" -o "$out/native-repeat"
"$out/native-repeat"

for name in txagc_float avfilter_bandpass avfilter_ctcss avfilter_emphasis \
	avfilter_equalizer \
	avfilter_deesser \
	avfilter_processor avfilter_permutations; do
	cc $common "$root/tests/test_$name.c" \
		"$root/src/txagc/agc_core.c" \
		"$root/src/txagc/avfilter_processor.c" \
		-o "$out/$name" $(pkg-config --cflags --libs libavfilter libavutil) -lm
	"$out/$name"
done

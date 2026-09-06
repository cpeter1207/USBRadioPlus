#!/bin/sh
## @file
## @brief Compile linked-object C harnesses and execute their test groups.
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
sys_io_tests=0
channel_invariant_sources="$root/src/usbradioplus_config.c $root/src/usbradioplus_radio.c \
$root/src/usbradioplus_dsp.c $root/src/usbradioplus_ctcss.c \
$root/src/usbradioplus_hardware.c $root/src/usbradioplus_repeat.c \
$root/src/usbradioplus_channel_core.c $root/src/usbradioplus_processing.c \
$root/src/txagc/agc_core.c $root/src/txagc/avfilter_processor.c \
$root/src/txagc/rnnoise_processor.c"
channel_variant_sources="$root/src/usbradioplus_channel_common.c \
$root/src/usbradioplus_native_tick.c $root/src/usbradioplus_tune_menu.c"
channel_wrap_flags="-Wl,--wrap=av_frame_alloc -Wl,--wrap=src_new -Wl,--wrap=src_process \
-Wl,--wrap=pthread_join -Wl,--wrap=read -Wl,--wrap=write -Wl,--wrap=usleep \
-Wl,--wrap=ioctl -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=poll -Wl,--wrap=pipe \
-Wl,--wrap=pipe2 -Wl,--wrap=ioperm -Wl,--wrap=usb_open -Wl,--wrap=usb_close \
-Wl,--wrap=usb_claim_interface -Wl,--wrap=usb_detach_kernel_driver_np"

# The groups have disjoint output names and coverage-counter files, so compile
# and execute them concurrently.  C_TEST_PARALLEL=1 retains a serial diagnostic
# mode for tools that need ordered output.
if [ -z "${C_TEST_GROUP:-}" ] && [ "${C_TEST_PARALLEL:-4}" != 1 ]; then
	pids=
	for group in basic channels rnnoise validation avfilter_bandpass \
		avfilter_ctcss avfilter_emphasis avfilter_equalizer avfilter_deesser \
		avfilter_processor avfilter_permutations avfilter_internals \
		avfilter_failures; do
		C_TEST_GROUP=$group C_TEST_OUTPUT="$out" sh "$0" &
		pids="$pids $!"
	done
	status=0
	for pid in $pids; do
		wait "$pid" || status=1
	done
	test "$status" -eq 0
	echo "All parallel C test groups passed"
	exit 0
fi

## @brief Execute one selected group of linked C test binaries.
run_group()
{
	[ -z "${C_TEST_GROUP:-}" ] || [ "$C_TEST_GROUP" = "$1" ]
}

if run_group basic; then
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
	"$root/src/usbradioplus_ctcss.c" -o "$out/ctcss-reference" -lm
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

C_TEST_OUTPUT="$out" C_TEST_CFLAGS="${C_TEST_CFLAGS:-}" sh "$root/tests/run_radio_core_tests.sh"
completed=$((completed + 2))
fi

if run_group channels; then
	## @brief Compile shared channel sources as separate test-instrumented objects.
	compile_channel_shared()
	{
		variant=$1
		variant_flags=$2
		variant_sources=$3
		compile_pids=
		channel_pkg_cflags=$(pkg-config --cflags rnnoise samplerate libavfilter libavutil alsa)
		for source in $variant_sources; do
			base=$(basename "$source" .c)
			object="$out/channel-$variant-$base.o"
			# Compiler flag lists intentionally undergo POSIX word splitting.
			# shellcheck disable=SC2086
			cc $common $variant_flags $channel_pkg_cflags \
				-DAST_MODULE='"chan_usbradioplus"' \
				-DAST_MODULE_SELF_SYM=test_module_self \
				-I/usr/include -I"$root/src" \
				-c "$source" -o "$object" &
			compile_pids="$compile_pids $!"
		done
		compile_status=0
		for compile_pid in $compile_pids; do
			wait "$compile_pid" || compile_status=1
		done
		test "$compile_status" -eq 0
	}

	## @brief Print the object paths required by a channel adapter harness.
	channel_shared_object_list()
	{
		object_variant=$1
		object_sources=$2
		object_list=
		for object_source in $object_sources; do
			object_base=$(basename "$object_source" .c)
			object_list="$object_list $out/channel-$object_variant-$object_base.o"
		done
		printf '%s\n' "$object_list"
	}

	have_sys_io=0
	if printf '#include <sys/io.h>\n' | cc -E -x c - >/dev/null 2>&1; then
		have_sys_io=1
	fi

	compile_pids=
	compile_channel_shared legacy "-DURP_PROCESSING_TESTING" \
		"$channel_invariant_sources $channel_variant_sources" &
	compile_pids="$compile_pids $!"
	if [ "$have_sys_io" -eq 1 ]; then
		compile_channel_shared sysio "-DHAVE_SYS_IO -DURP_PROCESSING_TESTING" \
			"$channel_invariant_sources $channel_variant_sources" &
		compile_pids="$compile_pids $!"
	fi
	if [ -n "${ASL_MODERN_INCLUDEDIR:-}" ]; then
		compile_channel_shared modern \
			"-DURP_TEST_MODERN -DURP_CHANNEL_MODERN -DURP_PROCESSING_TESTING -I$ASL_MODERN_INCLUDEDIR" \
			"$channel_invariant_sources $channel_variant_sources" &
		compile_pids="$compile_pids $!"
	fi

# Compile the channel driver independently so the test exercises the same
# linked-object boundary as the installed module.
# shellcheck disable=SC2086
cc $common -Wno-unused-function -ffunction-sections -fdata-sections \
	-DURP_CHANNEL_UNIT_TEST -DURP_PROCESSING_TESTING \
	-DAST_MODULE='"chan_usbradioplus"' -DAST_MODULE_SELF_SYM=test_module_self \
	-I/usr/include -I"$root/src" -c "$root/src/chan_usbradioplus.c" \
	-o "$out/chan-usbradioplus-test.o" &
	compile_pids="$compile_pids $!"
	if [ "$have_sys_io" -eq 1 ]; then
		# shellcheck disable=SC2086
		cc $common -Wno-unused-function -ffunction-sections -fdata-sections \
			-DHAVE_SYS_IO -DURP_CHANNEL_UNIT_TEST -DURP_PROCESSING_TESTING \
			-DAST_MODULE='"chan_usbradioplus"' -DAST_MODULE_SELF_SYM=test_module_self \
			-I/usr/include -I"$root/src" -c "$root/src/chan_usbradioplus.c" \
			-o "$out/chan-usbradioplus-sysio-test.o" &
		compile_pids="$compile_pids $!"
	fi
	if [ -n "${ASL_MODERN_INCLUDEDIR:-}" ]; then
		# shellcheck disable=SC2086
		cc $common -Wno-unused-function -ffunction-sections -fdata-sections \
			-DURP_TEST_MODERN -DURP_CHANNEL_MODERN -DURP_CHANNEL_UNIT_TEST \
			-DURP_PROCESSING_TESTING -DAST_MODULE='"chan_usbradioplus"' \
			-DAST_MODULE_SELF_SYM=test_module_self -I"$ASL_MODERN_INCLUDEDIR" \
			-I/usr/include -I"$root/src" -c "$root/src/chan_usbradioplus_modern.c" \
			-o "$out/chan-usbradioplus-modern-test.o" &
		compile_pids="$compile_pids $!"
	fi
	compile_status=0
	for compile_pid in $compile_pids; do
		wait "$compile_pid" || compile_status=1
	done
	test "$compile_status" -eq 0
	legacy_shared_objects=$(channel_shared_object_list legacy \
		"$channel_invariant_sources $channel_variant_sources")
	if [ "$have_sys_io" -eq 1 ]; then
		sysio_shared_objects=$(channel_shared_object_list sysio \
			"$channel_invariant_sources $channel_variant_sources")
	fi
# shellcheck disable=SC2046,SC2086
cc $common -Wno-unused-function -ffunction-sections -fdata-sections \
	-DURP_PROCESSING_TESTING -DAST_MODULE='"chan_usbradioplus"' \
	-DAST_MODULE_SELF_SYM=test_module_self \
	"$root/tests/test_channel_core.c" "$out/chan-usbradioplus-test.o" \
	$legacy_shared_objects -I/usr/include -I"$root/src" \
	-Wl,--gc-sections $channel_wrap_flags -o "$out/channel-core" \
	$(pkg-config --cflags --libs rnnoise samplerate libavfilter libavutil alsa) -lusb -lm
"$out/channel-core"
completed=$((completed + 1))

# Architectures with the legacy port-I/O ABI include two ioperm() call sites
# that are absent from the normal Debian build. The harness redirects them to a
# safe test boundary. ARM has no sys/io.h; its ppdev path is covered above.
if [ "$have_sys_io" -eq 1 ]; then
	# shellcheck disable=SC2046,SC2086
	cc $common -Wno-unused-function -ffunction-sections -fdata-sections \
		-DHAVE_SYS_IO -DURP_PROCESSING_TESTING -DAST_MODULE='"chan_usbradioplus"' \
		-DAST_MODULE_SELF_SYM=test_module_self \
		"$root/tests/test_channel_core.c" "$out/chan-usbradioplus-sysio-test.o" \
		$sysio_shared_objects \
		-I/usr/include -I"$root/src" \
		-Wl,--gc-sections $channel_wrap_flags -o "$out/channel-core-sysio" \
		$(pkg-config --cflags --libs rnnoise samplerate libavfilter libavutil alsa) -lusb -lm
	"$out/channel-core-sysio"
	completed=$((completed + 1))
	sys_io_tests=1
fi

if [ -n "${ASL_MODERN_INCLUDEDIR:-}" ]; then
	modern_shared_objects=$(channel_shared_object_list modern \
		"$channel_invariant_sources $channel_variant_sources")
	# shellcheck disable=SC2046,SC2086
	cc $common -Wno-unused-function -ffunction-sections -fdata-sections \
		-DURP_TEST_MODERN -DURP_CHANNEL_MODERN -DURP_PROCESSING_TESTING \
		-DAST_MODULE='"chan_usbradioplus"' \
		-DAST_MODULE_SELF_SYM=test_module_self \
		"$root/tests/test_channel_core.c" "$out/chan-usbradioplus-modern-test.o" \
		$modern_shared_objects \
		-I"$ASL_MODERN_INCLUDEDIR" -I/usr/include -I"$root/src" \
		-Wl,--gc-sections $channel_wrap_flags -Wl,--wrap=libusb_open \
		-Wl,--wrap=libusb_close -Wl,--wrap=libusb_claim_interface \
		-Wl,--wrap=libusb_detach_kernel_driver -o "$out/channel-core-modern" \
		$(pkg-config --cflags --libs rnnoise samplerate libavfilter libavutil alsa \
			portaudio-2.0 libusb-1.0) -lm
	"$out/channel-core-modern"
	completed=$((completed + 1))
fi
fi

if run_group rnnoise; then
# shellcheck disable=SC2046,SC2086
cc $common "$root/tests/test_rnnoise_processor.c" \
	"$root/src/txagc/rnnoise_processor.c" -o "$out/rnnoise-processor" \
	$(pkg-config --cflags --libs rnnoise samplerate) -lm
"$out/rnnoise-processor"
completed=$((completed + 1))

# shellcheck disable=SC2046,SC2086
cc $common -DURP_RNNOISE_TESTING "$root/tests/test_rnnoise_failures.c" \
	"$root/src/txagc/rnnoise_processor.c" -o "$out/rnnoise-failures" \
	-Wl,--wrap=rnnoise_create -Wl,--wrap=rnnoise_destroy \
	-Wl,--wrap=rnnoise_process_frame -Wl,--wrap=src_delete -Wl,--wrap=src_new \
	-Wl,--wrap=src_process -Wl,--wrap=src_reset \
	$(pkg-config --cflags --libs rnnoise samplerate) -lm
"$out/rnnoise-failures"
completed=$((completed + 1))
fi

if run_group validation; then
# shellcheck disable=SC2086
cc $common -Wno-unused-function -ffunction-sections -fdata-sections \
	-DURP_PROCESSING_TESTING -DAST_MODULE_SELF_SYM=test_module_self \
	"$root/tests/test_processing_validation.c" "$root/src/usbradioplus_processing.c" \
	"$root/src/txagc/agc_core.c" -I/usr/include -I"$root/src" -Wl,--gc-sections \
	-o "$out/processing-validation" -lm
"$out/processing-validation"
completed=$((completed + 1))
fi

for name in avfilter_bandpass avfilter_ctcss avfilter_emphasis \
	avfilter_equalizer \
	avfilter_deesser \
	avfilter_processor avfilter_permutations; do
	if run_group "$name"; then
	# shellcheck disable=SC2046,SC2086
	cc $common "$root/tests/test_$name.c" \
		"$root/src/txagc/agc_core.c" \
		"$root/src/txagc/avfilter_processor.c" \
		-o "$out/$name" $(pkg-config --cflags --libs libavfilter libavutil) -lm
	"$out/$name"
	completed=$((completed + 1))
	fi
done

if run_group avfilter_internals; then
# shellcheck disable=SC2046,SC2086
cc $common "$root/tests/test_avfilter_internals.c" "$root/src/txagc/agc_core.c" \
	-DURP_AVFILTER_TESTING "$root/src/txagc/avfilter_processor.c" \
	-o "$out/avfilter-internals" $(pkg-config --cflags --libs libavfilter libavutil) -lm
"$out/avfilter-internals"
completed=$((completed + 1))
fi

if run_group avfilter_failures; then
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
fi

if [ -z "${C_TEST_GROUP:-}" ]; then
	expected=$((24 + sys_io_tests))
	if [ -n "${ASL_MODERN_INCLUDEDIR:-}" ]; then
		expected=$((expected + 1))
	fi
	test "$completed" -eq "$expected"
	echo "All $completed C test executables passed"
else
	echo "C test group $C_TEST_GROUP passed"
fi

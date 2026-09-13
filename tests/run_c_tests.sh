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

# The Makefile supplies these paths during staged CI builds. Direct harness
# runs use the released shared libraries' pkg-config metadata.
if [ -z "${RPCR_CFLAGS:-}" ]; then
	RPCR_CFLAGS=$(pkg-config --cflags rate_adjusting_pcm_ring)
fi
if [ -z "${RPCR_LIBS:-}" ]; then
	RPCR_LIBS=$(pkg-config --libs rate_adjusting_pcm_ring)
fi
if [ -z "${RPTADV_RADIO_CFLAGS:-}" ]; then
	RPTADV_RADIO_CFLAGS=$(pkg-config --cflags rptadvradio)
fi
if [ -z "${RPTADV_RADIO_LIBS:-}" ]; then
	RPTADV_RADIO_LIBS=$(pkg-config --libs rptadvradio)
fi
if [ -z "${RPTADV_SAMPLERATE_CFLAGS:-}" ]; then
	RPTADV_SAMPLERATE_CFLAGS=$(pkg-config --cflags rptadv_samplerate_adapter)
fi
if [ -z "${RPTADV_SAMPLERATE_LIBS:-}" ]; then
	RPTADV_SAMPLERATE_LIBS=$(pkg-config --libs rptadv_samplerate_adapter)
fi
if [ -z "${RPTADV_FFMPEG_CFLAGS:-}" ]; then
	RPTADV_FFMPEG_CFLAGS=$(pkg-config --cflags rptadv_ffmpeg_adapter)
fi
if [ -z "${RPTADV_FFMPEG_LIBS:-}" ]; then
	RPTADV_FFMPEG_LIBS=$(pkg-config --libs rptadv_ffmpeg_adapter)
fi
if [ -z "${PORTAUDIO_POC_CFLAGS:-}" ]; then
	PORTAUDIO_POC_CFLAGS=$(pkg-config --cflags rptadv_portaudio_alsa_adapter)
fi
if [ -z "${PORTAUDIO_POC_LIBS:-}" ]; then
	PORTAUDIO_POC_LIBS=$(pkg-config --libs rptadv_portaudio_alsa_adapter)
fi
if [ -z "${GPIO_POC_CFLAGS:-}" ]; then
	GPIO_POC_CFLAGS=$(pkg-config --cflags rptadv_gpio_adapter)
fi
if [ -z "${GPIO_POC_LIBS:-}" ]; then
	GPIO_POC_LIBS=$(pkg-config --libs rptadv_gpio_adapter)
fi

# Asterisk's public locking header requires GNU pthread declarations when it
# is consumed by an external module rather than Asterisk's own build.
common="-D_GNU_SOURCE -std=gnu11 -Wall -Wextra -Werror ${C_TEST_CFLAGS:-} ${RPCR_CFLAGS} \
${RPTADV_RADIO_CFLAGS} ${RPTADV_SAMPLERATE_CFLAGS} ${RPTADV_FFMPEG_CFLAGS} \
${PORTAUDIO_POC_CFLAGS} ${GPIO_POC_CFLAGS} -DURP_HAVE_PORTAUDIO_POC -DURP_HAVE_GPIO_POC"
rpcr_libs=$RPCR_LIBS
rptadv_radio_libs=$RPTADV_RADIO_LIBS
rptadv_samplerate_libs=$RPTADV_SAMPLERATE_LIBS
rptadv_ffmpeg_libs=$RPTADV_FFMPEG_LIBS

# Each parallel group gets its own instrumented plugin to avoid shared gcov
# counter writes. FFmpeg discovers only these freshly compiled test effects.
plugin_dir="$out/plugin-${C_TEST_GROUP:-parent}"
mkdir -p "$plugin_dir"
# shellcheck disable=SC2086
cc $common -fPIC -shared "$root/src/txagc/rms_agc_ladspa.c" \
	-o "$plugin_dir/usbradioplus_agc.so" -lm
export LADSPA_PATH="$plugin_dir"
completed=0
channel_invariant_sources="$root/src/usbradioplus_config.c $root/src/usbradioplus_radio.c \
$root/src/usbradioplus_host_util.c \
$root/src/usbradioplus_squelch.c \
$root/src/usbradioplus_dsp.c $root/src/usbradioplus_dcs.c \
$root/src/usbradioplus_hardware.c $root/src/usbradioplus_radio_core_adapter.c \
$root/src/usbradioplus_samplerate_adapter.c \
$root/src/usbradioplus_ffmpeg_adapter.c \
$root/src/usbradioplus_repeat.c \
$root/src/usbradioplus_channel_core.c $root/src/usbradioplus_processing.c \
$root/src/usbradioplus_rpt_advanced.c \
$root/src/txagc/agc_core.c $root/src/txagc/avfilter_processor.c \
$root/src/txagc/rnnoise_processor.c"
channel_variant_sources="$root/src/usbradioplus_channel_common.c \
$root/src/usbradioplus_native_tick.c $root/src/usbradioplus_tune_menu.c"
channel_wrap_flags="-Wl,--wrap=av_frame_alloc -Wl,--wrap=src_new -Wl,--wrap=src_process -Wl,--wrap=rpcr_init \
	-Wl,--wrap=usbradioplus_samplerate_adapter_prepare_released \
	-Wl,--wrap=usbradioplus_samplerate_adapter_process \
	-Wl,--wrap=txagc_avfilter_prepare -Wl,--wrap=txagc_avfilter_process_prepared \
	-Wl,--wrap=usbradioplus_ffmpeg_adapter_prepare_dcs \
	-Wl,--wrap=usbradioplus_ffmpeg_adapter_process_block \
	-Wl,--wrap=rpcr_set_rates \
	-Wl,--wrap=usbradioplus_processing_get_option \
	-Wl,--wrap=usbradioplus_processing_get_hardware \
	-Wl,--wrap=usbradioplus_processing_get_composite \
-Wl,--wrap=pthread_join -Wl,--wrap=write -Wl,--wrap=usleep \
-Wl,--wrap=fcntl -Wl,--wrap=poll -Wl,--wrap=pipe \
-Wl,--wrap=pipe2 \
-Wl,--wrap=usbradioplus_host_time -Wl,--wrap=usbradioplus_host_tvnow \
-Wl,--wrap=usbradioplus_host_wait_or_poll -Wl,--wrap=usbradioplus_host_poll_input \
-Wl,--wrap=usbradioplus_host_print_audio_stats"

# The groups have disjoint output names and coverage-counter files, so compile
# and execute them concurrently. C_TEST_PARALLEL=1 retains a serial diagnostic
# mode for tools that need ordered output.
if [ -z "${C_TEST_GROUP:-}" ] && [ "${C_TEST_PARALLEL:-4}" != 1 ]; then
	pids=
	for group in basic dcs_turnoff descriptor channels rnnoise rms_agc validation avfilter_bandpass \
		avfilter_ctcss avfilter_emphasis avfilter_equalizer avfilter_deesser \
		avfilter_processor avfilter_agc avfilter_permutations avfilter_internals \
		avfilter_failures avfilter_dcs; do
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

if run_group dcs_turnoff; then
# The DCS tail transition owns only scalar ACTIVE/TOC timing. This narrow C
# bridge test verifies its transactional descriptor boundary separately from
# the complete legacy radio state machine.
# shellcheck disable=SC2086
cc $common "$root/tests/test_dcs_turnoff_adapter.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/dcs-turnoff-adapter" $rptadv_radio_libs
"$out/dcs-turnoff-adapter"
completed=$((completed + 1))
fi

if run_group descriptor; then
# Setup must reject an incomplete selected descriptor before any adapter path
# can treat an append-only operation as optional.
# shellcheck disable=SC2086
cc $common -DURP_RADIO_CORE_ADAPTER_TESTING "$root/tests/test_radio_core_descriptor.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/radio-core-descriptor" $rptadv_radio_libs
"$out/radio-core-descriptor"
completed=$((completed + 1))
fi

if run_group basic; then
# Host helpers retain the tuning clock, cancellation, and meter-text behavior.
# shellcheck disable=SC2086
cc $common -DAST_MODULE='"chan_usbradioplus"' \
	-DAST_MODULE_SELF_SYM=test_module_self "$root/tests/test_host_util.c" \
	"$root/src/usbradioplus_host_util.c" -I"$root/src" \
	-Wl,--wrap=poll,--wrap=usleep,--wrap=clock_gettime -o "$out/host-util" -lm
"$out/host-util"
completed=$((completed + 1))

# shellcheck disable=SC2086
cc $common "$root/tests/test_stage_order.c" \
	"$root/src/txagc/agc_core.c" -I"$root/src" -o "$out/stage-order"
"$out/stage-order"
completed=$((completed + 1))

# Compiler and pkg-config flag lists intentionally undergo POSIX word splitting.
# shellcheck disable=SC2046,SC2086
cc $common -DURP_TEST_ALLOCATORS "$root/tests/test_usbradioplus_dsp.c" \
	"$root/src/usbradioplus_dsp.c" "$root/src/usbradioplus_samplerate_adapter.c" \
	-o "$out/dsp" $(pkg-config --cflags --libs samplerate) $rptadv_samplerate_libs -lm
"$out/dsp"
completed=$((completed + 1))

# The facade's descriptor checks run against a deterministic fake table. The
# linked shared object is present only to prove the released descriptor symbol
# remains resolvable by a consumer.
# shellcheck disable=SC2086
cc $common "$root/tests/test_samplerate_adapter_facade.c" \
	"$root/src/usbradioplus_samplerate_adapter.c" -I"$root/src" \
	-o "$out/samplerate-adapter-facade" $rptadv_samplerate_libs
"$out/samplerate-adapter-facade"
completed=$((completed + 1))

# shellcheck disable=SC2086
cc $common -Wno-unused-variable "$root/tools/legacy_ctcss_reference.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/ctcss-reference" -lm $rptadv_radio_libs
"$out/ctcss-reference"
completed=$((completed + 1))

# shellcheck disable=SC2086
cc $common "$root/tests/test_ctcss_helpers.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/ctcss-helpers" -lm $rptadv_radio_libs
"$out/ctcss-helpers"
completed=$((completed + 1))

# The raw PCM bridge uses the released portable descriptor but preserves the
# exact signed-16 meter units consumed by the existing tune utility.
# shellcheck disable=SC2086
cc $common "$root/tests/test_audio_meter_adapter.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/audio-meter-adapter" $rptadv_radio_libs
"$out/audio-meter-adapter"
completed=$((completed + 1))

# The ordinary native discriminator frontend is an append-only portable-core
# operation. Its bridge keeps signed-16 compatibility state transactional so
# the retained C implementation can take over unchanged on a rejected call.
# shellcheck disable=SC2086
cc $common "$root/tests/test_receive_frontend_adapter.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/receive-frontend-adapter" $rptadv_radio_libs
"$out/receive-frontend-adapter"
completed=$((completed + 1))

# Sample-clocked signaling timing has an append-only portable primitive. The
# bridge test checks the exact remainder and post-expiry residual contract.
# shellcheck disable=SC2086
cc $common "$root/tests/test_timer_adapter.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/timer-adapter" $rptadv_radio_libs
"$out/timer-adapter"
completed=$((completed + 1))

# The CTCSS/DCS mode-hold decision is a narrow append-only Rust operation.
# Verify the adapter's transactional descriptor boundary independently of the
# complete legacy radio state machine.
# shellcheck disable=SC2086
cc $common "$root/tests/test_signal_mode_adapter.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/signal-mode-adapter" $rptadv_radio_libs
	"$out/signal-mode-adapter"
	completed=$((completed + 1))

# The final oscillator-facing CTCSS start/turn-off/disable transition is also
# an append-only Rust operation. Verify its deferred-disable contract at the
# narrow descriptor boundary before the complete native state-machine test.
# shellcheck disable=SC2086
cc $common "$root/tests/test_ctcss_render_state_adapter.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/ctcss-render-state-adapter" $rptadv_radio_libs
"$out/ctcss-render-state-adapter"
completed=$((completed + 1))

# The ordinary 80 ms transmitter finishing drain is a narrow append-only Rust
# operation. Test its descriptor transaction before the native state-machine
# suite exercises the same transition alongside PTT and signaling behavior.
# shellcheck disable=SC2086
cc $common "$root/tests/test_tx_finish_adapter.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/tx-finish-adapter" $rptadv_radio_libs
"$out/tx-finish-adapter"

# Exercise the post-drain scalar cleanup at the narrow descriptor boundary
# before the complete native state-machine test.
# shellcheck disable=SC2086
cc $common "$root/tests/test_tx_complete_adapter.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/tx-complete-adapter" $rptadv_radio_libs
"$out/tx-complete-adapter"
completed=$((completed + 1))

# Post-transmit RX blanking keeps the PCM mute loop in C while the portable
# core owns only its bounded state arithmetic. Exercise that ABI separately.
# shellcheck disable=SC2086
cc $common "$root/tests/test_rx_blanking_adapter.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/rx-blanking-adapter" $rptadv_radio_libs
"$out/rx-blanking-adapter"
completed=$((completed + 1))

# The legacy VOX envelope remains in C, while its pure timer/carrier result is
# an append-only portable operation. Check same-callback expiry and partition
# timing at the descriptor boundary before the complete radio-core harness.
# shellcheck disable=SC2086
cc $common "$root/tests/test_vox_carrier_adapter.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/vox-carrier-adapter" $rptadv_radio_libs
"$out/vox-carrier-adapter"
completed=$((completed + 1))

# The transmitter CPU saver now derives only its halt flag through the
# append-only core. Exercise the full compatibility truth table independently
# of the renderer's retained early-return branch.
# shellcheck disable=SC2086
cc $common "$root/tests/test_tx_cpu_saver_adapter.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/tx-cpu-saver-adapter" $rptadv_radio_libs
"$out/tx-cpu-saver-adapter"
completed=$((completed + 1))

# The receiver CPU saver returns only a scalar halt transition. The retained C
# receiver loop remains responsible for the HPF/deemphasis stage writes.
# shellcheck disable=SC2086
cc $common "$root/tests/test_rx_cpu_saver_adapter.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/rx-cpu-saver-adapter" $rptadv_radio_libs
"$out/rx-cpu-saver-adapter"
completed=$((completed + 1))

# The optional direct PortAudio proof uses this bounded handoff to preserve
# current RF audio if the Asterisk delivery worker falls behind. It has no
# hardware or Asterisk dependency, so test it in every C-test invocation.
# shellcheck disable=SC2086
cc $common "$root/tests/test_portaudio_poc_handoff.c" \
	"$root/src/usbradioplus_portaudio_poc_handoff.c" -I"$root/src" \
	-o "$out/portaudio-poc-handoff"
"$out/portaudio-poc-handoff"
completed=$((completed + 1))

# The ordinary 8 kHz direct-PortAudio proof retains actual streaming-SRC
# output until a complete established 20 ms app_rpt handoff is available.
# This pure helper test deliberately needs no Asterisk or PortAudio device.
# shellcheck disable=SC2086
cc $common "$root/tests/test_portaudio_poc_receive_assembler.c" -I"$root/src" \
	-o "$out/portaudio-poc-receive-assembler"
"$out/portaudio-poc-receive-assembler"
completed=$((completed + 1))

# Status events have their own bounded callback-to-worker queue so an audio
# handoff recovery cannot lose CTCSS-ready or voter text.
# shellcheck disable=SC2086
cc $common "$root/tests/test_portaudio_poc_status.c" \
	"$root/src/usbradioplus_portaudio_poc_status.c" \
	"$root/src/usbradioplus_portaudio_poc_handoff.c" -I"$root/src" \
	-o "$out/portaudio-poc-status"
"$out/portaudio-poc-status"
completed=$((completed + 1))

# shellcheck disable=SC2086
cc $common "$root/tests/test_dcs.c" "$root/src/usbradioplus_dcs.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/dcs" -lm $rptadv_radio_libs
"$out/dcs"
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
	"$root/src/usbradioplus_channel_core.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/channel-shared-core" -lm $rptadv_radio_libs
"$out/channel-shared-core"
completed=$((completed + 1))

# shellcheck disable=SC2086
cc $common "$root/tests/test_native_repeat.c" \
	"$root/src/usbradioplus_repeat.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/native-repeat" $rptadv_radio_libs
"$out/native-repeat"
completed=$((completed + 1))

# shellcheck disable=SC2086
cc $common "$root/tests/test_micor_squelch.c" \
	"$root/src/usbradioplus_squelch.c" "$root/src/usbradioplus_radio_core_adapter.c" -I"$root/src" \
	-o "$out/micor-squelch" $rptadv_radio_libs
"$out/micor-squelch"
completed=$((completed + 1))

C_TEST_OUTPUT="$out" C_TEST_CFLAGS="${C_TEST_CFLAGS:-}" \
	RPTADV_RADIO_CFLAGS="$RPTADV_RADIO_CFLAGS" RPTADV_RADIO_LIBS="$RPTADV_RADIO_LIBS" \
	sh "$root/tests/run_radio_core_tests.sh"
completed=$((completed + 2))
fi

if run_group channels; then
	channel_include=${ASTERISK_INCLUDEDIR:-/usr/include}
	## @brief Compile shared channel sources as separate test-instrumented objects.
	compile_channel_shared()
	{
		variant=$1
		variant_flags=$2
		variant_sources=$3
		variant_include=$4
		compile_pids=
		channel_pkg_cflags=$(pkg-config --cflags rnnoise samplerate libavfilter libavutil)
		for source in $variant_sources; do
			base=$(basename "$source" .c)
			object="$out/channel-$variant-$base.o"
			# Compiler flag lists intentionally undergo POSIX word splitting.
			# shellcheck disable=SC2086
			cc $common $variant_flags $channel_pkg_cflags \
				-DAST_MODULE='"chan_usbradioplus"' \
				-DAST_MODULE_SELF_SYM=test_module_self \
				-I"$variant_include" -I/usr/include -I"$root/src" \
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

	hardware_sources="$root/src/usbradioplus_portaudio_poc.c \
$root/src/usbradioplus_portaudio_poc_handoff.c \
$root/src/usbradioplus_portaudio_poc_selection.c \
$root/src/usbradioplus_portaudio_poc_status.c \
$root/src/usbradioplus_portaudio_poc_timing.c \
$root/src/usbradioplus_hardware_adapter.c \
$root/src/usbradioplus_cm119_gpio_poc_worker.c \
$root/src/usbradioplus_hardware_eeprom_poc.c \
$root/src/usbradioplus_hardware_gpio_poc.c \
$root/src/usbradioplus_hardware_mixer_poc.c \
$root/src/usbradioplus_parallel_adapter_poc.c \
$root/src/usbradioplus_portaudio_poc_identity.c"
	channel_sources="$channel_invariant_sources $channel_variant_sources $hardware_sources"
	channel_flags="-DURP_PROCESSING_TESTING -DURP_PORTAUDIO_POC_CALLBACK_TEST"
	compile_channel_shared selected "$channel_flags" "$channel_sources" "$channel_include"

	# Compile the same channel translation unit shipped in the module.
	# shellcheck disable=SC2086
	cc $common $channel_flags -Wno-unused-function -ffunction-sections -fdata-sections \
		-DURP_CHANNEL_UNIT_TEST -DAST_MODULE='"chan_usbradioplus"' \
		-DAST_MODULE_SELF_SYM=test_module_self -I"$channel_include" -I"$root/src" \
		-c "$root/src/chan_usbradioplus.c" -o "$out/chan-usbradioplus-test.o"
	channel_objects=$(channel_shared_object_list selected "$channel_sources")
	# Descriptors are replaced by deterministic test doubles; no physical device opens.
	# shellcheck disable=SC2046,SC2086
	cc $common $channel_flags -Wno-unused-function -ffunction-sections -fdata-sections \
		-DAST_MODULE='"chan_usbradioplus"' -DAST_MODULE_SELF_SYM=test_module_self \
		"$root/tests/test_channel_core.c" "$out/chan-usbradioplus-test.o" \
		$channel_objects -I"$channel_include" -I"$root/src" \
		-Wl,--gc-sections $channel_wrap_flags -o "$out/channel-core" \
		$(pkg-config --cflags --libs rnnoise samplerate libavfilter libavutil) -lm \
		$rpcr_libs $rptadv_radio_libs $rptadv_samplerate_libs $rptadv_ffmpeg_libs \
		$PORTAUDIO_POC_LIBS $GPIO_POC_LIBS
	"$out/channel-core"
	completed=$((completed + 1))

	# Selection and timing use fake descriptor tables and never open devices.
	# shellcheck disable=SC2086
	cc $common "$root/tests/test_portaudio_poc_selection.c" \
		"$root/src/usbradioplus_portaudio_poc_selection.c" -I"$root/src" \
		-o "$out/portaudio-poc-selection"
	"$out/portaudio-poc-selection"
	completed=$((completed + 1))
	# shellcheck disable=SC2086
	cc $common "$root/tests/test_portaudio_poc_timing.c" \
		"$root/src/usbradioplus_portaudio_poc_timing.c" -I"$root/src" \
		-o "$out/portaudio-poc-timing"
	"$out/portaudio-poc-timing"
	completed=$((completed + 1))
	# The common worker has no hardware or Asterisk dependency. These fake
	# callbacks exercise startup, service, wake, and ordered cleanup paths.
	# shellcheck disable=SC2086
	cc $common "$root/tests/test_cm119_gpio_poc_worker.c" \
		"$root/src/usbradioplus_cm119_gpio_poc_worker.c" -I"$root/src" \
		-o "$out/cm119-gpio-poc-worker"
	"$out/cm119-gpio-poc-worker"
	completed=$((completed + 1))
	# The hardware-composition facade uses deterministic fake descriptors, so it
	# verifies the released audio/GPIO ABI boundary without opening a device.
	# shellcheck disable=SC2046,SC2086
	cc $common $PORTAUDIO_POC_CFLAGS $GPIO_POC_CFLAGS \
		"$root/tests/test_hardware_adapter_facade.c" \
		"$root/src/usbradioplus_hardware_adapter.c" \
		-I"$root/src" \
		-o "$out/hardware-adapter-facade"
	"$out/hardware-adapter-facade"
	completed=$((completed + 1))
	# EEPROM words remain a compatible 13-word view, while the
	# GPIO adapter owns the physical 64-word transfer and checksum.
	# shellcheck disable=SC2046,SC2086
	cc $common $PORTAUDIO_POC_CFLAGS $GPIO_POC_CFLAGS \
		"$root/tests/test_hardware_eeprom_poc.c" \
		"$root/src/usbradioplus_hardware_eeprom_poc.c" -I"$root/src" \
		-o "$out/hardware-eeprom-poc"
	"$out/hardware-eeprom-poc"
	completed=$((completed + 1))
	# The GPIO bridge is deterministic and uses only fake facade callbacks. It
	# verifies ordinary GPIO and the legacy non-extending clip-LED behavior.
	# shellcheck disable=SC2046,SC2086
	cc $common $PORTAUDIO_POC_CFLAGS $GPIO_POC_CFLAGS \
		"$root/tests/test_hardware_gpio_poc.c" \
		"$root/src/usbradioplus_hardware_adapter.c" \
		"$root/src/usbradioplus_hardware_gpio_poc.c" -I"$root/src" \
		-o "$out/hardware-gpio-poc"
	"$out/hardware-gpio-poc"
	completed=$((completed + 1))
	# The semantic mixer bridge is exercised through fake adapter paths. It
	# proves normalized RX/TX A/TX B control without opening an ALSA card.
	# shellcheck disable=SC2046,SC2086
	cc $common $PORTAUDIO_POC_CFLAGS $GPIO_POC_CFLAGS \
	"$root/tests/test_hardware_mixer_poc.c" \
	"$root/src/usbradioplus_hardware_adapter.c" \
	"$root/src/usbradioplus_hardware_mixer_poc.c" -I"$root/src" \
	-o "$out/hardware-mixer-poc"
"$out/hardware-mixer-poc"
completed=$((completed + 1))
	# The parallel bridge keeps ppdev/raw-I/O calls in the released GPIO facade.
	# Fake descriptors cover absent, malformed, output failure, and service paths.
	# shellcheck disable=SC2086
	cc $common $PORTAUDIO_POC_CFLAGS $GPIO_POC_CFLAGS \
		"$root/tests/test_parallel_adapter_poc.c" \
		"$root/src/usbradioplus_hardware_adapter.c" \
		"$root/src/usbradioplus_parallel_adapter_poc.c" -I"$root/src" \
		-o "$out/parallel-adapter-poc"
	"$out/parallel-adapter-poc"
	completed=$((completed + 1))
	# The selected composition's fail-closed identity gate is pure C logic. It confirms
	# that no numeric PortAudio selection can bypass an unprepared facade or a
	# mismatched CM119 mixer/HID identity.
	# shellcheck disable=SC2046,SC2086
	cc $common $PORTAUDIO_POC_CFLAGS $GPIO_POC_CFLAGS \
		"$root/tests/test_portaudio_poc_identity.c" \
		"$root/src/usbradioplus_portaudio_poc_identity.c" -I"$root/src" \
		-o "$out/portaudio-poc-identity"
	"$out/portaudio-poc-identity"
	completed=$((completed + 1))
fi

if run_group rnnoise; then
# shellcheck disable=SC2046,SC2086
cc $common "$root/tests/test_rnnoise_processor.c" \
	"$root/src/txagc/rnnoise_processor.c" \
	-o "$out/rnnoise-processor" $(pkg-config --cflags --libs rnnoise) -lm
"$out/rnnoise-processor"
completed=$((completed + 1))

# shellcheck disable=SC2046,SC2086
cc $common "$root/tests/test_rnnoise_failures.c" \
	"$root/src/txagc/rnnoise_processor.c" \
	-o "$out/rnnoise-failures" \
	-Wl,--wrap=rnnoise_create -Wl,--wrap=rnnoise_destroy \
	-Wl,--wrap=rnnoise_process_frame \
	$(pkg-config --cflags --libs rnnoise) -lm
"$out/rnnoise-failures"
completed=$((completed + 1))
fi

if run_group validation; then
# shellcheck disable=SC2086
cc $common -Wno-unused-function -ffunction-sections -fdata-sections \
	-DURP_PROCESSING_TESTING -DAST_MODULE_SELF_SYM=test_module_self \
	"$root/tests/test_processing_validation.c" "$root/src/usbradioplus_processing.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" "$root/src/txagc/agc_core.c" \
	-I/usr/include -I"$root/src" -Wl,--gc-sections \
	-o "$out/processing-validation" -lm $rptadv_radio_libs
"$out/processing-validation"
completed=$((completed + 1))
fi

if run_group rms_agc; then
# shellcheck disable=SC2086
cc $common "$root/tests/test_rms_agc_ladspa.c" \
	"$root/src/txagc/rms_agc_ladspa.c" -Wl,--wrap=calloc \
	-o "$out/rms-agc" -lm
"$out/rms-agc"
completed=$((completed + 1))
fi

for name in avfilter_bandpass avfilter_ctcss avfilter_emphasis \
	avfilter_equalizer \
	avfilter_deesser \
	avfilter_processor avfilter_agc avfilter_permutations; do
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

if run_group avfilter_dcs; then
# The public adapter boundary is tested independently of radio synthesis.
# shellcheck disable=SC2086
cc $common "$root/tests/test_ffmpeg_adapter_facade.c" \
	"$root/src/usbradioplus_ffmpeg_adapter.c" -I"$root/src" \
	-o "$out/ffmpeg-adapter-facade" $rptadv_ffmpeg_libs -lm
"$out/ffmpeg-adapter-facade"
completed=$((completed + 1))
# DCS synthesis is filtered only through the same shared FFmpeg graph used by
# the module. This proves the transmitted data has no uncontrolled high-band
# NRZ energy before it joins the hardware output routes.
# shellcheck disable=SC2046,SC2086
cc $common "$root/tests/test_avfilter_dcs.c" "$root/src/usbradioplus_ffmpeg_adapter.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" \
	-I"$root/src" -o "$out/avfilter-dcs" \
	-lm $rptadv_radio_libs $rptadv_ffmpeg_libs
"$out/avfilter-dcs"
completed=$((completed + 1))
fi

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
cc $common -pthread "$root/tests/test_avfilter_failures.c" "$root/src/txagc/agc_core.c" \
	"$root/src/txagc/avfilter_processor.c" -o "$out/avfilter-failures" \
	-Wl,--wrap=avfilter_graph_alloc -Wl,--wrap=avfilter_graph_create_filter \
	-Wl,--wrap=avfilter_inout_alloc -Wl,--wrap=avfilter_graph_parse_ptr \
	-Wl,--wrap=avfilter_graph_config -Wl,--wrap=av_audio_fifo_alloc \
	-Wl,--wrap=av_frame_alloc -Wl,--wrap=av_frame_get_buffer \
	-Wl,--wrap=av_buffersrc_add_frame_flags -Wl,--wrap=av_buffersink_get_frame \
	-Wl,--wrap=av_audio_fifo_realloc -Wl,--wrap=av_audio_fifo_write -Wl,--wrap=av_mallocz \
	-Wl,--wrap=sched_yield \
	$(pkg-config --cflags --libs libavfilter libavutil) -lm
"$out/avfilter-failures"
completed=$((completed + 1))
fi

if [ -z "${C_TEST_GROUP:-}" ]; then
	test "$completed" -gt 0
	echo "All $completed C test executables passed"
else
	echo "C test group $C_TEST_GROUP passed"
fi

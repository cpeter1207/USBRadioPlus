/** @file
 * @brief Direct native receive processing and transmitter rendering.
 */

#include "asterisk.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "asterisk/frame.h"
#include "asterisk/utils.h"

#include "txagc/avfilter_processor.h"
#include "txagc/rnnoise_processor.h"
#include "usbradioplus_channel_core.h"
#include "usbradioplus_ctcss.h"
#include "usbradioplus_radio.h"
#include "usbradioplus_radio_core_adapter.h"
#include "usbradioplus_samplerate_adapter.h"
#include "usbradioplus_repeat.h"
#include "usbradioplus_channel_private.h"
#include "usbradioplus_channel_common.h"

/** Snapshot of one hardware callback's signaling state. */
struct native_renderer_input {
	/** Receiver qualification for CPU saving and local repeat. */
	int rxkeyed;
	/** Native DTMF muting indication. */
	int toneflag;
	/** Nonzero when Asterisk DTMF detection is active. */
	int usedtmf;
	/** Nonzero when a DSP detector supplies DTMF state. */
	int has_dsp;
	/** Selected receive CTCSS decoder index. */
	int decoded_ctcss;
	/** Per-sample carrier gate from the signaling frontend. */
	uint8_t carrier_gate[URP_NATIVE_MAX_SAMPLES];
	/** Signaling-engine transmitter assertion for this frame. */
	int tx_ptt_out;
	/** Signaling-engine transmit state-machine state. */
	int tx_state;
	/** Nonzero while continuous transmit CTCSS is enabled. */
	int tx_ctcss_enabled;
	/** Nonzero while the CTCSS turn-off mode suppresses the tone. */
	int tx_ctcss_off;
	/** Configured CTCSS phase-shift angle. */
	double tx_ctcss_phase_shift;
	/** Replacement CTCSS tail-tone frequency in hertz. */
	double tx_ctcss_tail_tone_hz;
	/** Continuous transmit CTCSS frequency in hertz. */
	double tx_ctcss_frequency_hz;
	/** Continuous transmit CTCSS PCM peak. */
	double tx_ctcss_peak;
	/** Nonzero while transmit DCS is configured. */
	int dcs_enabled;
	/** Nonzero while the DCS turn-off sequence is active. */
	int dcs_turnoff_active;
	/** Configured transmit DCS code. */
	int dcs_transmit_code;
	/** Nonzero for inverted transmit DCS polarity. */
	int dcs_transmit_inverted;
	/** Transmit DCS PCM peak. */
	double dcs_peak;
	/** Nonzero while the calibrated test tone replaces program audio. */
	int test_tone_enabled;
};

/** One immutable renderer diagnostics buffer pinned by control-plane readers. */
struct native_renderer_diagnostics {
	/** Native renderer diagnostic measurements. */
	struct usbradioplus_native_renderer_stats statistics;
	/** Portable transmitter audio meter. */
	struct rptadv_radio_audio_statistics tx_audio_statistics;
};

/** Persistent state owned solely by the hardware callback.
 *
 * Setup preallocates all workspaces, converter state, and RNNoise. The
 * callback renders each complete CM119 block directly, with no worker queue,
 * scheduler handoff, allocation, or lock between ADC and DAC.
 */
struct usbradioplus_native_renderer {
	/** Channel whose native hardware callback owns this renderer. */
	struct chan_usbradio_pvt *channel;
	/** Fixed-rate portable core object for native transmitter rendering. */
	struct rptadv_radio *radio_core;
	/** Last DCS code applied to the transmit generator. */
	int dcs_code;
	/** Last DCS polarity applied to the transmit generator. */
	int dcs_inverted;
	/** Last DCS code applied to the portable receive decoder. */
	int dcs_receive_code;
	/** Last DCS polarity applied to the portable receive decoder. */
	int dcs_receive_inverted;
	/** Last CTCSS table mask applied to the portable receive decoder. */
	uint64_t ctcss_receive_mask;
	/** Last CTCSS relaxed-talk-off setting applied to the portable decoder. */
	int ctcss_receive_relax;
	/** Canonical f32 hardware-bound receiver PCM. */
	float adc_stereo[URP_NATIVE_STEREO_SAMPLES];
	/** Canonical f32 post-filter CTCSS decoder PCM. */
	float ctcss_input[SAMPLES_PER_BLOCK];
	/** Delayed canonical f32 native-rate receiver PCM. */
	float rx_native[URP_NATIVE_MAX_SAMPLES];
	/** Processed native-rate local-receive workspace. */
	double local_native[URP_NATIVE_MAX_SAMPLES];
	/** Native-rate app_rpt program workspace. */
	short link_native[URP_NATIVE_MAX_SAMPLES];
	/** Source-rate legacy echo workspace. */
	short link_app[URP_NATIVE_MAX_SAMPLES];
	/** Native-rate receiver data before conversion to app_rpt. */
	short receive_app_input[URP_NATIVE_MAX_SAMPLES];
	/** Mixed final-transmit PCM workspace. */
	double program[URP_NATIVE_MAX_SAMPLES];
	/** Local repeat or native-parrot PCM workspace. */
	double local_program[URP_NATIVE_MAX_SAMPLES];
	/** Bounded f32 bridge workspace for the Rust-owned native repeat stage. */
	struct urp_native_repeat_workspace repeat_workspace;
	/** Bounded f32 bridge workspace for Rust-owned transmitter rendering. */
	struct urp_transmit_render_workspace transmit_workspace;
	/** Filtered normalized DCS PCM, separate from the generator input span. */
	float dcs_program[URP_NATIVE_MAX_SAMPLES];
	/** Interleaved DAC PCM used for transmitter measurements. */
	short statistics_stereo[URP_NATIVE_STEREO_SAMPLES];
	/** Canonical f32 receive squelch-delay storage. */
	float rx_delay[RXSQDELAYBUFSIZE * 6];
	/** Circular position in the receive squelch-delay storage. */
	unsigned int rx_delay_index;
	/** Legacy echo and RX-to-legacy-app conversion only. */
	struct urp_src *echo_up;
	/** Persistent native-to-app_rpt sample-rate converter. */
	struct urp_src *down;
	/** App_rpt source rate currently installed in the converters. */
	unsigned int app_rpt_rate;
	/** RNNoise is local-receive-only, before local dynamics. */
	struct txagc_rnnoise local_rnnoise;
	/** Caller-owned canonical-F32 native-parrot recording storage. */
	float *parrot_audio;
	/** Allocated native-parrot storage capacity in samples. */
	uint32_t parrot_capacity;
	/** Callback-local snapshot of Rust-owned native-parrot cursors. */
	struct rptadv_radio_native_parrot_status parrot_status;
	/** Previous receiver qualification for parrot transitions. */
	int previous_rxkeyed;
	/** Mutable renderer diagnostics, written only by the callback. */
	struct usbradioplus_native_renderer_stats statistics;
	/** Mutable portable transmitter meter, written only by the callback. */
	struct rptadv_radio_audio_statistics tx_audio_statistics;
	/** Double-buffered diagnostics exposed to control-plane readers. */
	struct native_renderer_diagnostics published_statistics[2];
	/** Index of the diagnostics buffer currently published to readers. */
	atomic_uint statistics_index;
	/** Reader pins for each published diagnostics buffer. */
	atomic_uint statistics_readers[2];
	/** Monotonic measurement-reset request from the control plane. */
	atomic_uint statistics_reset_request;
	/** Last measurement-reset request consumed by the callback. */
	unsigned int statistics_reset_seen;
	/** Monotonic native-parrot clear request from the control plane. */
	atomic_uint parrot_clear_request;
	/** Last native-parrot clear request consumed by the callback. */
	unsigned int parrot_clear_seen;
	/** Monotonic legacy-echo clear request from the control plane. */
	atomic_uint legacy_echo_clear_request;
	/** Last legacy-echo clear request consumed by the callback. */
	unsigned int legacy_echo_clear_seen;
};

/** @brief Decode DCS at the established post-blanking radio-core hook.
 * @param context Callback-owned native renderer.
 * @param samples Raw interleaved signed-16 ADC PCM after receive blanking.
 * @param frame_count Number of native frames in @p samples.
 * @param stride PCM words between selected left-channel samples.
 * @param sample_rate Fixed native ADC rate.
 * @param valid Receives portable-core DCS qualification.
 * @return Zero when the Rust decoder supplied @p valid; nonzero clears qualification.
 *
 * The C radio state machine calls this only at its existing DCS decision point,
 * after TX/RX blanking and before signaling-mode selection.  This preserves
 * input ordering while keeping all portable decode state inside librptadvradio.
 */
static int native_renderer_decode_dcs(void *context, const int16_t *samples, size_t frame_count,
				      size_t stride, unsigned int sample_rate, int *valid)
{
	struct usbradioplus_native_renderer *renderer = context;
	const struct chan_usbradio_pvt *channel;
	int code;
	int inverted;
	size_t index;

	if (valid)
		*valid = 0;
	if (!renderer || !samples || !valid || stride != 2U || sample_rate != URP_RATE_NATIVE ||
	    frame_count > URP_NATIVE_MAX_SAMPLES)
		return -1;
	/* Only a fully constructed renderer publishes this callback and context. */
	channel = renderer->channel;
	if (!channel->radio || !channel->radio->dcs.enabled_receive)
		return -1;
	code = channel->radio->dcs.receive_code;
	inverted = channel->radio->dcs.receive_inverted;
	if (renderer->dcs_receive_code != code || renderer->dcs_receive_inverted != inverted) {
		if (urp_radio_core_configure_dcs_receive(renderer->radio_core, code, inverted))
			return -1;
		renderer->dcs_receive_code = code;
		renderer->dcs_receive_inverted = inverted;
	}
	for (index = 0; index < frame_count * 2U; ++index)
		renderer->adc_stereo[index] = (float)samples[index] / 32768.0F;
	return urp_radio_core_process_dcs_receive(renderer->radio_core, renderer->adc_stereo,
						  frame_count, valid);
}

/** @brief Decode CTCSS at the established post-frontend radio-core hook.
 * @param context Callback-owned native renderer.
 * @param samples Signed-16 center-slicer PCM after the legacy CTCSS frontend.
 * @param sample_count Number of 8 kHz samples in @p samples.
 * @param tone_mask Configured legacy CTCSS receive-tone indexes.
 * @param relax Nonzero selects relaxed talk-off behavior.
 * @param carrier_detect Current compatibility carrier decision.
 * @param decoded Receives portable-core CTCSS qualification.
 * @return Zero when the Rust decoder supplied @p decoded; nonzero clears qualification.
 *
 * The C radio state machine invokes this at its former decoder call site,
 * after blanking, frontend filtering, and carrier determination. It retains
 * C-owned diagnostics; Rust owns the only decoder history and implementation.
 */
static int native_renderer_decode_ctcss(void *context, const int16_t *samples, size_t sample_count,
					uint64_t tone_mask, int relax, int carrier_detect,
					int *decoded)
{
	struct usbradioplus_native_renderer *renderer = context;
	const struct chan_usbradio_pvt *channel;
	size_t index;

	if (decoded)
		*decoded = CTCSS_NULL;
	if (!renderer || !samples || !decoded || sample_count > ARRAY_LEN(renderer->ctcss_input))
		return -1;
	/* Startup binds a valid core and owning channel before publishing this hook. */
	channel = renderer->channel;
	if (!channel->radio || !channel->radio->rxCtcss)
		return -1;
	if (renderer->ctcss_receive_mask != tone_mask || renderer->ctcss_receive_relax != !!relax) {
		if (urp_radio_core_configure_ctcss_receive(renderer->radio_core, tone_mask, relax))
			return -1;
		renderer->ctcss_receive_mask = tone_mask;
		renderer->ctcss_receive_relax = !!relax;
	}
	for (index = 0; index < sample_count; ++index)
		renderer->ctcss_input[index] = (float)samples[index] / 32768.0F;
	return urp_radio_core_process_ctcss_receive(renderer->radio_core, renderer->ctcss_input,
						    sample_count, carrier_detect, decoded);
}

#ifdef URP_PROCESSING_TESTING
static atomic_uint native_renderer_test_statistics_retry_count;
static atomic_uint native_renderer_test_hardware_snapshot_delta;
/** @brief Make a lock-free diagnostics reader retry in deterministic tests.
 * @param renderer Renderer whose published index is being read.
 * @param index Diagnostics buffer index pinned by the reader.
 */
static void
native_renderer_test_invalidate_statistics_read(struct usbradioplus_native_renderer *renderer,
						unsigned int index)
{
	unsigned int remaining = atomic_load_explicit(&native_renderer_test_statistics_retry_count,
						      memory_order_acquire);

	if (remaining != 0U) {
		atomic_store_explicit(&native_renderer_test_statistics_retry_count, remaining - 1U,
				      memory_order_release);
		atomic_store_explicit(&renderer->statistics_index, index ^ 1U,
				      memory_order_release);
	}
}
#endif

/** @brief Initialize direct-renderer measurement values.
 * @param renderer Renderer whose mutable measurements are initialized.
 */
static void native_renderer_statistics_init(struct usbradioplus_native_renderer *renderer)
{
	memset(&renderer->statistics, 0, sizeof(renderer->statistics));
	renderer->statistics.adc_peak_dbfs = -INFINITY;
	renderer->statistics.adc_max_peak_dbfs = -INFINITY;
	renderer->statistics.preemphasis_input_peak_dbfs = -INFINITY;
	renderer->statistics.preemphasis_input_max_peak_dbfs = -INFINITY;
	renderer->statistics.local_tx_peak_dbfs = -INFINITY;
	renderer->statistics.local_tx_max_peak_dbfs = -INFINITY;
	renderer->statistics.tx_program_peak_dbfs = -INFINITY;
	renderer->statistics.tx_program_max_peak_dbfs = -INFINITY;
}

/** @brief Publish callback-owned diagnostics without blocking the callback.
 * @param renderer Renderer that owns the diagnostics snapshot.
 */
static void native_renderer_publish_statistics(struct usbradioplus_native_renderer *renderer)
{
	unsigned int active =
		atomic_load_explicit(&renderer->statistics_index, memory_order_acquire);
	unsigned int inactive = active ^ 1U;

	if (atomic_load_explicit(&renderer->statistics_readers[inactive], memory_order_acquire) !=
	    0U)
		return;
	renderer->published_statistics[inactive].statistics = renderer->statistics;
	renderer->published_statistics[inactive].tx_audio_statistics =
		renderer->tx_audio_statistics;
	atomic_store_explicit(&renderer->statistics_index, inactive, memory_order_release);
}

/** @brief Copy scalar measurements from one prepared FFmpeg graph.
 * @param destination Destination native diagnostic fields.
 * @param source Prepared FFmpeg graph supplying the measurements.
 */
static void
native_renderer_copy_filter_statistics(struct usbradioplus_native_filter_statistics *destination,
				       const struct txagc_avfilter *source)
{
	destination->latency_samples = source->latency_samples;
	destination->buffered_samples = source->buffered_samples;
	destination->input_samples = source->input_samples;
	destination->output_samples = source->output_samples;
	destination->startup_fill_samples = source->startup_fill_samples;
	destination->runtime_underrun_samples = source->runtime_underrun_samples;
	destination->input_peak_dbfs = source->input_peak_dbfs;
	destination->input_max_peak_dbfs = source->input_max_peak_dbfs;
	destination->input_rms_dbfs = source->input_rms_dbfs;
	destination->input_max_rms_dbfs = source->input_max_rms_dbfs;
	destination->output_peak_dbfs = source->output_peak_dbfs;
	destination->output_max_peak_dbfs = source->output_max_peak_dbfs;
	destination->output_rms_dbfs = source->output_rms_dbfs;
	destination->output_max_rms_dbfs = source->output_max_rms_dbfs;
	destination->cleanup_pre_peak_dbfs = source->cleanup_pre_peak_dbfs;
	destination->cleanup_pre_max_peak_dbfs = source->cleanup_pre_max_peak_dbfs;
	destination->cleanup_pre_rms_dbfs = source->cleanup_pre_rms_dbfs;
	destination->cleanup_pre_max_rms_dbfs = source->cleanup_pre_max_rms_dbfs;
	destination->cleanup_pre_5_8_rms_dbfs = source->cleanup_pre_5_8_rms_dbfs;
	destination->cleanup_pre_5_8_max_rms_dbfs = source->cleanup_pre_5_8_max_rms_dbfs;
	destination->cleanup_pre_8_plus_rms_dbfs = source->cleanup_pre_8_plus_rms_dbfs;
	destination->cleanup_pre_8_plus_max_rms_dbfs = source->cleanup_pre_8_plus_max_rms_dbfs;
	destination->cleanup_post_5_8_rms_dbfs = source->cleanup_post_5_8_rms_dbfs;
	destination->cleanup_post_5_8_max_rms_dbfs = source->cleanup_post_5_8_max_rms_dbfs;
	destination->cleanup_post_8_plus_rms_dbfs = source->cleanup_post_8_plus_rms_dbfs;
	destination->cleanup_post_8_plus_max_rms_dbfs = source->cleanup_post_8_plus_max_rms_dbfs;
}

/** @brief Update the direct transmitter meter.
 * @param renderer Renderer that owns the transmitter meter.
 * @param samples Interleaved transmitter PCM samples.
 * @param count Number of PCM samples in @p samples.
 */
static void native_renderer_check_tx_audio(struct usbradioplus_native_renderer *renderer,
					   short *samples, size_t count)
{
	int clipping;

	/* The raw transmitter meter crosses an exact signed-16-to-F32 boundary.
	 * Keep the last published measurement if a malformed span is rejected;
	 * switching meter implementations would make its history inconsistent. */
	(void)urp_radio_core_measure_audio_s16((const int16_t *)samples, count, 2U,
					       &renderer->tx_audio_statistics, renderer->adc_stereo,
					       URP_NATIVE_STEREO_SAMPLES, &clipping);
}

/** @brief Reset extrema in one prepared graph at a requested measurement boundary.
 * @param filter Prepared graph whose extrema are reset.
 */
static void native_renderer_reset_filter_statistics(struct txagc_avfilter *filter)
{
	filter->input_max_peak_dbfs = -INFINITY;
	filter->input_max_rms_dbfs = -INFINITY;
	filter->output_max_peak_dbfs = -INFINITY;
	filter->output_max_rms_dbfs = -INFINITY;
	filter->cleanup_pre_max_peak_dbfs = -INFINITY;
	filter->cleanup_pre_max_rms_dbfs = -INFINITY;
	filter->cleanup_pre_5_8_max_rms_dbfs = -INFINITY;
	filter->cleanup_pre_8_plus_max_rms_dbfs = -INFINITY;
	filter->cleanup_post_5_8_max_rms_dbfs = -INFINITY;
	filter->cleanup_post_8_plus_max_rms_dbfs = -INFINITY;
	filter->runtime_underrun_samples = 0;
}

/** @brief Refresh the native-parrot cursor snapshot owned by the Rust core.
 * @param renderer Renderer whose callback owns both the core and snapshot.
 *
 * The shared core is accessed only by its native callback, so this remains a
 * bounded lock-free read.  A failed descriptor call cannot happen after setup,
 * but a zero snapshot preserves safe diagnostics if one is ever corrupted.
 */
static void native_renderer_refresh_parrot_status(struct usbradioplus_native_renderer *renderer)
{
	if (urp_radio_core_native_parrot_status(renderer->radio_core, &renderer->parrot_status))
		memset(&renderer->parrot_status, 0, sizeof(renderer->parrot_status));
}

/** @brief Apply pending control-plane requests at a callback boundary.
 * @param renderer Renderer consuming the requests.
 * @param graphs Active native graph generation.
 */
static void native_renderer_apply_requests(struct usbradioplus_native_renderer *renderer,
					   struct usbradioplus_native_graph_set *graphs)
{
	unsigned int request =
		atomic_load_explicit(&renderer->statistics_reset_request, memory_order_acquire);

	if (request != renderer->statistics_reset_seen) {
		native_renderer_statistics_init(renderer);
		native_renderer_reset_filter_statistics(&graphs->local_dynamics);
		native_renderer_reset_filter_statistics(&graphs->final);
		renderer->statistics_reset_seen = request;
	}
	request = atomic_load_explicit(&renderer->parrot_clear_request, memory_order_acquire);
	if (request != renderer->parrot_clear_seen) {
		(void)urp_radio_core_native_parrot_reset(renderer->radio_core);
		native_renderer_refresh_parrot_status(renderer);
		renderer->previous_rxkeyed = 0;
		atomic_store_explicit(&renderer->channel->echoing, 0, memory_order_release);
		renderer->parrot_clear_seen = request;
	}
	request = atomic_load_explicit(&renderer->legacy_echo_clear_request, memory_order_acquire);
	if (request != renderer->legacy_echo_clear_seen) {
		/* The callback owns this queue's consumer cursor. */
		urp_sample_queue_discard(&renderer->channel->echo_queue);
		atomic_store_explicit(&renderer->channel->echoing, 0, memory_order_release);
		renderer->legacy_echo_clear_seen = request;
	}
}

/** @brief Consume one native program frame from the sole app_rpt bridge.
 * @param channel Channel that owns the program ring.
 * @param graphs Active graph generation supplying program rate and target.
 * @param output Receives one native-rate program frame.
 * @param frame_count Number of native-rate frames requested.
 * @return Nonzero when every output sample came from the program ring.
 *
 * Its persistent converter handles every configured source rate, including a
 * nominal one-to-one native-rate stream, and provides concealment.
 */
static int read_native_program(struct chan_usbradio_pvt *channel,
			       const struct usbradioplus_native_graph_set *graphs, short *output,
			       size_t frame_count)
{
	struct rpcr_ring *ring = &channel->plus_program_ring;
	size_t index;
	int complete = 1;

	if (!ring->capacity)
		return 0;
	/* Reserve is an observable protection floor; the per-sample consumer uses
	 * target only for its persistent, slow clock-ratio controller. */
	atomic_store_explicit(&ring->reserve_samples, graphs->program_reserve_samples,
			      memory_order_relaxed);
	for (index = 0U; index < frame_count; ++index) {
		if (!rpcr_consumer_render_sample(ring, &output[index],
						 graphs->program_target_samples))
			complete = 0;
	}
	return complete;
}

/** @brief Apply fixed receive filtering and the selected decoded-tone notch.
 * @param input Per-block signaling snapshot selecting the optional notch.
 * @param graphs Active native graph generation.
 * @param samples Native receive PCM updated in place.
 * @param frame_count Number of native PCM samples in @p samples.
 */
static void process_receive_filter(const struct native_renderer_input *input,
				   struct usbradioplus_native_graph_set *graphs, double *samples,
				   size_t frame_count)
{
	int decoded = input->decoded_ctcss;

	(void)txagc_avfilter_process_prepared(&graphs->receive_filter, samples, frame_count);
	if (decoded > CTCSS_NULL && decoded < CTCSS_NUM_CODES &&
	    graphs->ctcss_notch[decoded].configured)
		(void)txagc_avfilter_process_prepared(&graphs->ctcss_notch[decoded], samples,
						      frame_count);
}

/** @brief Read a coherent routing snapshot without taking a hardware lock.
 * @param channel Channel that owns the applied hardware routing.
 * @param txmixa Receives output-A routing.
 * @param txmixb Receives output-B routing.
 */
static void read_hardware_snapshot(const struct chan_usbradio_pvt *channel,
				   enum radio_tx_mix *txmixa, enum radio_tx_mix *txmixb)
{
	unsigned int attempt;

	for (attempt = 0; attempt < 2U; ++attempt) {
		unsigned int before;
		unsigned int after;

		before = atomic_load_explicit(&channel->plus_hardware_generation,
					      memory_order_acquire);
		if (before & 1U)
			continue;
		*txmixa = (enum radio_tx_mix)atomic_load_explicit(&channel->plus_applied_txmixa,
								  memory_order_relaxed);
		*txmixb = (enum radio_tx_mix)atomic_load_explicit(&channel->plus_applied_txmixb,
								  memory_order_relaxed);
		after = atomic_load_explicit(&channel->plus_hardware_generation,
					     memory_order_acquire);
#ifdef URP_PROCESSING_TESTING
		after += atomic_exchange_explicit(&native_renderer_test_hardware_snapshot_delta, 0U,
						  memory_order_acq_rel);
#endif
		if (before == after)
			return;
	}
	/* Never wait for a control-plane writer in an audio callback. */
	*txmixa = (enum radio_tx_mix)atomic_load_explicit(&channel->plus_applied_txmixa,
							  memory_order_relaxed);
	*txmixb = (enum radio_tx_mix)atomic_load_explicit(&channel->plus_applied_txmixb,
							  memory_order_relaxed);
}

/** @brief Copy signaling state that must remain internally consistent per block.
 * @param snapshot Receives the block's signaling snapshot.
 * @param channel Channel supplying current radio and DTMF state.
 * @param frame_count Number of native samples represented by the snapshot.
 */
static void native_renderer_snapshot(struct native_renderer_input *snapshot,
				     const struct chan_usbradio_pvt *channel, size_t frame_count)
{
	const urp_radio_state *radio = channel->radio;

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->decoded_ctcss = CTCSS_NULL;
	snapshot->rxkeyed = channel->rxkeyed;
	snapshot->toneflag = channel->toneflag;
	snapshot->usedtmf = channel->usedtmf;
	snapshot->has_dsp = channel->dsp != NULL;
	if (radio) {
		if (radio->rxCtcss)
			snapshot->decoded_ctcss = radio->rxCtcss->decode;
		memcpy(snapshot->carrier_gate, radio->rxCarrierGate,
		       frame_count * sizeof(*snapshot->carrier_gate));
		snapshot->tx_ptt_out = radio->txPttOut;
		snapshot->tx_state = radio->txState;
		snapshot->tx_ctcss_enabled = radio->txCtcssEnabled;
		snapshot->tx_ctcss_off = radio->b.txCtcssOff;
		snapshot->tx_ctcss_phase_shift = radio->txCtcssPhaseShift;
		snapshot->tx_ctcss_tail_tone_hz = radio->txCtcssTailToneHz;
		snapshot->tx_ctcss_frequency_hz = radio->txCtcssFreq10 / 10.0;
		snapshot->tx_ctcss_peak = radio->txCtcssPeak;
		snapshot->dcs_enabled = radio->dcs.enabled_transmit;
		snapshot->dcs_turnoff_active = radio->dcs.enabled_transmit &&
					       radio->txState == CHAN_TXSTATE_TOC &&
					       radio->dcsTurnoffTimer > 0;
		snapshot->dcs_transmit_code = radio->dcs.transmit_code;
		snapshot->dcs_transmit_inverted = radio->dcs.transmit_inverted;
		snapshot->dcs_peak = radio->dcsPeak;
	}
	snapshot->test_tone_enabled =
		atomic_load_explicit(&channel->plus_test_tone_enabled, memory_order_acquire);
}

/** @brief Convert one native callback duration to an exact app-facing duration.
 * @param app_rate Nonzero app-facing rate checked by the capacity boundary.
 * @param native_frame_count Nonzero bounded native callback span.
 * @param app_frame_count Non-NULL local output for the app-facing sample count.
 * @return Zero when the duration has an exact bounded app-facing representation.
 *
 * Fixed compatibility frames and legacy app-rate echo require an exact
 * matching duration. Direct callback receive processing uses the separate
 * capacity helper below and retains only the samples its streaming SRC emits.
 */
static int native_renderer_app_frame_count(unsigned int app_rate, size_t native_frame_count,
					   size_t *app_frame_count)
{
	size_t scaled;

	scaled = native_frame_count * app_rate;
	if (scaled % URP_RATE_NATIVE)
		return -1;
	scaled /= URP_RATE_NATIVE;
	if (scaled > URP_NATIVE_MAX_SAMPLES)
		return -1;
	*app_frame_count = scaled;
	return 0;
}

/** @brief Bound converter output for one arbitrary native callback span.
 * @param app_rate Active app-facing sample rate in Hz.
 * @param native_frame_count Nonzero bounded native callback span.
 * @param app_capacity Non-NULL local output for the app-facing capacity.
 * @return Zero when the callback has a bounded app-facing conversion.
 *
 * Ordinary direct callbacks may end between app-rate sample boundaries.  A
 * streaming SRC can produce one carried sample beyond the duration ceiling,
 * so fractional spans reserve that one extra sample.  Exact spans retain the
 * historical capacity, which keeps the established 960-to-160 path unchanged.
 */
static int native_renderer_app_frame_capacity(unsigned int app_rate, size_t native_frame_count,
					      size_t *app_capacity)
{
	size_t exact_count;
	size_t scaled;
	size_t capacity;

	if (!app_rate)
		return -1;
	if (!native_renderer_app_frame_count(app_rate, native_frame_count, &exact_count)) {
		*app_capacity = exact_count;
		return 0;
	}
	scaled = native_frame_count * (size_t)app_rate;
	capacity = scaled / URP_RATE_NATIVE + 1U;
	/* libsamplerate can flush one carried output sample on a fractional span. */
	if (capacity >= URP_NATIVE_MAX_SAMPLES)
		return -1;
	*app_capacity = capacity + 1U;
	return 0;
}

/** @brief Copy processed local receive to the app_rpt receive frame.
 * @param renderer Renderer that owns the local-receive workspace and converter.
 * @param graphs Active graph generation supplying app_rpt format details.
 * @param app_pcm Receives the app_rpt-format receive frame.
 * @param native_frame_count Number of local-receive native PCM samples.
 * @param app_capacity Preallocated app_rpt PCM capacity for this native span.
 * @return Number of app_rpt samples actually emitted by the streaming converter.
 */
static size_t
native_renderer_copy_receive_to_app(struct usbradioplus_native_renderer *renderer,
				    const struct usbradioplus_native_graph_set *graphs,
				    short *app_pcm, size_t native_frame_count, size_t app_capacity)
{
	size_t index;
	size_t used = 0U;
	size_t made = 0U;

	for (index = 0; index < native_frame_count; ++index) {
		double sample = renderer->local_native[index];

		renderer->receive_app_input[index] =
			urp_apply_gain((short)fmax(-32768.0, fmin(32767.0, sample)), 1.0);
	}
	if (graphs->app_rpt_rate == URP_RATE_NATIVE) {
		memcpy(app_pcm, renderer->receive_app_input, native_frame_count * sizeof(*app_pcm));
		return native_frame_count;
	}
	if (urp_rate_convert_prepared(renderer->down, renderer->receive_app_input,
				      native_frame_count, URP_RATE_NATIVE, app_pcm, app_capacity,
				      graphs->app_rpt_rate, &used, &made) ||
	    used != native_frame_count) {
		renderer->statistics.src_errors++;
		memset(app_pcm, 0, app_capacity * sizeof(*app_pcm));
		return 0U;
	}
	return made;
}

/** @brief Run the local receive branch directly in the hardware callback.
 * @param renderer Renderer that owns receive processing state and workspaces.
 * @param input Per-block signaling snapshot.
 * @param graphs Active native graph generation.
 * @param adc_pcm Native ADC PCM for this block.
 * @param app_pcm Receives the corresponding app_rpt receive frame.
 * @param frame_count Number of native PCM samples in this block.
 * @param app_capacity Preallocated app_rpt PCM capacity for this native span.
 * @return Number of app_rpt samples actually emitted by the receive branch.
 *
 * RNNoise is intentionally confined here: after deemphasis, squelch gate,
 * input gain, and receive/PL filtering, and before local dynamics. It never
 * processes app_rpt program audio or the transmitter/DAC branch.
 */
static size_t native_renderer_render_receive(struct usbradioplus_native_renderer *renderer,
					     const struct native_renderer_input *input,
					     struct usbradioplus_native_graph_set *graphs,
					     const short *adc_pcm, short *app_pcm,
					     size_t frame_count, size_t app_capacity)
{
	float peak = 0.0F;
	unsigned long rail_samples = 0U;
	size_t index;

	/* The hardware/Asterisk boundary is signed 16-bit PCM.  The portable core
	 * receives only canonical f32 and preserves the existing left-channel
	 * extraction, raw measurement, and pre-deemphasis delay ordering. */
	for (index = 0; index < frame_count * 2U; ++index)
		renderer->adc_stereo[index] = (float)adc_pcm[index] / 32768.0F;
	if (urp_radio_core_extract_receive(renderer->radio_core, renderer->adc_stereo,
					   renderer->rx_native, frame_count, renderer->rx_delay,
					   graphs->receive_squelch_delay_samples,
					   &renderer->rx_delay_index, &peak, &rail_samples)) {
		memset(renderer->rx_native, 0, frame_count * sizeof(*renderer->rx_native));
		peak = 0.0F;
		rail_samples = 0U;
	}
	for (index = 0; index < frame_count; ++index)
		renderer->local_native[index] = (double)renderer->rx_native[index] * 32768.0;
	renderer->statistics.adc_peak_dbfs =
		urp_pcm_peak_dbfs((unsigned int)lrintf(fminf(1.0F, peak) * 32768.0F));
	if (renderer->statistics.adc_peak_dbfs > renderer->statistics.adc_max_peak_dbfs)
		renderer->statistics.adc_max_peak_dbfs = renderer->statistics.adc_peak_dbfs;
	renderer->statistics.adc_rail_samples += rail_samples;
	(void)txagc_avfilter_process_prepared(&graphs->receive_deemphasis, renderer->local_native,
					      frame_count);
	if (graphs->noise_squelch_gate) {
		for (index = 0; index < frame_count; ++index)
			if (!input->carrier_gate[index])
				renderer->local_native[index] = 0.0;
	}
	for (index = 0; index < frame_count; ++index)
		renderer->local_native[index] *= graphs->local_input_gain_linear;
	process_receive_filter(input, graphs, renderer->local_native, frame_count);
	if (graphs->local_chain_enabled && (!graphs->receive_cpu_saver || input->rxkeyed)) {
		if (graphs->local_rnnoise_enabled)
			(void)txagc_rnnoise_process_prepared(&renderer->local_rnnoise,
							     renderer->local_native, frame_count);
		else
			txagc_rnnoise_bypass(&renderer->local_rnnoise);
		(void)txagc_avfilter_process_prepared(&graphs->local_dynamics,
						      renderer->local_native, frame_count);
	} else {
		txagc_rnnoise_bypass(&renderer->local_rnnoise);
	}
	renderer->statistics.rnnoise_frames = renderer->local_rnnoise.rnnoise_frames;
	renderer->statistics.rnnoise_output_samples = renderer->local_rnnoise.output_samples;
	renderer->statistics.rnnoise_startup_samples = renderer->local_rnnoise.startup_samples;
	renderer->statistics.rnnoise_errors = renderer->local_rnnoise.errors;
	renderer->statistics.rnnoise_vad_probability = renderer->local_rnnoise.vad_probability;
	if (graphs->legacy_interface && graphs->echo_mode) {
		int playback_started = 0;

		if (!urp_radio_core_native_parrot_rx_transition(
			    renderer->radio_core, renderer->previous_rxkeyed, input->rxkeyed,
			    &playback_started)) {
			native_renderer_refresh_parrot_status(renderer);
			if (playback_started)
				atomic_store_explicit(&renderer->channel->echoing, 1,
						      memory_order_release);
		}
	}
	renderer->previous_rxkeyed = input->rxkeyed;
	return native_renderer_copy_receive_to_app(renderer, graphs, app_pcm, frame_count,
						   app_capacity);
}

/** @brief Generate direct CTCSS and DCS sources for one DAC frame.
 * @param renderer Renderer that owns the signaling generators.
 * @param input Per-block transmit signaling snapshot.
 * @param graphs Active native graph generation supplying DCS filters.
 * @param frame_count Number of native PCM samples to generate.
 */
static void native_renderer_generate_signaling(struct usbradioplus_native_renderer *renderer,
					       const struct native_renderer_input *input,
					       struct usbradioplus_native_graph_set *graphs,
					       size_t frame_count)
{
	int dcs_normal_active;
	int ctcss_ready;
	int dcs_ready = 1;

	if (input->tx_ctcss_tail_tone_hz > 0.0)
		ctcss_ready = urp_radio_core_generate_ctcss_tail(
				      renderer->radio_core, renderer->transmit_workspace.ctcss,
				      frame_count, input->tx_ctcss_tail_tone_hz, 1.0F,
				      input->tx_ctcss_enabled && !input->tx_ctcss_off) == 0;
	else
		ctcss_ready = urp_radio_core_generate_ctcss(
				      renderer->radio_core, renderer->transmit_workspace.ctcss,
				      frame_count, input->tx_ctcss_frequency_hz, 1.0F,
				      input->tx_ctcss_enabled && !input->tx_ctcss_off,
				      input->tx_ctcss_phase_shift) == 0;
	if (!ctcss_ready)
		memset(renderer->transmit_workspace.ctcss, 0,
		       frame_count * sizeof(*renderer->transmit_workspace.ctcss));
	if (renderer->dcs_code != input->dcs_transmit_code ||
	    renderer->dcs_inverted != input->dcs_transmit_inverted) {
		if (!urp_radio_core_configure_dcs(renderer->radio_core, input->dcs_transmit_code,
						  input->dcs_transmit_inverted)) {
			renderer->dcs_code = input->dcs_transmit_code;
			renderer->dcs_inverted = input->dcs_transmit_inverted;
		} else {
			dcs_ready = 0;
		}
	}
	dcs_normal_active =
		input->dcs_enabled && input->tx_ptt_out && input->tx_state == CHAN_TXSTATE_ACTIVE;
	if (dcs_ready &&
	    urp_radio_core_generate_dcs(renderer->radio_core, renderer->transmit_workspace.dcs,
					frame_count, input->dcs_peak / 32767.0,
					dcs_normal_active || input->dcs_turnoff_active,
					input->dcs_turnoff_active))
		dcs_ready = 0;
	if (!dcs_ready) {
		memset(renderer->transmit_workspace.dcs, 0,
		       frame_count * sizeof(*renderer->transmit_workspace.dcs));
	}
	if (usbradioplus_ffmpeg_adapter_process_block(
		    input->dcs_turnoff_active ? &graphs->dcs_turnoff : &graphs->dcs,
		    renderer->transmit_workspace.dcs, (uint32_t)frame_count,
		    renderer->dcs_program) != 0)
		memset(renderer->dcs_program, 0, frame_count * sizeof(*renderer->dcs_program));
	if (!dcs_normal_active && !input->dcs_turnoff_active)
		memset(renderer->dcs_program, 0, frame_count * sizeof(*renderer->dcs_program));
}

/** @brief Render the transmitter branch directly into the hardware DAC buffer.
 * @param renderer Renderer that owns transmit workspaces and generators.
 * @param input Per-block signaling snapshot.
 * @param graphs Active native graph generation.
 * @param stereo Receives interleaved native DAC PCM.
 * @param frame_count Number of native PCM samples to render.
 * @param app_frame_count Matching app_rpt PCM sample count.
 *
 * This branch contains app_rpt program audio, local repeat/parrot audio,
 * final transmit filtering, test tone, CTCSS/DCS generation, and routing. It
 * deliberately contains no RNNoise processing.
 */
static void native_renderer_render_transmit(struct usbradioplus_native_renderer *renderer,
					    const struct native_renderer_input *input,
					    struct usbradioplus_native_graph_set *graphs,
					    short *stereo, size_t frame_count,
					    size_t app_frame_count)
{
	struct chan_usbradio_pvt *channel = renderer->channel;
	double ctcss_peak_a = input->tx_ctcss_peak;
	enum radio_tx_mix txmixa;
	enum radio_tx_mix txmixb;
	size_t index;
	size_t used = 0U;
	size_t made = 0U;

	memset(stereo, 0, frame_count * 2U * sizeof(*stereo));
	native_renderer_generate_signaling(renderer, input, graphs, frame_count);
	memset(renderer->link_native, 0, frame_count * sizeof(*renderer->link_native));
	if (graphs->legacy_interface && app_frame_count &&
	    atomic_load_explicit(&channel->echoing, memory_order_acquire)) {
		int have_frame = 0;

		if (urp_sample_queue_samples(&channel->echo_queue) >= app_frame_count) {
			for (index = 0; index < app_frame_count; ++index)
				(void)urp_sample_queue_pop_sample(&channel->echo_queue,
								  &renderer->link_app[index]);
			have_frame = 1;
		}
		if (!have_frame) {
			if (atomic_load_explicit(&channel->txkeyed, memory_order_acquire))
				renderer->statistics.link_queue_underflows++;
			urp_src_reset(renderer->echo_up);
		} else if (graphs->app_rpt_rate == URP_RATE_NATIVE) {
			memcpy(renderer->link_native, renderer->link_app,
			       frame_count * sizeof(*renderer->link_native));
		} else if (urp_rate_convert_prepared(renderer->echo_up, renderer->link_app,
						     app_frame_count, graphs->app_rpt_rate,
						     renderer->link_native, frame_count,
						     URP_RATE_NATIVE, &used, &made) ||
			   used != app_frame_count) {
			renderer->statistics.src_errors++;
			memset(renderer->link_native, 0,
			       frame_count * sizeof(*renderer->link_native));
		} else if (made < frame_count) {
			memset(renderer->link_native + made, 0,
			       (frame_count - made) * sizeof(*renderer->link_native));
		}
	} else if (!read_native_program(channel, graphs, renderer->link_native, frame_count) &&
		   atomic_load_explicit(&channel->txkeyed, memory_order_acquire)) {
		renderer->statistics.link_queue_underflows++;
	}
	for (index = 0; index < frame_count; ++index)
		renderer->program[index] = renderer->link_native[index];
	memset(renderer->local_program, 0, frame_count * sizeof(*renderer->local_program));
	if (graphs->legacy_interface && renderer->parrot_status.playing) {
		size_t played = 0U;

		/* The core deliberately leaves the tail untouched when playback ends
		 * inside this span, so preserve the former zero-filled local workspace. */
		memset(renderer->transmit_workspace.program, 0,
		       frame_count * sizeof(*renderer->transmit_workspace.program));
		if (!urp_radio_core_native_parrot_play(renderer->radio_core,
						       renderer->transmit_workspace.program,
						       frame_count, &played)) {
			for (index = 0; index < played; ++index)
				renderer->local_program[index] =
					(double)renderer->transmit_workspace.program[index] *
					32768.0;
		}
		renderer->statistics.parrot_playback_frames++;
		native_renderer_refresh_parrot_status(renderer);
		if (!renderer->parrot_status.playing)
			atomic_store_explicit(&channel->echoing, 0, memory_order_release);
	} else if (graphs->legacy_interface && input->rxkeyed && graphs->software_repeat_enabled) {
		(void)urp_native_repeat_prepare(renderer->local_program, renderer->local_native,
						frame_count, 1.0,
						input->usedtmf && input->has_dsp && input->toneflag,
						&renderer->repeat_workspace);
		if (graphs->echo_mode) {
			size_t recorded;

			(void)urp_radio_core_native_parrot_record(
				renderer->radio_core, renderer->repeat_workspace.output,
				frame_count, renderer->parrot_capacity, &recorded);
		}
		for (index = 0; index < frame_count; ++index)
			renderer->local_program[index] *= graphs->software_repeat_gain;
	}
	{
		double peak = urp_double_peak(renderer->local_program, frame_count);

		renderer->statistics.preemphasis_input_peak_dbfs =
			peak > 0.0 ? 20.0 * log10(peak / 32768.0) : -INFINITY;
		if (renderer->statistics.preemphasis_input_peak_dbfs >
		    renderer->statistics.preemphasis_input_max_peak_dbfs)
			renderer->statistics.preemphasis_input_max_peak_dbfs =
				renderer->statistics.preemphasis_input_peak_dbfs;
		renderer->statistics.local_tx_peak_dbfs =
			peak > 0.0 ? 20.0 * log10(peak / 32768.0) : -INFINITY;
		if (renderer->statistics.local_tx_peak_dbfs >
		    renderer->statistics.local_tx_max_peak_dbfs)
			renderer->statistics.local_tx_max_peak_dbfs =
				renderer->statistics.local_tx_peak_dbfs;
	}
	for (index = 0; index < frame_count; ++index)
		renderer->program[index] += renderer->local_program[index];
	if (txagc_avfilter_process_prepared(&graphs->final, renderer->program, frame_count) < 0)
		memset(renderer->program, 0, frame_count * sizeof(*renderer->program));
	/* The shared Rust core owns the calibration oscillator. The existing f32
	 * transmit workspace avoids adding another allocation; it is converted back
	 * only at this temporary legacy-PCM boundary before routing. */
	if (urp_radio_core_render_calibrated_test_tone(renderer->radio_core,
						       renderer->transmit_workspace.program,
						       frame_count, input->test_tone_enabled)) {
		if (input->test_tone_enabled)
			memset(renderer->program, 0, frame_count * sizeof(*renderer->program));
	} else if (input->test_tone_enabled) {
		for (index = 0; index < frame_count; ++index)
			renderer->program[index] =
				(double)renderer->transmit_workspace.program[index] * 32767.0;
	}
	{
		double peak = urp_double_peak(renderer->program, frame_count);

		renderer->statistics.tx_program_peak_dbfs =
			peak > 0.0 ? 20.0 * log10(peak / 32768.0) : -INFINITY;
		if (renderer->statistics.tx_program_peak_dbfs >
		    renderer->statistics.tx_program_max_peak_dbfs)
			renderer->statistics.tx_program_max_peak_dbfs =
				renderer->statistics.tx_program_peak_dbfs;
	}
	read_hardware_snapshot(channel, &txmixa, &txmixb);
	{
		unsigned long rail_samples = 0;
		const double ctcss_peak_b = ctcss_peak_a;

		if (urp_render_transmit_block(
			    renderer->radio_core, renderer->program,
			    renderer->transmit_workspace.ctcss, renderer->dcs_program, frame_count,
			    (enum urp_tx_output_mode)txmixa, (enum urp_tx_output_mode)txmixb,
			    ctcss_peak_a, 0.0, ctcss_peak_b, 0.0, &renderer->transmit_workspace,
			    stereo, renderer->statistics_stereo, &rail_samples)) {
			/* A verified descriptor makes this unreachable during normal operation.
			 * Preserve safe DAC behavior if a corrupted context is ever detected. */
			memset(stereo, 0, frame_count * 2U * sizeof(*stereo));
			memset(renderer->statistics_stereo, 0,
			       frame_count * 2U * sizeof(*renderer->statistics_stereo));
		} else {
			renderer->statistics.tx_program_rail_samples += rail_samples;
		}
	}
	native_renderer_check_tx_audio(renderer, renderer->statistics_stereo, frame_count * 2U);
}

/** @brief Publish measurements after one direct block.
 * @param renderer Renderer whose diagnostics are published.
 * @param graphs Active native graph generation supplying filter measurements.
 */
static void native_renderer_finish_block(struct usbradioplus_native_renderer *renderer,
					 struct usbradioplus_native_graph_set *graphs)
{
	renderer->statistics.native_frames++;
	native_renderer_refresh_parrot_status(renderer);
	renderer->statistics.parrot_samples = renderer->parrot_status.recorded_samples;
	renderer->statistics.parrot_playing = !!renderer->parrot_status.playing;
	renderer->statistics.parrot_truncated = !!renderer->parrot_status.truncated;
	native_renderer_copy_filter_statistics(&renderer->statistics.local_filter,
					       &graphs->local_dynamics);
	native_renderer_copy_filter_statistics(&renderer->statistics.receive_deemphasis_filter,
					       &graphs->receive_deemphasis);
	native_renderer_copy_filter_statistics(&renderer->statistics.final_filter, &graphs->final);
	native_renderer_publish_statistics(renderer);
}

#ifdef URP_PROCESSING_TESTING
/** @brief Copy callback-local native observables into the test channel.
 * @param renderer Renderer that completed the native PCM span.
 * @param frame_count Native PCM frames produced by this callback.
 */
static void native_renderer_publish_test_observables(struct usbradioplus_native_renderer *renderer,
						     size_t frame_count)
{
	struct chan_usbradio_pvt *channel = renderer->channel;

	/* The test observables represent this callback only.  Clearing the unused
	 * tail avoids exposing a prior larger callback when the native span varies. */
	memset(channel->plus_rx_native, 0, sizeof(channel->plus_rx_native));
	memset(channel->plus_local_native, 0, sizeof(channel->plus_local_native));
	memset(channel->plus_link_native, 0, sizeof(channel->plus_link_native));
	for (size_t index = 0; index < frame_count; ++index) {
		float sample = renderer->rx_native[index];

		channel->plus_rx_native[index] =
			(short)lrintf(fmaxf(-1.0F, fminf(32767.0F / 32768.0F, sample)) * 32768.0F);
	}
	memcpy(channel->plus_local_native, renderer->local_native,
	       frame_count * sizeof(*channel->plus_local_native));
	memcpy(channel->plus_link_native, renderer->link_native,
	       frame_count * sizeof(*channel->plus_link_native));
	channel->plus_native_frames = renderer->statistics.native_frames;
	channel->plus_src_errors = renderer->statistics.src_errors;
	channel->plus_adc_peak_dbfs = renderer->statistics.adc_peak_dbfs;
	channel->plus_adc_max_peak_dbfs = renderer->statistics.adc_max_peak_dbfs;
	channel->plus_adc_rail_samples = renderer->statistics.adc_rail_samples;
	channel->plus_preemphasis_input_peak_dbfs =
		renderer->statistics.preemphasis_input_peak_dbfs;
	channel->plus_preemphasis_input_max_peak_dbfs =
		renderer->statistics.preemphasis_input_max_peak_dbfs;
	channel->plus_local_tx_peak_dbfs = renderer->statistics.local_tx_peak_dbfs;
	channel->plus_local_tx_max_peak_dbfs = renderer->statistics.local_tx_max_peak_dbfs;
	channel->plus_tx_program_peak_dbfs = renderer->statistics.tx_program_peak_dbfs;
	channel->plus_tx_program_max_peak_dbfs = renderer->statistics.tx_program_max_peak_dbfs;
	channel->plus_tx_program_rail_samples = renderer->statistics.tx_program_rail_samples;
	channel->plus_link_queue_underflows = renderer->statistics.link_queue_underflows;
	channel->plus_parrot_playback_frames = renderer->statistics.parrot_playback_frames;
	channel->plus_parrot_playing = !!renderer->parrot_status.playing;
	channel->plus_parrot_count = renderer->parrot_status.recorded_samples;
	channel->plus_parrot_play = renderer->parrot_status.playback_offset;
	channel->plus_parrot_truncated = !!renderer->parrot_status.truncated;
	if (urp_radio_core_ctcss_phase(renderer->radio_core, &channel->plus_ctcss_generator.phase))
		channel->plus_ctcss_generator.phase = 0.0;
	if (urp_radio_core_calibrated_test_tone_phase(renderer->radio_core,
						      &channel->plus_test_tone_phase))
		channel->plus_test_tone_phase = 0.0;
}
#endif

/** @brief Fill direct app and DAC destinations with one silent native block.
 * @param channel Channel whose frame buffers receive silence.
 * @param frame_count Number of native PCM samples to clear.
 * @param app_frame_count Number of matching app_rpt PCM samples to clear.
 */
static void native_renderer_silence(struct chan_usbradio_pvt *channel, size_t frame_count,
				    size_t app_frame_count)
{
	if (app_frame_count)
		memset(channel->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET, 0,
		       app_frame_count * sizeof(short));
	memset(channel->usbradio_write_buf, 0, frame_count * 2U * sizeof(short));
}

void usbradioplus_native_renderer_bind_radio(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_renderer *renderer;

	if (!channel || !channel->radio || !channel->plus_native_renderer)
		return;
	renderer = channel->plus_native_renderer;
	if (channel->radio->dcs.receive_callback != native_renderer_decode_dcs ||
	    channel->radio->dcs.receive_callback_context != renderer) {
		renderer->dcs_receive_code = -2;
		renderer->dcs_receive_inverted = -1;
	}
	if (channel->radio->rxCtcss &&
	    (channel->radio->rxCtcss->receive_callback != native_renderer_decode_ctcss ||
	     channel->radio->rxCtcss->receive_callback_context != renderer)) {
		renderer->ctcss_receive_mask = UINT64_MAX;
		renderer->ctcss_receive_relax = -1;
	}
	urp_dcs_set_receive_callback(&channel->radio->dcs, native_renderer_decode_dcs, renderer);
	urp_ctcss_set_receive_callback(channel->radio->rxCtcss, native_renderer_decode_ctcss,
				       renderer);
}

int usbradioplus_native_renderer_start(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_renderer *renderer;

	if (!channel || channel->plus_native_renderer)
		return channel ? 0 : -1;
	if (urp_native_repeat_initialize())
		return -1;
	renderer = ast_calloc(1, sizeof(*renderer));
	if (!renderer)
		return -1;
	renderer->channel = channel;
	if (urp_radio_core_create(URP_RATE_NATIVE, URP_NATIVE_MAX_SAMPLES, &renderer->radio_core)) {
		ast_free(renderer);
		return -1;
	}
	native_renderer_statistics_init(renderer);
	atomic_init(&renderer->statistics_index, 0U);
	atomic_init(&renderer->statistics_readers[0], 0U);
	atomic_init(&renderer->statistics_readers[1], 0U);
	atomic_init(&renderer->statistics_reset_request, 0U);
	atomic_init(&renderer->parrot_clear_request, 0U);
	atomic_init(&renderer->legacy_echo_clear_request, 0U);
	renderer->dcs_code = -2;
	renderer->dcs_inverted = -1;
	renderer->dcs_receive_code = -2;
	renderer->dcs_receive_inverted = -1;
	renderer->ctcss_receive_mask = UINT64_MAX;
	renderer->ctcss_receive_relax = -1;
	renderer->echo_up = urp_src_create(RPTADV_SAMPLERATE_QUALITY_SINC_BEST, 1);
	renderer->down = urp_src_create(RPTADV_SAMPLERATE_QUALITY_SINC_BEST, 1);
	if ((size_t)DEFAULT_ECHO_MAX * URP_NATIVE_SAMPLES > UINT32_MAX) {
		urp_src_destroy(renderer->echo_up);
		urp_src_destroy(renderer->down);
		urp_radio_core_destroy(renderer->radio_core);
		ast_free(renderer);
		return -1;
	}
	renderer->parrot_capacity = (uint32_t)((size_t)DEFAULT_ECHO_MAX * URP_NATIVE_SAMPLES);
	renderer->parrot_audio =
		ast_calloc(renderer->parrot_capacity, sizeof(*renderer->parrot_audio));
	txagc_rnnoise_init(&renderer->local_rnnoise);
	if (!renderer->echo_up || !renderer->down || !renderer->parrot_audio ||
	    urp_radio_core_native_parrot_bind(renderer->radio_core, renderer->parrot_audio,
					      renderer->parrot_capacity) ||
	    urp_src_reserve(renderer->echo_up, URP_NATIVE_SAMPLES, URP_NATIVE_SAMPLES) ||
	    urp_src_reserve(renderer->down, URP_NATIVE_SAMPLES, URP_NATIVE_SAMPLES) ||
	    txagc_rnnoise_prepare(&renderer->local_rnnoise, URP_RATE_NATIVE)) {
		txagc_rnnoise_destroy(&renderer->local_rnnoise);
		urp_src_destroy(renderer->echo_up);
		urp_src_destroy(renderer->down);
		urp_radio_core_destroy(renderer->radio_core);
		ast_free(renderer->parrot_audio);
		ast_free(renderer);
		return -1;
	}
	native_renderer_publish_statistics(renderer);
	channel->plus_native_renderer = renderer;
	usbradioplus_native_renderer_bind_radio(channel);
	return 0;
}

void usbradioplus_native_renderer_stop(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_renderer *renderer;

	if (!channel)
		return;
	renderer = channel->plus_native_renderer;
	if (!renderer)
		return;
	channel->plus_native_renderer = NULL;
	if (channel->radio) {
		urp_dcs_set_receive_callback(&channel->radio->dcs, NULL, NULL);
		urp_ctcss_set_receive_callback(channel->radio->rxCtcss, NULL, NULL);
	}
	txagc_rnnoise_destroy(&renderer->local_rnnoise);
	urp_src_destroy(renderer->echo_up);
	urp_src_destroy(renderer->down);
	urp_radio_core_destroy(renderer->radio_core);
	ast_free(renderer->parrot_audio);
	ast_free(renderer);
}

int usbradioplus_native_renderer_stats_read(struct chan_usbradio_pvt *channel,
					    struct usbradioplus_native_renderer_stats *statistics)
{
	struct usbradioplus_native_renderer *renderer;
	unsigned int attempt;

	if (!channel || !statistics)
		return -1;
	renderer = channel->plus_native_renderer;
	if (!renderer)
		return -1;
	for (attempt = 0U; attempt < 3U; ++attempt) {
		unsigned int index =
			atomic_load_explicit(&renderer->statistics_index, memory_order_acquire);

		atomic_fetch_add_explicit(&renderer->statistics_readers[index], 1U,
					  memory_order_acquire);
#ifdef URP_PROCESSING_TESTING
		native_renderer_test_invalidate_statistics_read(renderer, index);
#endif
		if (index ==
		    atomic_load_explicit(&renderer->statistics_index, memory_order_acquire)) {
			*statistics = renderer->published_statistics[index].statistics;
			atomic_fetch_sub_explicit(&renderer->statistics_readers[index], 1U,
						  memory_order_release);
			return 0;
		}
		atomic_fetch_sub_explicit(&renderer->statistics_readers[index], 1U,
					  memory_order_release);
	}
	return -1;
}

int usbradioplus_native_renderer_tx_audio_stats_read(
	struct chan_usbradio_pvt *channel, struct rptadv_radio_audio_statistics *statistics)
{
	struct usbradioplus_native_renderer *renderer;
	unsigned int attempt;

	if (!channel || !statistics)
		return -1;
	renderer = channel->plus_native_renderer;
	if (!renderer)
		return -1;
	for (attempt = 0U; attempt < 3U; ++attempt) {
		unsigned int index =
			atomic_load_explicit(&renderer->statistics_index, memory_order_acquire);

		atomic_fetch_add_explicit(&renderer->statistics_readers[index], 1U,
					  memory_order_acquire);
#ifdef URP_PROCESSING_TESTING
		native_renderer_test_invalidate_statistics_read(renderer, index);
#endif
		if (index ==
		    atomic_load_explicit(&renderer->statistics_index, memory_order_acquire)) {
			*statistics = renderer->published_statistics[index].tx_audio_statistics;
			atomic_fetch_sub_explicit(&renderer->statistics_readers[index], 1U,
						  memory_order_release);
			return 0;
		}
		atomic_fetch_sub_explicit(&renderer->statistics_readers[index], 1U,
					  memory_order_release);
	}
	return -1;
}

void usbradioplus_native_renderer_stats_reset(struct chan_usbradio_pvt *channel)
{
	if (channel && channel->plus_native_renderer)
		atomic_fetch_add_explicit(&channel->plus_native_renderer->statistics_reset_request,
					  1U, memory_order_release);
}

void usbradioplus_native_renderer_clear_parrot(struct chan_usbradio_pvt *channel)
{
	if (channel && channel->plus_native_renderer)
		atomic_fetch_add_explicit(&channel->plus_native_renderer->parrot_clear_request, 1U,
					  memory_order_release);
}

void usbradioplus_native_renderer_clear_legacy_echo(struct chan_usbradio_pvt *channel)
{
	if (!channel)
		return;
	if (channel->plus_native_renderer) {
		atomic_fetch_add_explicit(&channel->plus_native_renderer->legacy_echo_clear_request,
					  1U, memory_order_release);
		return;
	}
	urp_sample_queue_discard(&channel->echo_queue);
}

#ifdef URP_PROCESSING_TESTING
/** @brief Exercise nonblocking diagnostics publication while a reader retains its target page.
 * @param channel Channel whose initialized renderer is exercised.
 */
void usbradioplus_native_renderer_test_statistics_publish_busy(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_renderer *renderer = channel->plus_native_renderer;
	unsigned int active =
		atomic_load_explicit(&renderer->statistics_index, memory_order_acquire);
	unsigned int inactive = active ^ 1U;

	atomic_fetch_add_explicit(&renderer->statistics_readers[inactive], 1U,
				  memory_order_acquire);
	native_renderer_publish_statistics(renderer);
	atomic_fetch_sub_explicit(&renderer->statistics_readers[inactive], 1U,
				  memory_order_release);
}

/** @brief Force direct-statistics reads to exhaust their bounded retry count.
 * @param channel Channel whose renderer is exercised.
 * @return The forced statistics-read result.
 */
int usbradioplus_native_renderer_test_statistics_retry(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_renderer *renderer = channel->plus_native_renderer;
	struct usbradioplus_native_renderer_stats statistics;
	unsigned int index;
	int result;

	index = atomic_load_explicit(&renderer->statistics_index, memory_order_acquire);
	atomic_store_explicit(&native_renderer_test_statistics_retry_count, 3U,
			      memory_order_release);
	result = usbradioplus_native_renderer_stats_read(channel, &statistics);
	atomic_store_explicit(&native_renderer_test_statistics_retry_count, 0U,
			      memory_order_release);
	atomic_store_explicit(&renderer->statistics_index, index, memory_order_release);
	return result;
}

/** @brief Force direct transmitter-meter reads to exhaust bounded retries.
 * @param channel Channel whose renderer is exercised.
 * @return The forced transmitter-meter-read result.
 */
int usbradioplus_native_renderer_test_tx_audio_statistics_retry(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_renderer *renderer = channel->plus_native_renderer;
	struct rptadv_radio_audio_statistics statistics;
	unsigned int index;
	int result;

	index = atomic_load_explicit(&renderer->statistics_index, memory_order_acquire);
	atomic_store_explicit(&native_renderer_test_statistics_retry_count, 3U,
			      memory_order_release);
	result = usbradioplus_native_renderer_tx_audio_stats_read(channel, &statistics);
	atomic_store_explicit(&native_renderer_test_statistics_retry_count, 0U,
			      memory_order_release);
	atomic_store_explicit(&renderer->statistics_index, index, memory_order_release);
	return result;
}

/** @brief Exercise the bounded routing snapshot retry without scheduling a thread.
 * @param channel Channel supplying the routing snapshot.
 */
void usbradioplus_native_renderer_test_hardware_snapshot_race(
	const struct chan_usbradio_pvt *channel)
{
	enum radio_tx_mix txmixa;
	enum radio_tx_mix txmixb;

	atomic_store_explicit(&native_renderer_test_hardware_snapshot_delta, 2U,
			      memory_order_release);
	read_hardware_snapshot(channel, &txmixa, &txmixb);
}

#endif

/* Process one physical callback block and write direct app/DAC outputs.
 *
 * The adapter stages the completed DAC block after this call.  Consequently
 * the signaling engine and program ring advance once for every native input
 * span, independent of temporary hardware-output congestion.
 */
size_t usbradioplus_native_tick(struct chan_usbradio_pvt *channel, size_t frame_count)
{
	struct usbradioplus_native_renderer *renderer;
	struct usbradioplus_native_graph_set *graphs;
	struct native_renderer_input input;
	unsigned int app_rate;
	size_t app_frame_count;
	size_t app_capacity;
	size_t app_rx_count;
	size_t maximum_frame_count;
	int app_frame_count_exact;
	short *app_pcm;
	const short *adc_pcm;

	if (!channel || !frame_count)
		return 0U;
	maximum_frame_count = channel->plus_native_max_frames;
	if (!maximum_frame_count)
		maximum_frame_count = URP_NATIVE_MAX_SAMPLES;
	if (frame_count > maximum_frame_count || frame_count > URP_NATIVE_MAX_SAMPLES)
		return 0U;
	app_rate = channel->plus_app_rpt_rate;
	if (!app_rate)
		app_rate = URP_APP_RPT_RATE_DEFAULT;
	if (native_renderer_app_frame_capacity(app_rate, frame_count, &app_capacity)) {
		native_renderer_silence(channel, frame_count, 0U);
		usbradioplus_native_output_stage_publish_ptt(channel);
		return 0U;
	}
	/* DSP initialization publishes the callback-owned atomics and workspaces.
	 * Retain the former pre-start behavior for harmless direct calls during
	 * setup or teardown instead of reading uninitialized hardware snapshots. */
	if (!channel->plus_dsp_initialized) {
		native_renderer_silence(channel, frame_count, app_capacity);
		usbradioplus_native_output_stage_publish_ptt(channel);
		return 0U;
	}
	app_pcm = (short *)(channel->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET);
	adc_pcm = (const short *)(channel->usbradio_read_buf + AST_FRIENDLY_OFFSET);

	/* HID sampling and physical PTT completion are independent workers. Import
	 * their atomically published snapshot before this audio-owned state advance. */
	usbradioplus_audio_load_hardware_state(channel);
	usbradioplus_note_hardware_ptt_applied(channel);
	if (channel->radio) {
		usbradioplus_import_external_ptt_request(channel);
		usbradioplus_prepare_squelch_audio(channel, frame_count);
		usbradioplus_tx_playout_hold_prepare(channel);
		(void)urp_radio_process_native_timed(
			channel->radio, (i16 *)channel->plus_squelch_native, (i16 *)app_pcm,
			(i16 *)channel->usbradio_write_buf, frame_count, 1);
		usbradioplus_tx_playout_hold_apply(channel);
		usbradioplus_tx_playout_hold_publish(channel);
		usbradioplus_refresh_ctcss_decode(channel);
	}
	renderer = channel->plus_native_renderer;
	if (!renderer) {
		native_renderer_silence(channel, frame_count, app_capacity);
		usbradioplus_native_output_stage_publish_ptt(channel);
		return 0U;
	}
	native_renderer_silence(channel, frame_count, app_capacity);
	graphs = usbradioplus_native_graphs_acquire(channel);
	if (!graphs) {
		usbradioplus_native_output_stage_publish_ptt(channel);
		return 0U;
	}
	if (native_renderer_app_frame_capacity(graphs->app_rpt_rate, frame_count, &app_capacity)) {
		native_renderer_silence(channel, frame_count, 0U);
		usbradioplus_native_graphs_release(channel);
		usbradioplus_native_output_stage_publish_ptt(channel);
		return 0U;
	}
	app_frame_count_exact = !native_renderer_app_frame_count(graphs->app_rpt_rate, frame_count,
								 &app_frame_count);
	native_renderer_snapshot(&input, channel, frame_count);
	native_renderer_apply_requests(renderer, graphs);
	if (renderer->app_rpt_rate != graphs->app_rpt_rate) {
		/* Converter history is callback-owned; reload never touches it. */
		urp_src_reset(renderer->echo_up);
		urp_src_reset(renderer->down);
		renderer->app_rpt_rate = graphs->app_rpt_rate;
	}
	app_rx_count = native_renderer_render_receive(renderer, &input, graphs, adc_pcm, app_pcm,
						      frame_count, app_capacity);
	native_renderer_render_transmit(renderer, &input, graphs,
					(short *)channel->usbradio_write_buf, frame_count,
					app_frame_count_exact ? app_frame_count : 0U);
	native_renderer_finish_block(renderer, graphs);
#ifdef URP_PROCESSING_TESTING
	native_renderer_publish_test_observables(renderer, frame_count);
#endif
	usbradioplus_native_graphs_release(channel);
	/* The signaling engine owns logical PTT, while queued historical PCM may
	 * briefly keep physical PTT asserted until the DAC consumes that PCM. */
	usbradioplus_native_output_stage_publish_ptt(channel);
	return app_rx_count;
}

/** @brief Clear one canonical-F32 stereo callback span without allocating.
 * @param output Writable interleaved output span, or NULL.
 * @param frame_count Native frames represented by @p output.
 */
static void native_tick_f32_silence(float *output, size_t frame_count)
{
	if (!output || !frame_count || frame_count > SIZE_MAX / 2U)
		return;
	memset(output, 0, frame_count * 2U * sizeof(*output));
}

/** @brief Map one canonical-F32 ADC sample to the retained signed-16 boundary.
 * @param sample Canonical normalized F32 sample.
 * @return The legacy signed-16 PCM value.
 *
 * This deliberately retains the original direct-PortAudio mapping: NaN and
 * infinities become silence, negative full scale becomes -32768, positive
 * full scale becomes 32767, and finite interior values use the current C
 * floating-point rounding mode through @c lrintf.
 */
static short native_tick_f32_to_s16(float sample)
{
	if (!isfinite(sample))
		return 0;
	if (sample <= -1.0F)
		return INT16_MIN;
	if (sample >= 1.0F)
		return INT16_MAX;
	return (short)lrintf(sample * (float)INT16_MAX);
}

/** @brief Map one retained signed-16 DAC sample to canonical F32.
 * @param sample Legacy signed-16 PCM sample.
 * @return Canonical normalized F32 sample.
 */
static float native_tick_s16_to_f32(short sample)
{
	return (float)sample / 32768.0F;
}

/*
 * @brief Bridge an F32 direct-audio callback into the retained native renderer.
 * @param channel Callback-owned channel state.
 * @param input Interleaved canonical-F32 stereo ADC input, or NULL for silence.
 * @param output Writable interleaved canonical-F32 stereo DAC output.
 * @param frame_count Native frames in both F32 spans.
 * @param logical_ptt Receives the post-render logical PTT state, or NULL.
 * @param output_has_audio Receives whether post-render signed PCM is audible, or NULL.
 * @return App-facing receive samples emitted by @ref usbradioplus_native_tick.
 *
 * The Asterisk compatibility buffers are allocated before this callback starts
 * and remain owned by the channel.  They are the only temporary S16 storage;
 * neither this bridge nor the retained renderer allocates or waits.  The
 * native renderer is intentionally still permitted to use its signed-16
 * compatibility workspaces while ADR-0029 migration is incomplete.
 */
size_t usbradioplus_native_tick_f32(struct chan_usbradio_pvt *channel, const float *input,
				    float *output, size_t frame_count, int *logical_ptt,
				    int *output_has_audio)
{
	short *input_pcm;
	const short *output_pcm;
	size_t maximum_frame_count;
	size_t index;
	size_t app_samples;
	int ptt;
	int has_audio;

	if (logical_ptt)
		*logical_ptt = 0;
	if (output_has_audio)
		*output_has_audio = 0;
	if (!channel || !output || !frame_count) {
		native_tick_f32_silence(output, frame_count);
		return 0U;
	}
	maximum_frame_count = channel->plus_native_max_frames;
	if (!maximum_frame_count)
		maximum_frame_count = URP_NATIVE_MAX_SAMPLES;
	if (frame_count > maximum_frame_count || frame_count > URP_NATIVE_MAX_SAMPLES) {
		native_tick_f32_silence(output, frame_count);
		return 0U;
	}

	/* These channel-owned S16 buffers are preallocated at adapter construction. */
	input_pcm = (short *)(channel->usbradio_read_buf + AST_FRIENDLY_OFFSET);
	for (index = 0U; index < frame_count * 2U; ++index)
		input_pcm[index] = input ? native_tick_f32_to_s16(input[index]) : 0;
	/* Keep raw capture calibration and clip requests on the existing shared
	 * meter before radio processing changes the input span. */
	usbradioplus_measure_rx_audio(channel, input_pcm, frame_count * 2U);

	app_samples = usbradioplus_native_tick(channel, frame_count);
	ptt = atomic_load_explicit(&channel->plus_radio_tx_active, memory_order_acquire);
	output_pcm = (const short *)channel->usbradio_write_buf;
	has_audio = ptt && usbradioplus_pcm_has_audio(output_pcm, frame_count * 2U);
	for (index = 0U; index < frame_count * 2U; ++index)
		output[index] = ptt ? native_tick_s16_to_f32(output_pcm[index]) : 0.0F;
	if (logical_ptt)
		*logical_ptt = ptt;
	if (output_has_audio)
		*output_has_audio = has_audio;
	return app_samples;
}

/** @file
 * @brief Direct native receive processing and transmitter rendering.
 */

#include "asterisk.h"

#include <string.h>

#include "asterisk/frame.h"
#include "asterisk/res_usbradio.h"
#include "asterisk/utils.h"

#include "txagc/avfilter_processor.h"
#include "txagc/rnnoise_processor.h"
#include "usbradioplus_channel_core.h"
#include "usbradioplus_ctcss.h"
#include "usbradioplus_radio.h"
#include "usbradioplus_repeat.h"
#include "usbradioplus_channel_private.h"
#include "usbradioplus_channel_common.h"

/** Interleaved PCM words in one native CM119 hardware frame. */
#define URP_NATIVE_STEREO_SAMPLES (URP_NATIVE_SAMPLES * 2U)

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
	uint8_t carrier_gate[URP_NATIVE_SAMPLES];
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
	/** Asterisk-compatible transmitter audio meter. */
	struct audiostatistics tx_audio_statistics;
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
	/** Transmit DCS state must not share the receive decoder state. */
	struct urp_dcs_state dcs;
	/** Last DCS code applied to the transmit generator. */
	int dcs_code;
	/** Last DCS polarity applied to the transmit generator. */
	int dcs_inverted;
	/** Persistent CTCSS oscillator state. */
	struct urp_ctcss_generator ctcss_generator;
	/** Calibrated one-kilohertz test-tone phase. */
	double test_tone_phase;
	/** Raw native-rate receiver PCM workspace. */
	short rx_native[URP_NATIVE_SAMPLES];
	/** Processed native-rate local-receive workspace. */
	double local_native[URP_NATIVE_SAMPLES];
	/** Native-rate app_rpt program workspace. */
	short link_native[URP_NATIVE_SAMPLES];
	/** Source-rate legacy echo workspace. */
	short link_app[URP_NATIVE_SAMPLES];
	/** Native-rate receiver data before conversion to app_rpt. */
	short receive_app_input[URP_NATIVE_SAMPLES];
	/** Mixed final-transmit PCM workspace. */
	double program[URP_NATIVE_SAMPLES];
	/** Local repeat or native-parrot PCM workspace. */
	double local_program[URP_NATIVE_SAMPLES];
	/** Generated CTCSS PCM workspace. */
	double ctcss[URP_NATIVE_SAMPLES];
	/** Generated DCS PCM workspace. */
	double dcs_program[URP_NATIVE_SAMPLES];
	/** Interleaved DAC PCM used for transmitter measurements. */
	short statistics_stereo[URP_NATIVE_STEREO_SAMPLES];
	/** Receive squelch-delay storage. */
	short rx_delay[RXSQDELAYBUFSIZE * 6];
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
	/** Native-rate parrot recording and playback state. */
	struct urp_parrot_state parrot;
	/** Previous receiver qualification for parrot transitions. */
	int previous_rxkeyed;
	/** Mutable renderer diagnostics, written only by the callback. */
	struct usbradioplus_native_renderer_stats statistics;
	/** Mutable transmitter meter, written only by the callback. */
	struct audiostatistics tx_audio_statistics;
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
#ifdef URP_CHANNEL_MODERN
	ast_radio_check_audio(samples, &renderer->tx_audio_statistics, (short)count, 0);
#else
	ast_radio_check_audio(samples, &renderer->tx_audio_statistics, (short)count);
#endif
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
		renderer->parrot.count = 0;
		renderer->parrot.play = 0;
		renderer->parrot.playing = 0;
		renderer->parrot.truncated = 0;
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
 * @return Nonzero when every output sample came from the program ring.
 *
 * Its persistent converter handles every configured source rate, including a
 * nominal one-to-one native-rate stream, and provides concealment.
 */
static int read_native_program(struct chan_usbradio_pvt *channel,
			       const struct usbradioplus_native_graph_set *graphs, short *output)
{
	struct rpcr_ring *ring = &channel->plus_program_ring;
	size_t index;
	int complete = 1;

	for (index = 0; index < URP_NATIVE_SAMPLES; ++index) {
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
 */
static void process_receive_filter(const struct native_renderer_input *input,
				   struct usbradioplus_native_graph_set *graphs, double *samples)
{
	int decoded = input->decoded_ctcss;

	(void)txagc_avfilter_process_prepared(&graphs->receive_filter, samples, URP_NATIVE_SAMPLES);
	if (decoded > CTCSS_NULL && decoded < CTCSS_NUM_CODES &&
	    graphs->ctcss_notch[decoded].configured)
		(void)txagc_avfilter_process_prepared(&graphs->ctcss_notch[decoded], samples,
						      URP_NATIVE_SAMPLES);
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
 */
static void native_renderer_snapshot(struct native_renderer_input *snapshot,
				     const struct chan_usbradio_pvt *channel)
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
		       sizeof(snapshot->carrier_gate));
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

/** @brief Copy processed local receive to the app_rpt receive frame.
 * @param renderer Renderer that owns the local-receive workspace and converter.
 * @param graphs Active graph generation supplying app_rpt format details.
 * @param app_pcm Receives the app_rpt-format receive frame.
 */
static void native_renderer_copy_receive_to_app(struct usbradioplus_native_renderer *renderer,
						const struct usbradioplus_native_graph_set *graphs,
						short *app_pcm)
{
	size_t index;
	size_t used = 0U;
	size_t made = 0U;

	for (index = 0; index < URP_NATIVE_SAMPLES; ++index) {
		double sample = renderer->local_native[index];

		renderer->receive_app_input[index] =
			urp_apply_gain((short)fmax(-32768.0, fmin(32767.0, sample)), 1.0);
	}
	if (graphs->app_rpt_rate == URP_RATE_NATIVE) {
		memcpy(app_pcm, renderer->receive_app_input, URP_NATIVE_SAMPLES * sizeof(*app_pcm));
		return;
	}
	if (urp_rate_convert_prepared(renderer->down, renderer->receive_app_input,
				      URP_NATIVE_SAMPLES, URP_RATE_NATIVE, app_pcm,
				      graphs->app_rpt_samples, graphs->app_rpt_rate, &used,
				      &made) ||
	    used != URP_NATIVE_SAMPLES) {
		renderer->statistics.src_errors++;
		memset(app_pcm, 0, graphs->app_rpt_samples * sizeof(*app_pcm));
	} else if (made < graphs->app_rpt_samples) {
		memset(app_pcm + made, 0, (graphs->app_rpt_samples - made) * sizeof(*app_pcm));
	}
}

/** @brief Run the local receive branch directly in the hardware callback.
 * @param renderer Renderer that owns receive processing state and workspaces.
 * @param input Per-block signaling snapshot.
 * @param graphs Active native graph generation.
 * @param adc_pcm Native ADC PCM for this block.
 * @param app_pcm Receives the corresponding app_rpt receive frame.
 *
 * RNNoise is intentionally confined here: after deemphasis, squelch gate,
 * input gain, and receive/PL filtering, and before local dynamics. It never
 * processes app_rpt program audio or the transmitter/DAC branch.
 */
static void native_renderer_render_receive(struct usbradioplus_native_renderer *renderer,
					   const struct native_renderer_input *input,
					   struct usbradioplus_native_graph_set *graphs,
					   const short *adc_pcm, short *app_pcm)
{
	struct urp_receive_block_stats stats;
	size_t index;

	urp_prepare_receive_block(adc_pcm, renderer->rx_native, renderer->local_native,
				  URP_NATIVE_SAMPLES, renderer->rx_delay,
				  graphs->receive_squelch_delay_samples, &renderer->rx_delay_index,
				  &stats);
	renderer->statistics.adc_peak_dbfs = urp_pcm_peak_dbfs(stats.peak);
	if (renderer->statistics.adc_peak_dbfs > renderer->statistics.adc_max_peak_dbfs)
		renderer->statistics.adc_max_peak_dbfs = renderer->statistics.adc_peak_dbfs;
	renderer->statistics.adc_rail_samples += stats.rail_samples;
	(void)txagc_avfilter_process_prepared(&graphs->receive_deemphasis, renderer->local_native,
					      URP_NATIVE_SAMPLES);
	if (graphs->noise_squelch_gate) {
		for (index = 0; index < URP_NATIVE_SAMPLES; ++index)
			if (!input->carrier_gate[index])
				renderer->local_native[index] = 0.0;
	}
	for (index = 0; index < URP_NATIVE_SAMPLES; ++index)
		renderer->local_native[index] *= graphs->local_input_gain_linear;
	process_receive_filter(input, graphs, renderer->local_native);
	if (graphs->local_chain_enabled && (!graphs->receive_cpu_saver || input->rxkeyed)) {
		if (graphs->local_rnnoise_enabled)
			(void)txagc_rnnoise_process_prepared(&renderer->local_rnnoise,
							     renderer->local_native,
							     URP_NATIVE_SAMPLES);
		else
			txagc_rnnoise_bypass(&renderer->local_rnnoise);
		(void)txagc_avfilter_process_prepared(&graphs->local_dynamics,
						      renderer->local_native, URP_NATIVE_SAMPLES);
	} else {
		txagc_rnnoise_bypass(&renderer->local_rnnoise);
	}
	renderer->statistics.rnnoise_frames = renderer->local_rnnoise.rnnoise_frames;
	renderer->statistics.rnnoise_output_samples = renderer->local_rnnoise.output_samples;
	renderer->statistics.rnnoise_startup_samples = renderer->local_rnnoise.startup_samples;
	renderer->statistics.rnnoise_errors = renderer->local_rnnoise.errors;
	renderer->statistics.rnnoise_vad_probability = renderer->local_rnnoise.vad_probability;
	if (graphs->legacy_interface && graphs->echo_mode &&
	    urp_parrot_rx_transition(&renderer->parrot, renderer->previous_rxkeyed, input->rxkeyed))
		atomic_store_explicit(&renderer->channel->echoing, 1, memory_order_release);
	renderer->previous_rxkeyed = input->rxkeyed;
	native_renderer_copy_receive_to_app(renderer, graphs, app_pcm);
}

/** @brief Generate direct CTCSS and DCS sources for one DAC frame.
 * @param renderer Renderer that owns the signaling generators.
 * @param input Per-block transmit signaling snapshot.
 * @param graphs Active native graph generation supplying DCS filters.
 */
static void native_renderer_generate_signaling(struct usbradioplus_native_renderer *renderer,
					       const struct native_renderer_input *input,
					       struct usbradioplus_native_graph_set *graphs)
{
	int dcs_normal_active;

	if (input->tx_ctcss_tail_tone_hz > 0.0)
		urp_ctcss_generate_tail_tone(&renderer->ctcss_generator, renderer->ctcss,
					     URP_NATIVE_SAMPLES, input->tx_ctcss_tail_tone_hz, 1.0,
					     input->tx_ctcss_enabled && !input->tx_ctcss_off);
	else
		urp_ctcss_generate(&renderer->ctcss_generator, renderer->ctcss, URP_NATIVE_SAMPLES,
				   input->tx_ctcss_frequency_hz, 1.0,
				   input->tx_ctcss_enabled && !input->tx_ctcss_off,
				   input->tx_ctcss_phase_shift);
	if (renderer->dcs_code != input->dcs_transmit_code ||
	    renderer->dcs_inverted != input->dcs_transmit_inverted) {
		urp_dcs_configure(&renderer->dcs, -1, 0, input->dcs_transmit_code,
				  input->dcs_transmit_inverted);
		renderer->dcs_code = input->dcs_transmit_code;
		renderer->dcs_inverted = input->dcs_transmit_inverted;
	}
	dcs_normal_active =
		input->dcs_enabled && input->tx_ptt_out && input->tx_state == CHAN_TXSTATE_ACTIVE;
	urp_dcs_generate(&renderer->dcs, renderer->dcs_program, URP_NATIVE_SAMPLES, URP_RATE_NATIVE,
			 input->dcs_peak, dcs_normal_active || input->dcs_turnoff_active,
			 input->dcs_turnoff_active);
	if (txagc_avfilter_process_prepared(input->dcs_turnoff_active ? &graphs->dcs_turnoff
								      : &graphs->dcs,
					    renderer->dcs_program, URP_NATIVE_SAMPLES) < 0)
		memset(renderer->dcs_program, 0, sizeof(renderer->dcs_program));
	if (!dcs_normal_active && !input->dcs_turnoff_active)
		memset(renderer->dcs_program, 0, sizeof(renderer->dcs_program));
}

/** @brief Render the transmitter branch directly into the hardware DAC buffer.
 * @param renderer Renderer that owns transmit workspaces and generators.
 * @param input Per-block signaling snapshot.
 * @param graphs Active native graph generation.
 * @param stereo Receives interleaved native DAC PCM.
 *
 * This branch contains app_rpt program audio, local repeat/parrot audio,
 * final transmit filtering, test tone, CTCSS/DCS generation, and routing. It
 * deliberately contains no RNNoise processing.
 */
static void native_renderer_render_transmit(struct usbradioplus_native_renderer *renderer,
					    const struct native_renderer_input *input,
					    struct usbradioplus_native_graph_set *graphs,
					    short *stereo)
{
	struct chan_usbradio_pvt *channel = renderer->channel;
	double ctcss_peak_a = input->tx_ctcss_peak;
	double ctcss_peak_b = ctcss_peak_a;
	enum radio_tx_mix txmixa;
	enum radio_tx_mix txmixb;
	size_t index;
	size_t used = 0U;
	size_t made = 0U;

	memset(stereo, 0, URP_NATIVE_STEREO_SAMPLES * sizeof(*stereo));
	native_renderer_generate_signaling(renderer, input, graphs);
	memset(renderer->link_native, 0, sizeof(renderer->link_native));
	if (graphs->legacy_interface &&
	    atomic_load_explicit(&channel->echoing, memory_order_acquire)) {
		int have_frame = 0;

		if (urp_sample_queue_samples(&channel->echo_queue) >= graphs->app_rpt_samples) {
			for (index = 0; index < graphs->app_rpt_samples; ++index)
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
			       sizeof(renderer->link_native));
		} else if (urp_rate_convert_prepared(renderer->echo_up, renderer->link_app,
						     graphs->app_rpt_samples, graphs->app_rpt_rate,
						     renderer->link_native, URP_NATIVE_SAMPLES,
						     URP_RATE_NATIVE, &used, &made) ||
			   used != graphs->app_rpt_samples) {
			renderer->statistics.src_errors++;
			memset(renderer->link_native, 0, sizeof(renderer->link_native));
		} else if (made < URP_NATIVE_SAMPLES) {
			memset(renderer->link_native + made, 0,
			       (URP_NATIVE_SAMPLES - made) * sizeof(*renderer->link_native));
		}
	} else if (!read_native_program(channel, graphs, renderer->link_native) &&
		   atomic_load_explicit(&channel->txkeyed, memory_order_acquire)) {
		renderer->statistics.link_queue_underflows++;
	}
	for (index = 0; index < URP_NATIVE_SAMPLES; ++index)
		renderer->program[index] = renderer->link_native[index];
	memset(renderer->local_program, 0, sizeof(renderer->local_program));
	if (graphs->legacy_interface && renderer->parrot.playing) {
		urp_parrot_play(&renderer->parrot, renderer->local_program, URP_NATIVE_SAMPLES);
		renderer->statistics.parrot_playback_frames++;
		if (!renderer->parrot.playing)
			atomic_store_explicit(&channel->echoing, 0, memory_order_release);
	} else if (graphs->legacy_interface && input->rxkeyed && graphs->software_repeat_enabled) {
		urp_native_repeat_prepare(renderer->local_program, renderer->local_native,
					  URP_NATIVE_SAMPLES, 1.0,
					  input->usedtmf && input->has_dsp && input->toneflag);
		if (graphs->echo_mode)
			urp_parrot_record(&renderer->parrot, renderer->local_program,
					  URP_NATIVE_SAMPLES, renderer->parrot.capacity);
		for (index = 0; index < URP_NATIVE_SAMPLES; ++index)
			renderer->local_program[index] *= graphs->software_repeat_gain;
	}
	{
		double peak = urp_double_peak(renderer->local_program, URP_NATIVE_SAMPLES);

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
	for (index = 0; index < URP_NATIVE_SAMPLES; ++index)
		renderer->program[index] += renderer->local_program[index];
	if (txagc_avfilter_process_prepared(&graphs->final, renderer->program, URP_NATIVE_SAMPLES) <
	    0)
		memset(renderer->program, 0, sizeof(renderer->program));
	if (input->test_tone_enabled) {
		const double step = 2.0 * M_PI * 1000.0 / URP_RATE_NATIVE;

		for (index = 0; index < URP_NATIVE_SAMPLES; ++index) {
			renderer->program[index] =
				URP_LEGACY_TEST_TONE_PEAK * sin(renderer->test_tone_phase);
			renderer->test_tone_phase += step;
			if (renderer->test_tone_phase >= 2.0 * M_PI)
				renderer->test_tone_phase -= 2.0 * M_PI;
		}
	} else {
		renderer->test_tone_phase = 0.0;
	}
	{
		double peak = urp_double_peak(renderer->program, URP_NATIVE_SAMPLES);

		renderer->statistics.tx_program_peak_dbfs =
			peak > 0.0 ? 20.0 * log10(peak / 32768.0) : -INFINITY;
		if (renderer->statistics.tx_program_peak_dbfs >
		    renderer->statistics.tx_program_max_peak_dbfs)
			renderer->statistics.tx_program_max_peak_dbfs =
				renderer->statistics.tx_program_peak_dbfs;
	}
	read_hardware_snapshot(channel, &txmixa, &txmixb);
	renderer->statistics.tx_program_rail_samples += urp_render_transmit_block(
		renderer->program, renderer->ctcss, renderer->dcs_program, URP_NATIVE_SAMPLES,
		(enum urp_tx_output_mode)txmixa, (enum urp_tx_output_mode)txmixb, ctcss_peak_a, 0.0,
		ctcss_peak_b, 0.0, stereo, renderer->statistics_stereo);
	native_renderer_check_tx_audio(renderer, renderer->statistics_stereo,
				       URP_NATIVE_STEREO_SAMPLES);
}

/** @brief Publish measurements and test observables after one direct block.
 * @param renderer Renderer whose diagnostics are published.
 * @param graphs Active native graph generation supplying filter measurements.
 */
static void native_renderer_finish_block(struct usbradioplus_native_renderer *renderer,
					 struct usbradioplus_native_graph_set *graphs)
{
	renderer->statistics.native_frames++;
	renderer->statistics.parrot_samples = renderer->parrot.count;
	renderer->statistics.parrot_playing = renderer->parrot.playing;
	renderer->statistics.parrot_truncated = renderer->parrot.truncated;
	native_renderer_copy_filter_statistics(&renderer->statistics.local_filter,
					       &graphs->local_dynamics);
	native_renderer_copy_filter_statistics(&renderer->statistics.receive_deemphasis_filter,
					       &graphs->receive_deemphasis);
	native_renderer_copy_filter_statistics(&renderer->statistics.final_filter, &graphs->final);
#ifdef URP_PROCESSING_TESTING
	struct chan_usbradio_pvt *channel = renderer->channel;

	memcpy(channel->plus_rx_native, renderer->rx_native, sizeof(renderer->rx_native));
	memcpy(channel->plus_local_native, renderer->local_native, sizeof(renderer->local_native));
	memcpy(channel->plus_link_native, renderer->link_native, sizeof(renderer->link_native));
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
	channel->txaudiostats = renderer->tx_audio_statistics;
	channel->plus_parrot_playing = renderer->parrot.playing;
	channel->plus_parrot_count = renderer->parrot.count;
	channel->plus_parrot_play = renderer->parrot.play;
	channel->plus_parrot_truncated = renderer->parrot.truncated;
	channel->plus_ctcss_generator = renderer->ctcss_generator;
	channel->plus_test_tone_phase = renderer->test_tone_phase;
#endif
	native_renderer_publish_statistics(renderer);
}

/** @brief Fill direct app and DAC destinations with one silent block.
 * @param channel Channel whose frame buffers receive silence.
 */
static void native_renderer_silence(struct chan_usbradio_pvt *channel)
{
	memset(channel->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET, 0,
	       channel->plus_app_rpt_samples * sizeof(short));
	memset(channel->usbradio_write_buf, 0, URP_NATIVE_STEREO_SAMPLES * sizeof(short));
}

int usbradioplus_native_renderer_start(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_renderer *renderer;

	if (!channel || channel->plus_native_renderer)
		return channel ? 0 : -1;
	renderer = ast_calloc(1, sizeof(*renderer));
	if (!renderer)
		return -1;
	renderer->channel = channel;
	native_renderer_statistics_init(renderer);
	atomic_init(&renderer->statistics_index, 0U);
	atomic_init(&renderer->statistics_readers[0], 0U);
	atomic_init(&renderer->statistics_readers[1], 0U);
	atomic_init(&renderer->statistics_reset_request, 0U);
	atomic_init(&renderer->parrot_clear_request, 0U);
	atomic_init(&renderer->legacy_echo_clear_request, 0U);
	urp_dcs_init(&renderer->dcs);
	renderer->dcs_code = -2;
	renderer->dcs_inverted = -1;
	renderer->echo_up = urp_src_create(SRC_SINC_BEST_QUALITY, 1);
	renderer->down = urp_src_create(SRC_SINC_BEST_QUALITY, 1);
	renderer->parrot.capacity = (size_t)DEFAULT_ECHO_MAX * URP_NATIVE_SAMPLES;
	renderer->parrot.audio =
		ast_calloc(renderer->parrot.capacity, sizeof(*renderer->parrot.audio));
	txagc_rnnoise_init(&renderer->local_rnnoise);
	if (!renderer->echo_up || !renderer->down || !renderer->parrot.audio ||
	    urp_src_reserve(renderer->echo_up, URP_NATIVE_SAMPLES, URP_NATIVE_SAMPLES) ||
	    urp_src_reserve(renderer->down, URP_NATIVE_SAMPLES, URP_NATIVE_SAMPLES) ||
	    txagc_rnnoise_prepare(&renderer->local_rnnoise, URP_RATE_NATIVE)) {
		txagc_rnnoise_destroy(&renderer->local_rnnoise);
		urp_src_destroy(renderer->echo_up);
		urp_src_destroy(renderer->down);
		ast_free(renderer->parrot.audio);
		ast_free(renderer);
		return -1;
	}
	native_renderer_publish_statistics(renderer);
	channel->plus_native_renderer = renderer;
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
	txagc_rnnoise_destroy(&renderer->local_rnnoise);
	urp_src_destroy(renderer->echo_up);
	urp_src_destroy(renderer->down);
	ast_free(renderer->parrot.audio);
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

int usbradioplus_native_renderer_tx_audio_stats_read(struct chan_usbradio_pvt *channel,
						     struct audiostatistics *statistics)
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
	struct audiostatistics statistics;
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
 * Receive processing always runs so app_rpt sees every ADC frame. Program PCM
 * and TX-only state advance only when the DAC will accept the matching frame;
 * a full device therefore cannot consume the program ring and make a dropout.
 */
void usbradioplus_native_tick(struct chan_usbradio_pvt *channel, int transmit_ready)
{
	struct usbradioplus_native_renderer *renderer;
	struct usbradioplus_native_graph_set *graphs;
	struct native_renderer_input input;
	short *app_pcm;
	const short *adc_pcm;

	if (!channel)
		return;
	renderer = channel->plus_native_renderer;
	if (!renderer) {
		native_renderer_silence(channel);
		usbradioplus_publish_hardware_ptt(channel,
						  channel->radio ? channel->radio->txPttOut : 0);
		return;
	}
	native_renderer_silence(channel);
	graphs = usbradioplus_native_graphs_acquire(channel);
	if (!graphs) {
		usbradioplus_publish_hardware_ptt(channel,
						  channel->radio ? channel->radio->txPttOut : 0);
		return;
	}
	native_renderer_snapshot(&input, channel);
	native_renderer_apply_requests(renderer, graphs);
	if (renderer->app_rpt_rate != graphs->app_rpt_rate) {
		/* Converter history is callback-owned; reload never touches it. */
		urp_src_reset(renderer->echo_up);
		urp_src_reset(renderer->down);
		renderer->app_rpt_rate = graphs->app_rpt_rate;
	}
	adc_pcm = (const short *)(channel->usbradio_read_buf + AST_FRIENDLY_OFFSET);
	app_pcm = (short *)(channel->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET);
	native_renderer_render_receive(renderer, &input, graphs, adc_pcm, app_pcm);
	if (transmit_ready)
		native_renderer_render_transmit(renderer, &input, graphs,
						(short *)channel->usbradio_write_buf);
	native_renderer_finish_block(renderer, graphs);
	usbradioplus_native_graphs_release(channel);
	/* PTT remains a signaling-engine decision, independent of audio availability. */
	usbradioplus_publish_hardware_ptt(channel, channel->radio ? channel->radio->txPttOut : 0);
}

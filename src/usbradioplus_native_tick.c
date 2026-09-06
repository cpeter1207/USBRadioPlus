/** @file
 * @brief Native receive gating, processing, repeat, echo, and transmitter rendering.
 */

#include "asterisk.h"

#include <math.h>
#include <search.h>
#include <string.h>

#include "asterisk/logger.h"
#include "asterisk/lock.h"
#include "asterisk/frame.h"
#include "asterisk/res_usbradio.h"
#include "asterisk/utils.h"

#include "txagc/avfilter_processor.h"
#include "txagc/rnnoise_processor.h"
#include "usbradioplus_channel_core.h"
#include "usbradioplus_ctcss.h"
#include "usbradioplus_processing.h"
#include "usbradioplus_radio.h"
#include "usbradioplus_repeat.h"
#include "usbradioplus_channel_private.h"

/* Shared native-rate channel engine instantiated by each hardware adapter. */
void usbradioplus_native_tick(struct chan_usbradio_pvt *o)
{
	struct txagc_chain chain;
	double program[URP_NATIVE_SAMPLES];
	double local_program[URP_NATIVE_SAMPLES];
	double ctcss[URP_NATIVE_SAMPLES];
	short network_program[URP_NATIVE_SAMPLES];
	short *stereo = (short *)o->usbradio_write_buf;
	size_t used = 0, made = 0, i;
	int local_chain_enabled;
	int ctcss_phase_reverse;
	double ctcss_frequency, ctcss_peak_a, ctcss_peak_b, correction;
	double ctcss_bias_a, ctcss_bias_b;
	int ctcss_filter_250, ctcss_tone_gain;
	enum radio_tx_mix txmixa = effective_txmixa(o);
	enum radio_tx_mix txmixb = effective_txmixb(o);

	refresh_processing_hardware(o);
	/* A non-null destination is infallible; only the configured state controls
	 * whether the optional local chain runs. */
	usbradioplus_processing_get_local(o->name, &chain);
	local_chain_enabled = chain.enabled;
	ctcss_phase_reverse = o->radio->txCtcssPhaseShift;
	ctcss_frequency = o->radio->txCtcssFreq10 / 10.0;
	ctcss_filter_250 = o->radio->txCtcssFilter250;
	ctcss_tone_gain = o->radio->txCtcssGainQ8;
	urp_ctcss_legacy_scaled_levels(ctcss_frequency, ctcss_filter_250, ctcss_tone_gain,
				       o->radio->txOutputGainA, &ctcss_peak_a, &ctcss_bias_a);
	urp_ctcss_legacy_scaled_levels(ctcss_frequency, ctcss_filter_250, ctcss_tone_gain,
				       o->radio->txOutputGainB, &ctcss_peak_b, &ctcss_bias_b);
	urp_ctcss_generate(&o->plus_ctcss_generator, ctcss, URP_NATIVE_SAMPLES, ctcss_frequency,
			   1.0, o->radio->txCtcssEnabled && !o->radio->b.txCtcssOff,
			   ctcss_phase_reverse);

	{
		struct urp_receive_block_stats stats;
		size_t delay_samples =
			o->rxsquelchdelay ? o->rxsquelchdelay * (URP_RATE_NATIVE / 1000) : 0;
		urp_prepare_receive_block((short *)(o->usbradio_read_buf + AST_FRIENDLY_OFFSET),
					  o->plus_rx_native, o->plus_local_native,
					  URP_NATIVE_SAMPLES, o->plus_rx_delay, delay_samples,
					  &o->plus_rx_delay_index, &stats);
		o->plus_adc_peak_dbfs = urp_pcm_peak_dbfs(stats.peak);
		if (o->plus_adc_peak_dbfs > o->plus_adc_max_peak_dbfs)
			o->plus_adc_max_peak_dbfs = o->plus_adc_peak_dbfs;
		o->plus_adc_rail_samples += stats.rail_samples;
	}
	{
		struct txagc_config receive_cfg;
		/* Keep de-emphasis separate so RNNoise can run immediately after the
		 * receiver gate and before any optional dynamics. */
		memset(&receive_cfg, 0, sizeof(receive_cfg));
		receive_cfg.deemphasis_enabled = o->rxdemod == RX_AUDIO_FLAT;
		receive_cfg.emphasis_corner_hz = o->plus_emphasis_corner_hz;
		receive_cfg.emphasis_reference_hz = 1000.0;
		receive_cfg.stage_count = 0;
		if (txagc_avfilter_process(&o->plus_rx_filter, &receive_cfg, o->plus_local_native,
					   URP_NATIVE_SAMPLES, URP_RATE_NATIVE) < 0)
			ast_log(LOG_WARNING, "RadioPlus/%s: receive de-emphasis failed\n", o->name);
	}
	if (o->radio->rxCdType == CD_XPMR_NOISE) {
		/* Preserve continuous de-emphasis state, then gate at the detector's
		 * exact sample. app_rpt carrier notifications remain frame-cadenced;
		 * their previous-frame state must not admit a native squelch tail. */
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			if (!o->radio->rxCarrierGate[i])
				o->plus_local_native[i] = 0.0;
		}
	}
	{
		double gain_db = chain.agc.input_gain_db;
		double gain = pow(10.0, gain_db / 20.0);
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i)
			o->plus_local_native[i] *= gain;
	}
	{
		struct txagc_config filter_cfg;
		memset(&filter_cfg, 0, sizeof(filter_cfg));
		filter_cfg.receive_bandpass_enabled = chain.agc.receive_bandpass_enabled;
		filter_cfg.receive_bandpass_highpass_hz = chain.agc.receive_bandpass_highpass_hz;
		filter_cfg.receive_bandpass_lowpass_hz = chain.agc.receive_bandpass_lowpass_hz;
		filter_cfg.ctcss_filter_mode = chain.agc.ctcss_filter_mode;
		filter_cfg.ctcss_highpass_hz = chain.agc.ctcss_highpass_hz;
		filter_cfg.ctcss_notch_width_hz = chain.agc.ctcss_notch_width_hz;
		if (filter_cfg.ctcss_filter_mode == TXAGC_CTCSS_FILTER_NOTCH)
			ast_copy_string(filter_cfg.ctcss_notch_frequencies, o->rxctcssfreq,
					sizeof(filter_cfg.ctcss_notch_frequencies));
		if (txagc_avfilter_process(&o->plus_rx_filter_after, &filter_cfg,
					   o->plus_local_native, URP_NATIVE_SAMPLES,
					   URP_RATE_NATIVE) < 0)
			ast_log(LOG_WARNING, "RadioPlus/%s: fixed receive filter failed\n",
				o->name);
	}
	if (local_chain_enabled && o->rxkeyed) {
		struct txagc_config dynamics_cfg = chain.agc;
		dynamics_cfg.input_gain_db = 0.0;

		/* De-emphasis, squelch qualification, and fixed receive filtering have
		 * already run. RNNoise is therefore the first optional dynamics stage. */
		if (chain.rnnoise_enabled &&
		    txagc_rnnoise_process_double(&o->plus_local_rnnoise, o->plus_local_native,
						 URP_NATIVE_SAMPLES, URP_RATE_NATIVE)) {
			ast_log(LOG_WARNING, "RadioPlus/%s: local RNNoise processing failed\n",
				o->name);
		}
		if (!chain.rnnoise_enabled)
			txagc_rnnoise_bypass(&o->plus_local_rnnoise);
		dynamics_cfg.deemphasis_enabled = 0;
		/* Fixed receive filtering has already run; do not duplicate it here. */
		dynamics_cfg.ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;
		dynamics_cfg.receive_bandpass_enabled = 0;
		if (txagc_avfilter_process(&o->plus_local_avfilter, &dynamics_cfg,
					   o->plus_local_native, URP_NATIVE_SAMPLES,
					   URP_RATE_NATIVE) < 0)
			ast_log(LOG_WARNING, "RadioPlus/%s: local dynamics processing failed\n",
				o->name);
	} else {
		txagc_rnnoise_bypass(&o->plus_local_rnnoise);
	}
	/* Convert the PL-filtered local signal to the app_rpt rate exactly once. */
	memcpy(program, o->plus_local_native, sizeof(program));
	for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
		network_program[i] =
			urp_apply_gain((short)fmax(-32768.0, fmin(32767.0, program[i])), 1.0);
	}
	if (urp_rate_convert(o->plus_down, network_program, URP_NATIVE_SAMPLES, URP_RATE_NATIVE,
			     (short *)(o->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET),
			     o->plus_app_rpt_samples, o->plus_app_rpt_rate, &used, &made)) {
		o->plus_src_errors++;
	}

	memset(o->plus_link_native, 0, sizeof(o->plus_link_native));
	if (!o->plus_native_fifo.target_samples)
		o->plus_native_fifo.target_samples = URP_FIFO_TARGET_NORMAL;
	if (o->txkeyed && !o->plus_native_fifo.was_keyed) {
		urp_native_fifo_key_start(&o->plus_native_fifo);
		o->plus_link_src_pending = 0;
		urp_clock_recovery_reset(&o->plus_link_clock);
	} else if (!o->txkeyed) {
		o->plus_native_fifo.was_keyed = 0;
	}
	/* app_rpt and the CM119 use independent clocks. Convert queued frames into
	 * an elastic native-rate FIFO and trim the ratio gently around its target. */
	{
		unsigned int queued_frames;
		ast_mutex_lock(&o->plus_link_lock);
		queued_frames = o->plus_program_queue.count;
		ast_mutex_unlock(&o->plus_link_lock);
		correction = urp_clock_recovery_update(
			&o->plus_link_clock,
			o->plus_native_fifo.count + queued_frames * URP_NATIVE_SAMPLES,
			o->plus_native_fifo.target_samples + URP_FIFO_TARGET_STEP);
	}
	while (o->plus_native_fifo.count < o->plus_native_fifo.target_samples) {
		double ratio;
		int have_frame = 0;
		memset(o->plus_link_8k, 0, sizeof(o->plus_link_8k));
		ast_mutex_lock(&o->plus_link_lock);
		have_frame = urp_program_queue_pop(&o->plus_program_queue, o->plus_link_8k);
		ast_mutex_unlock(&o->plus_link_lock);
		if (!have_frame) {
			if (!o->plus_link_src_pending ||
			    o->plus_native_fifo.count >= URP_NATIVE_SAMPLES)
				break;
			/* Release retained sinc samples only when output would starve.
			 * Padding earlier would insert silence between arriving frames;
			 * repeating it would hide a genuine underrun indefinitely. */
			o->plus_link_src_pending = 0;
		}
		if (o->plus_app_rpt_rate == URP_RATE_NATIVE) {
			plus_link_native_push(o, o->plus_link_8k, o->plus_app_rpt_samples);
			continue;
		}
		ratio = (double)URP_RATE_NATIVE / o->plus_app_rpt_rate * (1.0 + correction);
		used = made = 0;
		if (urp_src_process(o->plus_up, o->plus_link_8k, o->plus_app_rpt_samples,
				    o->plus_link_resampled,
				    sizeof(o->plus_link_resampled) /
					    sizeof(o->plus_link_resampled[0]),
				    ratio, &used, &made) ||
		    used != o->plus_app_rpt_samples) {
			o->plus_src_errors++;
			break;
		}
		plus_link_native_push(o, o->plus_link_resampled, made);
		if (have_frame)
			o->plus_link_src_pending = 1;
	}
	if (!o->plus_native_fifo.primed &&
	    o->plus_native_fifo.count >= o->plus_native_fifo.target_samples) {
		o->plus_native_fifo.primed = 1;
	}
	if (o->plus_native_fifo.primed &&
	    !urp_native_fifo_render(&o->plus_native_fifo, o->plus_link_native)) {
		o->plus_native_fifo.primed = 0;
		/* A short SRC remainder is expected while an unkeyed burst drains. */
		if (o->txkeyed) {
			o->plus_link_queue_underflows++;
			urp_native_fifo_note_underrun(&o->plus_native_fifo);
		}
		urp_src_reset(o->plus_up);
		o->plus_link_src_pending = 0;
		urp_clock_recovery_reset(&o->plus_link_clock);
		urp_native_fifo_reset(&o->plus_native_fifo);
	} else if (o->plus_native_fifo.primed && o->txkeyed) {
		urp_native_fifo_note_stable(&o->plus_native_fifo);
	}
	for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
		program[i] = o->plus_link_native[i];
	}
	memset(local_program, 0, sizeof(local_program));

	if (o->plus_parrot_playing) {
		urp_parrot_play(&o->plus_parrot_state, local_program, URP_NATIVE_SAMPLES);
		o->plus_parrot_playback_frames++;
		if (!o->plus_parrot_playing) {
			o->echoing = 0;
		}
	} else if (o->rxkeyed && o->duplex3 > 0 && o->duplex3mode == DUPLEX3_MODE_SOFTWARE) {
		double duplex3_gain = (double)o->duplex3 / DUPLEX3_LEVEL_MAX;
		urp_native_repeat_prepare(local_program, o->plus_local_native, URP_NATIVE_SAMPLES,
					  1.0, o->usedtmf && o->dsp && o->toneflag);
		if (o->echomode && o->plus_parrot) {
			size_t record_capacity = (size_t)DEFAULT_ECHO_MAX * URP_NATIVE_SAMPLES;
			urp_parrot_record(&o->plus_parrot_state, local_program, URP_NATIVE_SAMPLES,
					  record_capacity);
		}
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			local_program[i] *= duplex3_gain;
		}
	}
	{
		double peak = urp_double_peak(local_program, URP_NATIVE_SAMPLES);
		o->plus_preemphasis_input_peak_dbfs =
			peak > 0.0 ? 20.0 * log10(peak / 32768.0) : -INFINITY;
		if (o->plus_preemphasis_input_peak_dbfs > o->plus_preemphasis_input_max_peak_dbfs)
			o->plus_preemphasis_input_max_peak_dbfs =
				o->plus_preemphasis_input_peak_dbfs;
	}
	/* Keep unlimited floating-point headroom through preemphasis, mixing, and
	 * final brick-wall band-pass. Low-frequency energy that will be removed
	 * must never hit a ceiling first and create broadband clipping products. */
	{
		double peak = urp_double_peak(local_program, URP_NATIVE_SAMPLES);
		o->plus_local_tx_peak_dbfs = peak > 0.0 ? 20.0 * log10(peak / 32768.0) : -INFINITY;
		if (o->plus_local_tx_peak_dbfs > o->plus_local_tx_max_peak_dbfs) {
			o->plus_local_tx_max_peak_dbfs = o->plus_local_tx_peak_dbfs;
		}
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			program[i] += local_program[i];
		}
	}
	{
		struct txagc_config final_cfg;
		struct txagc_chain composite_chain;

		usbradioplus_processing_get_composite(o->name, &composite_chain);
		final_cfg = composite_chain.agc;
		/* The source master gates reorderable dynamics. Fixed transmitter-tail
		 * stages retain their own explicit enable controls. */
		if (!composite_chain.enabled) {
			final_cfg.agc_enabled = 0;
			final_cfg.expander_enabled = 0;
			final_cfg.compressor_enabled = 0;
			final_cfg.limiter_enabled = 0;
		}
		/* Pre-emphasis belongs to the native composite graph regardless of which
		 * source supplied the audio. */
		final_cfg.preemphasis_enabled = o->txprelim;
		final_cfg.emphasis_corner_hz = o->plus_emphasis_corner_hz;
		final_cfg.emphasis_reference_hz = 1000.0;
		if (final_cfg.output_highpass_hz > 0.0 && final_cfg.output_lowpass_hz > 0.0 &&
		    final_cfg.output_highpass_hz >= final_cfg.output_lowpass_hz) {
			ast_log(LOG_ERROR,
				"RadioPlus/%s: txhpf cutoff must be below txlpf cutoff\n", o->name);
			memset(program, 0, sizeof(program));
			return;
		}
		if (txagc_avfilter_process(&o->plus_final_avfilter, &final_cfg, program,
					   URP_NATIVE_SAMPLES, URP_RATE_NATIVE) < 0) {
			ast_log(LOG_ERROR, "RadioPlus/%s: final FFmpeg processing failed\n",
				o->name);
			memset(program, 0, sizeof(program));
		}
	}
	/* Match the established deviation reference: bypass voice dynamics and emphasis,
	 * but retain configured output routing and safe PCM conversion. */
	if (o->plus_test_tone_enabled) {
		const double step = 2.0 * M_PI * 1000.0 / URP_RATE_NATIVE;
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			/* The legacy 59/256 generator gain followed by its 8-to-48 kHz FIR
			 * produces exactly +/-7518 PCM codes at steady-state. */
			program[i] = URP_LEGACY_TEST_TONE_PEAK * sin(o->plus_test_tone_phase);
			o->plus_test_tone_phase += step;
			if (o->plus_test_tone_phase >= 2.0 * M_PI)
				o->plus_test_tone_phase -= 2.0 * M_PI;
		}
	} else {
		o->plus_test_tone_phase = 0.0;
	}
	{
		double peak = urp_double_peak(program, URP_NATIVE_SAMPLES);
		o->plus_tx_program_peak_dbfs =
			peak > 0.0 ? 20.0 * log10(peak / 32768.0) : -INFINITY;
		if (o->plus_tx_program_peak_dbfs > o->plus_tx_program_max_peak_dbfs) {
			o->plus_tx_program_max_peak_dbfs = o->plus_tx_program_peak_dbfs;
		}
	}
	{
		short stats_stereo[URP_NATIVE_SAMPLES * 2];
		o->plus_tx_program_rail_samples += urp_render_transmit_block(
			program, ctcss, URP_NATIVE_SAMPLES, (enum urp_tx_output_mode)txmixa,
			(enum urp_tx_output_mode)txmixb, ctcss_peak_a, ctcss_bias_a, ctcss_peak_b,
			ctcss_bias_b, stereo, stats_stereo);
		/* Meter program audio before CM119 mixer gain, regardless of whether
		 * the configured voice/composite output is channel A or channel B. */
		usbradioplus_check_tx_audio(o, stats_stereo, URP_NATIVE_SAMPLES * 2);
	}
	o->plus_native_frames++;
}

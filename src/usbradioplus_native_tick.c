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
	double ctcss_frequency, ctcss_peak_a, ctcss_peak_b;
	double ctcss_bias_a, ctcss_bias_b;
	int ctcss_filter_250, ctcss_tone_gain;
	enum radio_tx_mix txmixa = effective_txmixa(o);
	enum radio_tx_mix txmixb = effective_txmixb(o);

	refresh_processing_hardware(o);
	/* A non-null destination is infallible; only the configured state controls
	 * whether the optional local chain runs. */
	usbradioplus_processing_get_local(&chain);
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
		double rx_high = o->plus_rxhpf_exact
					 ? o->plus_rxhpf_hz
					 : usbradioplus_legacy_cutoff("rxhpf", o->rxhpf);
		double rx_low = o->plus_rxlpf_exact ? o->plus_rxlpf_hz
						    : usbradioplus_legacy_cutoff("rxlpf", o->rxlpf);
		if (o->plus_rxhpf_enabled && o->plus_rxlpf_enabled && rx_high >= rx_low) {
			ast_log(LOG_ERROR,
				"RadioPlus/%s: rxhpf cutoff must be below rxlpf cutoff\n", o->name);
			memset(o->plus_local_native, 0, sizeof(o->plus_local_native));
			return;
		}
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
	{
		double gain_db = chain.input_gain_configured ? chain.agc.input_gain_db
							     : effective_rx_input_gain_db(o);
		double gain = pow(10.0, gain_db / 20.0);
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i)
			o->plus_local_native[i] *= gain;
	}
	{
		struct txagc_config filter_cfg;
		double rx_high = o->plus_rxhpf_exact
					 ? o->plus_rxhpf_hz
					 : usbradioplus_legacy_cutoff("rxhpf", o->rxhpf);
		double rx_low = o->plus_rxlpf_exact ? o->plus_rxlpf_hz
						    : usbradioplus_legacy_cutoff("rxlpf", o->rxlpf);
		memset(&filter_cfg, 0, sizeof(filter_cfg));
		filter_cfg.receive_bandpass_enabled = 1;
		filter_cfg.receive_bandpass_highpass_hz = 20.0;
		filter_cfg.receive_bandpass_lowpass_hz = 5000.0;
		filter_cfg.ctcss_notch_width_hz = 5.0;
		/* Apply exactly one PL-rejection filter after the qualified receiver
		 * gate and before RNNoise or dynamics. An explicit processing mode owns
		 * its type and cutoff; otherwise rxhpf supplies the compatible default. */
		if (local_chain_enabled && chain.ctcss_filter_configured) {
			filter_cfg.ctcss_filter_mode = chain.agc.ctcss_filter_mode;
			filter_cfg.ctcss_highpass_hz = chain.agc.ctcss_highpass_hz;
			filter_cfg.ctcss_notch_width_hz = chain.agc.ctcss_notch_width_hz;
			if (filter_cfg.ctcss_filter_mode == TXAGC_CTCSS_FILTER_NOTCH)
				ast_copy_string(filter_cfg.ctcss_notch_frequencies, o->rxctcssfreq,
						sizeof(filter_cfg.ctcss_notch_frequencies));
		} else {
			filter_cfg.ctcss_filter_mode = o->plus_rxhpf_enabled
							       ? TXAGC_CTCSS_FILTER_HIGHPASS
							       : TXAGC_CTCSS_FILTER_DISABLED;
			filter_cfg.ctcss_highpass_hz = rx_high;
		}
		if (local_chain_enabled) {
			filter_cfg.receive_bandpass_enabled = chain.agc.receive_bandpass_enabled;
			filter_cfg.receive_bandpass_highpass_hz =
				chain.agc.receive_bandpass_highpass_hz;
			filter_cfg.receive_bandpass_lowpass_hz =
				chain.agc.receive_bandpass_lowpass_hz;
		}
		filter_cfg.splatter_filter_enabled = o->plus_rxlpf_enabled;
		filter_cfg.output_lowpass_hz = rx_low;
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
	/* app_rpt and the CM119 use independent clocks. Convert queued frames into
	 * an elastic native-rate FIFO and trim the ratio gently around its target. */
	while (o->plus_native_fifo.count < PLUS_LINK_NATIVE_TARGET_SAMPLES) {
		unsigned int queued_frames;
		double correction, ratio;
		int have_frame = 0;
		memset(o->plus_link_8k, 0, sizeof(o->plus_link_8k));
		ast_mutex_lock(&o->plus_link_lock);
		queued_frames = o->plus_program_queue.count;
		have_frame = urp_program_queue_pop(&o->plus_program_queue, o->plus_link_8k);
		ast_mutex_unlock(&o->plus_link_lock);
		if (!have_frame)
			break;
		correction = urp_clock_recovery_update(&o->plus_link_clock,
						       o->plus_native_fifo.count +
							       queued_frames * URP_NATIVE_SAMPLES,
						       PLUS_LINK_NATIVE_TARGET_SAMPLES);
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
	}
	if (!o->plus_native_fifo.primed &&
	    o->plus_native_fifo.count >= PLUS_LINK_NATIVE_TARGET_SAMPLES) {
		o->plus_native_fifo.primed = 1;
	}
	if (o->plus_native_fifo.primed && !plus_link_native_pop(o, o->plus_link_native)) {
		o->plus_native_fifo.primed = 0;
		if (o->txkeyed || usbradioplus_program_pending(o))
			o->plus_link_queue_underflows++;
		urp_src_reset(o->plus_up);
		urp_clock_recovery_reset(&o->plus_link_clock);
		urp_native_fifo_reset(&o->plus_native_fifo);
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

		usbradioplus_processing_get_composite(&composite_chain);
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
		if (!composite_chain.splatter_filter_configured) {
			final_cfg.splatter_filter_enabled =
				o->plus_txhpf_enabled || o->plus_txlpf_enabled;
			final_cfg.output_highpass_hz =
				o->plus_txhpf_enabled
					? (o->plus_txhpf_exact
						   ? o->plus_txhpf_hz
						   : usbradioplus_legacy_cutoff("txhpf", o->txhpf))
					: 0.0;
			final_cfg.output_lowpass_hz =
				o->plus_txlpf_enabled
					? (o->plus_txlpf_exact
						   ? o->plus_txlpf_hz
						   : usbradioplus_legacy_cutoff("txlpf", o->txlpf))
					: 0.0;
		}
		/* A configured RadioPlus yes/no is authoritative.  Only an omitted
		 * setting inherits txprelim, txlimonly, and txslimsp semantics. */
		if (!composite_chain.lookahead_limiter_configured &&
		    (o->txprelim || o->txlimonly)) {
			final_cfg.lookahead_limiter_enabled = 1;
			final_cfg.lookahead_limit_dbfs =
				urp_legacy_limiter_ceiling_dbfs(o->txslimsp);
		}
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

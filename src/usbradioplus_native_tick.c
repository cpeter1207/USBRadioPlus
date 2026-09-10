/** @file
 * @brief Native receive gating, processing, repeat, echo, and transmitter rendering.
 */

#include "asterisk.h"

#include <sched.h>
#include <string.h>
#include <time.h>

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

/** Number of native frames retained solely for scheduler jitter protection. */
#define URP_NATIVE_WORKER_QUEUE_FRAMES 3U
/** Native TX frames retained while the physical DAC temporarily has no room.
 *
 * This is a capacity limit, not intentional playout latency: the hardware
 * callback normally acknowledges one frame on every native tick.  Keeping a
 * separate bounded TX queue lets the app_rpt receive side retain its cadence
 * while a transient device-full condition retries the oldest DAC frame. */
#define URP_NATIVE_WORKER_TX_QUEUE_FRAMES 8U
/** Interleaved PCM words in one native CM119 hardware frame. */
#define URP_NATIVE_WORKER_STEREO_SAMPLES (URP_NATIVE_SAMPLES * 2U)

/** Snapshot paired with one raw hardware frame sent to the native worker.
 *
 * The hardware callback owns the producer side.  It captures every signaling
 * decision needed by the renderer so the worker never dereferences mutable
 * radio state after the callback leaves its lock-free access span.
 */
struct native_worker_input {
	/** Immutable graph generation retained until the worker finishes this frame. */
	struct usbradioplus_native_graph_set *graphs;
	/** Receiver qualification used by optional CPU saving and local repeat. */
	int rxkeyed;
	/** Native DTMF muting state. */
	int toneflag;
	/** Nonzero when Asterisk DTMF detection is enabled. */
	int usedtmf;
	/** Nonzero when a DSP instance supplies DTMF state. */
	int has_dsp;
	/** Selected decoder index for a dynamic CTCSS notch graph. */
	int decoded_ctcss;
	/** Exact per-sample carrier gate produced by the signaling frontend. */
	uint8_t carrier_gate[URP_NATIVE_SAMPLES];
	/** Logical transmitter state that must align with rendered PCM. */
	int tx_ptt_out;
	/** Transmit signaling state machine state. */
	int tx_state;
	/** CTCSS oscillator enable and turn-off state. */
	int tx_ctcss_enabled;
	/** Nonzero suppresses continuous CTCSS during its turn-off sequence. */
	int tx_ctcss_off;
	/** CTCSS generation settings captured with the block. */
	/** Phase-shift tail angle in degrees. */
	double tx_ctcss_phase_shift;
	/** Replacement-tail tone frequency in hertz. */
	double tx_ctcss_tail_tone_hz;
	/** Continuous transmit CTCSS frequency in hertz. */
	double tx_ctcss_frequency_hz;
	/** Continuous CTCSS peak in native PCM codes. */
	double tx_ctcss_peak;
	/** DCS generation settings captured with the block. */
	int dcs_enabled;
	/** Nonzero selects the DCS end-of-transmission tone. */
	int dcs_turnoff_active;
	/** Configured transmit DCS code. */
	int dcs_transmit_code;
	/** Nonzero selects inverted transmit DCS polarity. */
	int dcs_transmit_inverted;
	/** DCS peak amplitude in PCM codes. */
	double dcs_peak;
	/** Calibrated test-tone selection. */
	int test_tone_enabled;
};

/** One rendered app-facing output descriptor. */
struct native_worker_output {
	/** App-facing samples paired with this frame. */
	unsigned int app_samples;
};

/** One transmit record whose PCM remains queued until the DAC accepts it. */
struct native_worker_tx_output {
	/** Monotonic worker-output sequence paired with this native PCM frame. */
	unsigned int sequence;
	/** Zero prevents a malformed app-facing record from reaching the DAC. */
	int valid;
};

/** One immutable worker diagnostics buffer pinned by control-plane readers. */
struct native_worker_diagnostics {
	/** Complete native-worker statistic snapshot. */
	struct usbradioplus_native_worker_stats statistics;
	/** Asterisk-compatible transmit-audio statistic snapshot. */
	struct audiostatistics tx_audio_statistics;
};

/** Per-channel asynchronous native processing state.
 *
 * Each audio queue is SPSC and sample-addressed.  The hardware callback only
 * copies bounded PCM and snapshot metadata; this worker alone calls FFmpeg,
 * RNNoise, and libsamplerate.
 */
struct usbradioplus_native_worker {
	/** Owning channel; valid until stop joins the worker. */
	struct chan_usbradio_pvt *channel;
	/** Background owner of non-real-time graph execution. */
	pthread_t thread;
	/** Stop request checked between complete native frames. */
	atomic_int stopping;
	/** Nonzero after pthread_create() succeeds. */
	int started;
	/** Raw interleaved ADC PCM, callback producer and worker consumer. */
	struct urp_sample_queue input_pcm;
	/** Storage backing the raw ADC SPSC queue. */
	short input_samples[URP_NATIVE_WORKER_QUEUE_FRAMES * URP_NATIVE_WORKER_STEREO_SAMPLES];
	/** Processed app_rpt PCM, worker producer and callback consumer. */
	struct urp_sample_queue receive_pcm;
	/** Storage backing the app_rpt receive SPSC queue. */
	short receive_samples[URP_NATIVE_WORKER_QUEUE_FRAMES * URP_NATIVE_SAMPLES];
	/** Rendered interleaved DAC PCM, worker producer and callback consumer. */
	struct urp_sample_queue transmit_pcm;
	/** Storage backing the retriable DAC SPSC queue. */
	short transmit_samples[URP_NATIVE_WORKER_TX_QUEUE_FRAMES *
			       URP_NATIVE_WORKER_STEREO_SAMPLES];
	/** Input metadata ring paired with complete input PCM frames. */
	struct native_worker_input input[URP_NATIVE_WORKER_QUEUE_FRAMES];
	/** Worker-consumed input metadata cursor. */
	atomic_uint input_read;
	/** Callback-published input metadata cursor. */
	atomic_uint input_write;
	/** Output metadata ring paired with complete output PCM frames. */
	struct native_worker_output output[URP_NATIVE_WORKER_QUEUE_FRAMES];
	/** Callback-consumed output metadata cursor. */
	atomic_uint output_read;
	/** Worker-published output metadata cursor. */
	atomic_uint output_write;
	/** TX metadata remains independent of app-facing output consumption. */
	struct native_worker_tx_output tx_output[URP_NATIVE_WORKER_TX_QUEUE_FRAMES];
	/** Callback-consumed transmit metadata cursor. */
	atomic_uint tx_read;
	/** Worker-published transmit metadata cursor. */
	atomic_uint tx_write;
	/** Callback-owned identity of the DAC frame copied for the current write. */
	unsigned int tx_staged_sequence;
	/** Nonzero while tx_staged_sequence awaits device-write acknowledgement. */
	int tx_staged;
	/** Worker-owned transmitter DCS state, separate from receive decoding. */
	struct urp_dcs_state dcs;
	/** Last configured transmit DCS code. */
	int dcs_code;
	/** Last configured transmit DCS polarity. */
	int dcs_inverted;
	/** Worker-owned CTCSS and calibrated-tone phase state. */
	struct urp_ctcss_generator ctcss_generator;
	/** Phase accumulator for the calibrated transmitter test tone. */
	double test_tone_phase;
	/** Worker-owned receive, link, and resampler workspaces. */
	short rx_native[URP_NATIVE_SAMPLES];
	/** De-emphasized and filtered local receive workspace. */
	double local_native[URP_NATIVE_SAMPLES];
	/** Native-rate program/link workspace. */
	short link_native[URP_NATIVE_SAMPLES];
	/** Source-rate echo workspace. */
	short link_app[URP_NATIVE_SAMPLES];
	/** Receive squelch-delay storage. */
	short rx_delay[RXSQDELAYBUFSIZE * 6];
	/** Circular index into receive squelch-delay storage. */
	unsigned int rx_delay_index;
	/** Persistent resamplers never touched by the hardware callback. */
	struct urp_src *echo_up;
	/** Persistent native-to-app_rpt converter. */
	struct urp_src *down;
	/** Source rate currently configured in the persistent converters. */
	unsigned int app_rpt_rate;
	/** RNNoise state is likewise owned only by this worker. */
	struct txagc_rnnoise local_rnnoise;
	/** Native echo recording/playback is worker-owned to avoid callback races. */
	struct urp_parrot_state parrot;
	/** Prior receive qualification used for native echo transitions. */
	int previous_rxkeyed;
	/** Mutable diagnostics, written only by the worker. */
	struct usbradioplus_native_worker_stats statistics;
	/** Asterisk-compatible transmitter meter owned only by the worker. */
	struct audiostatistics tx_audio_statistics;
	/** Double-buffered control-plane views of worker diagnostics. */
	struct native_worker_diagnostics published_statistics[2];
	/** Index of the currently published immutable diagnostics buffer. */
	atomic_uint statistics_index;
	/** Readers pinning each published diagnostics buffer during a copy. */
	atomic_uint statistics_readers[2];
	/** Monotonic request number consumed by the worker between complete frames. */
	atomic_uint statistics_reset_request;
	/** Last statistics-reset request handled by the worker. */
	unsigned int statistics_reset_seen;
	/** Monotonic request number used to discard echo state outside the callback. */
	atomic_uint parrot_clear_request;
	/** Last native-parrot clear request handled by the worker. */
	unsigned int parrot_clear_seen;
	/** Monotonic request number used to discard legacy echo PCM safely. */
	atomic_uint legacy_echo_clear_request;
	/** Last legacy-echo clear request handled by the worker. */
	unsigned int legacy_echo_clear_seen;
	/** Callback-owned queue diagnostics published by the worker. */
	atomic_uint_fast64_t input_overflows;
	/** Callback count of missing app-facing worker output. */
	atomic_uint_fast64_t output_underflows;
	/** Callback count of malformed worker output descriptors. */
	atomic_uint_fast64_t output_malformed;
};

#ifdef URP_PROCESSING_TESTING
/** Number of deterministic statistics-reader invalidations still requested. */
static atomic_uint native_worker_test_statistics_retry_count;
/** Selected synthetic output-queue producer failure. */
static atomic_uint native_worker_test_output_push_fault;
/** Synthetic generation delta injected between coherent-snapshot loads. */
static atomic_uint native_worker_test_hardware_snapshot_delta;

/** @brief Invalidate one pinned diagnostics snapshot when a test requests it.
 * @param worker Worker whose published index is being read.
 * @param index Snapshot index pinned by the reader.
 */
static void native_worker_test_invalidate_statistics_read(struct usbradioplus_native_worker *worker,
							  unsigned int index)
{
	unsigned int remaining = atomic_load_explicit(&native_worker_test_statistics_retry_count,
						      memory_order_acquire);

	if (remaining != 0U) {
		atomic_store_explicit(&native_worker_test_statistics_retry_count, remaining - 1U,
				      memory_order_release);
		atomic_store_explicit(&worker->statistics_index, index ^ 1U, memory_order_release);
	}
}
#endif

/** @brief Return current occupancy of a small monotonic SPSC metadata ring.
 * @param read Consumer cursor.
 * @param write Producer cursor.
 * @return Number of published records not yet consumed.
 */
static unsigned int native_worker_control_count(const atomic_uint *read, const atomic_uint *write)
{
	unsigned int produced = atomic_load_explicit(write, memory_order_acquire);
	unsigned int consumed = atomic_load_explicit(read, memory_order_acquire);

	return produced - consumed;
}

/** @brief Publish one worker-owned diagnostics block without a data race.
 * @param worker Worker owning the mutable diagnostics.
 *
 * The worker writes only the inactive buffer after observing that no reader
 * pins it, then release-publishes its index. A reader increments the selected
 * buffer's counter and rechecks the index before copying. Thus a reader that
 * races a swap retries before touching storage, while a writer never overwrites
 * an older buffer still being copied. If both buffers are temporarily pinned,
 * the worker leaves the last complete snapshot in place; diagnostics may be
 * one frame stale but neither endpoint locks or races.
 */
static void native_worker_publish_statistics(struct usbradioplus_native_worker *worker)
{
	unsigned int active = atomic_load_explicit(&worker->statistics_index, memory_order_acquire);
	unsigned int inactive = active ^ 1U;

	if (atomic_load_explicit(&worker->statistics_readers[inactive], memory_order_acquire) != 0U)
		return;
	worker->published_statistics[inactive].statistics = worker->statistics;
	worker->published_statistics[inactive].tx_audio_statistics = worker->tx_audio_statistics;
	atomic_store_explicit(&worker->statistics_index, inactive, memory_order_release);
}

/** @brief Initialize worker-owned diagnostic values before publishing them.
 * @param worker Worker whose diagnostics are initialized.
 */
static void native_worker_statistics_init(struct usbradioplus_native_worker *worker)
{
	memset(&worker->statistics, 0, sizeof(worker->statistics));
	worker->statistics.adc_peak_dbfs = -INFINITY;
	worker->statistics.adc_max_peak_dbfs = -INFINITY;
	worker->statistics.preemphasis_input_peak_dbfs = -INFINITY;
	worker->statistics.preemphasis_input_max_peak_dbfs = -INFINITY;
	worker->statistics.local_tx_peak_dbfs = -INFINITY;
	worker->statistics.local_tx_max_peak_dbfs = -INFINITY;
	worker->statistics.tx_program_peak_dbfs = -INFINITY;
	worker->statistics.tx_program_max_peak_dbfs = -INFINITY;
}

/** @brief Fold callback-owned queue event counters into worker diagnostics.
 * @param worker Worker receiving the counter snapshot.
 */
static void native_worker_copy_queue_statistics(struct usbradioplus_native_worker *worker)
{
	worker->statistics.worker_input_overflows =
		atomic_load_explicit(&worker->input_overflows, memory_order_relaxed);
	worker->statistics.worker_output_underflows =
		atomic_load_explicit(&worker->output_underflows, memory_order_relaxed);
	worker->statistics.worker_output_malformed =
		atomic_load_explicit(&worker->output_malformed, memory_order_relaxed);
}

/** @brief Copy only scalar FFmpeg measurements from a worker-owned graph.
 * @param destination Destination statistic snapshot.
 * @param source Prepared FFmpeg graph providing measurements.
 */
static void
native_worker_copy_filter_statistics(struct usbradioplus_native_filter_statistics *destination,
				     const struct txagc_avfilter *source)
{
	if (!destination || !source)
		return;
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
	destination->cleanup_post_5_8_rms_dbfs = source->cleanup_post_5_8_rms_dbfs;
	destination->cleanup_post_5_8_max_rms_dbfs = source->cleanup_post_5_8_max_rms_dbfs;
	destination->cleanup_pre_8_plus_rms_dbfs = source->cleanup_pre_8_plus_rms_dbfs;
	destination->cleanup_pre_8_plus_max_rms_dbfs = source->cleanup_pre_8_plus_max_rms_dbfs;
	destination->cleanup_post_8_plus_rms_dbfs = source->cleanup_post_8_plus_rms_dbfs;
	destination->cleanup_post_8_plus_max_rms_dbfs = source->cleanup_post_8_plus_max_rms_dbfs;
}

/** @brief Update the worker-owned transmitter meter outside the hardware callback.
 * @param worker Worker receiving the updated meter state.
 * @param samples Interleaved transmitter PCM samples.
 * @param count Number of PCM samples.
 */
static void native_worker_check_tx_audio(struct usbradioplus_native_worker *worker, short *samples,
					 size_t count)
{
#ifdef URP_CHANNEL_MODERN
	ast_radio_check_audio(samples, &worker->tx_audio_statistics, (short)count, 0);
#else
	ast_radio_check_audio(samples, &worker->tx_audio_statistics, (short)count);
#endif
}

/** @brief Reset meter fields owned by a prepared FFmpeg graph between frames.
 * @param filter Prepared graph whose extrema are reset.
 */
static void native_worker_reset_filter_statistics(struct txagc_avfilter *filter)
{
	if (!filter)
		return;
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

/** @brief Apply asynchronous control-plane requests at a frame boundary.
 * @param worker Worker consuming control-plane requests.
 * @param graphs Current graph generation, if one is active.
 */
static void native_worker_apply_requests(struct usbradioplus_native_worker *worker,
					 struct usbradioplus_native_graph_set *graphs)
{
	unsigned int request =
		atomic_load_explicit(&worker->statistics_reset_request, memory_order_acquire);

	if (request != worker->statistics_reset_seen) {
		native_worker_statistics_init(worker);
		atomic_store_explicit(&worker->input_overflows, 0U, memory_order_relaxed);
		atomic_store_explicit(&worker->output_underflows, 0U, memory_order_relaxed);
		atomic_store_explicit(&worker->output_malformed, 0U, memory_order_relaxed);
		if (graphs) {
			native_worker_reset_filter_statistics(&graphs->local_dynamics);
			native_worker_reset_filter_statistics(&graphs->final);
		}
		worker->statistics_reset_seen = request;
	}
	request = atomic_load_explicit(&worker->parrot_clear_request, memory_order_acquire);
	if (request != worker->parrot_clear_seen) {
		worker->parrot.count = 0;
		worker->parrot.play = 0;
		worker->parrot.playing = 0;
		worker->parrot.truncated = 0;
		worker->previous_rxkeyed = 0;
		atomic_store_explicit(&worker->channel->echoing, 0, memory_order_release);
		worker->parrot_clear_seen = request;
	}
	request = atomic_load_explicit(&worker->legacy_echo_clear_request, memory_order_acquire);
	if (request != worker->legacy_echo_clear_seen) {
		/* The worker is the legacy echo queue's consumer.  Advancing read to
		 * the published write cursor preserves SPSC ownership even if the audio
		 * adapter begins recording a new echo frame concurrently. */
		urp_sample_queue_discard(&worker->channel->echo_queue);
		atomic_store_explicit(&worker->channel->echoing, 0, memory_order_release);
		worker->legacy_echo_clear_seen = request;
	}
}

#ifndef URP_PROCESSING_TESTING
/** @brief Pause a non-real-time worker briefly after finding no complete frame. */
static void native_worker_wait(void)
{
	struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};

	(void)nanosleep(&pause, NULL);
}
#endif

/** @brief Render source-rate program PCM through the native clock-recovery converter.
 * @param channel Active native audio channel.
 * @param graphs Immutable graph generation supplying FIFO timing controls.
 * @param output Receives one native-rate output frame.
 * @return Nonzero when a complete native output block was rendered.
 */
static int read_native_program_elastic(struct chan_usbradio_pvt *channel,
				       const struct usbradioplus_native_graph_set *graphs,
				       short *output)
{
	struct rpcr_ring *ring = &channel->plus_program_ring;
	size_t index;
	int complete = 1;

	/* There is deliberately no keyed-start or recovery admission gate.  The
	 * device clock consumes one output sample per tick whether its program is
	 * speech or idle silence.  A rare producer shortfall is concealed by the
	 * ring, while its result remains visible to the diagnostic counters. */
	for (index = 0; index < URP_NATIVE_SAMPLES; ++index) {
		if (!rpcr_consumer_render_sample(ring, &output[index],
						 graphs->program_target_samples))
			complete = 0;
	}
	return complete;
}

/** @brief Process fixed receive conditioning and select the decoded-tone notch graph.
 * @param input Snapshot selecting the decoded-tone notch graph.
 * @param graphs Immutable graph generation containing the selected filter bank.
 * @param samples Native-rate receiver samples processed in place.
 * @param count Number of samples in samples.
 *
 * The base graph retains the configured PL mode.  In notch mode its frequency
 * list is intentionally empty; the selected CTCSS decoder index chooses a
 * separate prebuilt graph.  The selection uses atomic graph references only,
 * so a decoder transition cannot allocate, lock, or rebuild in this callback.
 */
static void process_receive_filter(const struct native_worker_input *input,
				   struct usbradioplus_native_graph_set *graphs, double *samples,
				   size_t count)
{
	int decoded = input->decoded_ctcss;

	(void)txagc_avfilter_process_prepared(&graphs->receive_filter, samples, count);
	if (decoded > CTCSS_NULL && decoded < CTCSS_NUM_CODES &&
	    graphs->ctcss_notch[decoded].configured)
		(void)txagc_avfilter_process_prepared(&graphs->ctcss_notch[decoded], samples,
						      count);
}

/** @brief Read a coherent control-plane hardware snapshot without a lock.
 * @param channel Native channel supplying published hardware values.
 * @param txmixa Receives the output-A routing assignment.
 * @param txmixb Receives the output-B routing assignment.
 *
 * Setup and reload bracket updates with an odd generation. A native tick can
 * retry atomic loads instead of taking the adapter mixer lock in the callback.
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
		/* A deterministic harness can model a writer completing between the
		 * two loads without scheduling a timing-sensitive helper thread. */
		after += atomic_exchange_explicit(&native_worker_test_hardware_snapshot_delta, 0U,
						  memory_order_acq_rel);
#endif
		/* The earlier odd-generation retry established that an unchanged value
		 * is even, so equality alone proves this snapshot is coherent. */
		if (before == after)
			return;
	}
	/* Do not spin in the native callback if a control-plane thread was
	 * preempted while publishing. A rare mixed hardware block changes only
	 * output routing/CTCSS amplitude; the following tick receives a coherent
	 * snapshot, and audio scheduling never waits on a writer. */
	*txmixa = (enum radio_tx_mix)atomic_load_explicit(&channel->plus_applied_txmixa,
							  memory_order_relaxed);
	*txmixb = (enum radio_tx_mix)atomic_load_explicit(&channel->plus_applied_txmixb,
							  memory_order_relaxed);
}

/** @brief Render one queued native frame outside the hardware callback.
 * @param worker Worker owning all mutable DSP and conversion state.
 * @param input Signaling and graph snapshot paired with the input frame.
 * @param adc_pcm Interleaved native ADC PCM for this exact hardware frame.
 * @param app_pcm Destination app_rpt-rate receive PCM.
 * @param stereo Destination interleaved native DAC PCM.
 *
 * FFmpeg, RNNoise, and sample-rate conversion live here rather than in the
 * hardware callback.  Every pointer is worker-owned or points to one complete
 * SPSC frame, so processing cannot race the next device callback.
 */
static void native_worker_render_block(struct usbradioplus_native_worker *worker,
				       const struct native_worker_input *input,
				       const short *adc_pcm, short *app_pcm, short *stereo)
{
	struct chan_usbradio_pvt *o = worker->channel;
	struct usbradioplus_native_graph_set *graphs = input->graphs;
	double program[URP_NATIVE_SAMPLES];
	double local_program[URP_NATIVE_SAMPLES];
	double ctcss[URP_NATIVE_SAMPLES];
	double dcs[URP_NATIVE_SAMPLES];
	short network_program[URP_NATIVE_SAMPLES];
	size_t used = 0, made = 0, i;
	double ctcss_phase_shift_degrees;
	double ctcss_tail_tone_hz;
	double ctcss_frequency, ctcss_peak_a, ctcss_peak_b;
	double ctcss_bias_a, ctcss_bias_b;
	int dcs_normal_active, dcs_turnoff_active;
	enum radio_tx_mix txmixa;
	enum radio_tx_mix txmixb;

	if (!graphs)
		return;
	native_worker_apply_requests(worker, graphs);
	if (worker->app_rpt_rate != graphs->app_rpt_rate) {
		/* Rate changes occur on the control plane, but converter history belongs
		 * exclusively to this worker. Reset it at the first matching frame. */
		urp_src_reset(worker->echo_up);
		urp_src_reset(worker->down);
		worker->app_rpt_rate = graphs->app_rpt_rate;
	}
	read_hardware_snapshot(o, &txmixa, &txmixb);
	ctcss_phase_shift_degrees = input->tx_ctcss_phase_shift;
	ctcss_tail_tone_hz = input->tx_ctcss_tail_tone_hz;
	ctcss_frequency = input->tx_ctcss_frequency_hz;
	/* The configured CTCSS level is an exact native PCM peak.  Hardware output
	 * gain is applied by the CM119 after this renderer and must not rescale it. */
	ctcss_peak_a = input->tx_ctcss_peak;
	ctcss_peak_b = input->tx_ctcss_peak;
	ctcss_bias_a = 0.0;
	ctcss_bias_b = 0.0;
	if (ctcss_tail_tone_hz > 0.0)
		urp_ctcss_generate_tail_tone(&worker->ctcss_generator, ctcss, URP_NATIVE_SAMPLES,
					     ctcss_tail_tone_hz, 1.0,
					     input->tx_ctcss_enabled && !input->tx_ctcss_off);
	else
		urp_ctcss_generate(
			&worker->ctcss_generator, ctcss, URP_NATIVE_SAMPLES, ctcss_frequency, 1.0,
			input->tx_ctcss_enabled && !input->tx_ctcss_off, ctcss_phase_shift_degrees);
#ifdef URP_PROCESSING_TESTING
	/* Keep legacy unit fixtures observable without making this channel field a
	 * production cross-thread data path. */
	o->plus_ctcss_generator = worker->ctcss_generator;
#endif
	/* DCS shares the signaling output routes with CTCSS, but is generated by
	 * its own NRZ encoder so no CTCSS calibration or phase state is reused.
	 * The signaling engine deliberately keeps physical PTT asserted while it
	 * drains its finishing state. Once a DCS tail begins, emit only the tail
	 * (then silence) until a real rekey returns the state to ACTIVE. */
	dcs_turnoff_active = input->dcs_turnoff_active;
	dcs_normal_active =
		input->dcs_enabled && input->tx_ptt_out && input->tx_state == CHAN_TXSTATE_ACTIVE;
	if (worker->dcs_code != input->dcs_transmit_code ||
	    worker->dcs_inverted != input->dcs_transmit_inverted) {
		urp_dcs_configure(&worker->dcs, -1, 0, input->dcs_transmit_code,
				  input->dcs_transmit_inverted);
		worker->dcs_code = input->dcs_transmit_code;
		worker->dcs_inverted = input->dcs_transmit_inverted;
	}
	urp_dcs_generate(&worker->dcs, dcs, URP_NATIVE_SAMPLES, URP_RATE_NATIVE, input->dcs_peak,
			 dcs_normal_active || dcs_turnoff_active, dcs_turnoff_active);
	/* DCS remains outside pre-emphasis and speech dynamics. Its dedicated
	 * shared-FFmpeg shaping graph was prepared before this hardware callback. */
	if (txagc_avfilter_process_prepared(dcs_turnoff_active ? &graphs->dcs_turnoff
							       : &graphs->dcs,
					    dcs, URP_NATIVE_SAMPLES) < 0)
		memset(dcs, 0, sizeof(dcs));
	/* Feed zeroes into the normal graph to drain its history during finishing,
	 * but do not let that history reintroduce the normal word before unkey. */
	if (!dcs_normal_active && !dcs_turnoff_active)
		memset(dcs, 0, sizeof(dcs));

	{
		struct urp_receive_block_stats stats;
		urp_prepare_receive_block(adc_pcm, worker->rx_native, worker->local_native,
					  URP_NATIVE_SAMPLES, worker->rx_delay,
					  graphs->receive_squelch_delay_samples,
					  &worker->rx_delay_index, &stats);
		worker->statistics.adc_peak_dbfs = urp_pcm_peak_dbfs(stats.peak);
		if (worker->statistics.adc_peak_dbfs > worker->statistics.adc_max_peak_dbfs)
			worker->statistics.adc_max_peak_dbfs = worker->statistics.adc_peak_dbfs;
		worker->statistics.adc_rail_samples += stats.rail_samples;
	}
	/* Keep de-emphasis separate so RNNoise can run immediately after the
	 * receiver gate and before any optional dynamics. */
	(void)txagc_avfilter_process_prepared(&graphs->receive_deemphasis, worker->local_native,
					      URP_NATIVE_SAMPLES);
	if (graphs->noise_squelch_gate) {
		/* Preserve continuous de-emphasis state, then gate at the detector's
		 * exact sample. app_rpt carrier notifications remain frame-cadenced;
		 * their previous-frame state must not admit a native squelch tail. */
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			if (!input->carrier_gate[i])
				worker->local_native[i] = 0.0;
		}
	}
	for (i = 0; i < URP_NATIVE_SAMPLES; ++i)
		worker->local_native[i] *= graphs->local_input_gain_linear;
	process_receive_filter(input, graphs, worker->local_native, URP_NATIVE_SAMPLES);
	/* Keep optional processing state advancing unless RX CPU saver is enabled
	 * and the receiver is unqualified. Receive routing still controls audibility. */
	if (graphs->local_chain_enabled && (!graphs->receive_cpu_saver || input->rxkeyed)) {
		/* De-emphasis, squelch qualification, and fixed receive filtering have
		 * already run. RNNoise is therefore the first optional dynamics stage. */
		if (graphs->local_rnnoise_enabled)
			(void)txagc_rnnoise_process_prepared(
				&worker->local_rnnoise, worker->local_native, URP_NATIVE_SAMPLES);
		if (!graphs->local_rnnoise_enabled)
			txagc_rnnoise_bypass(&worker->local_rnnoise);
		(void)txagc_avfilter_process_prepared(&graphs->local_dynamics, worker->local_native,
						      URP_NATIVE_SAMPLES);
	} else {
		txagc_rnnoise_bypass(&worker->local_rnnoise);
	}
	worker->statistics.rnnoise_frames = worker->local_rnnoise.rnnoise_frames;
	worker->statistics.rnnoise_output_samples = worker->local_rnnoise.output_samples;
	worker->statistics.rnnoise_startup_samples = worker->local_rnnoise.startup_samples;
	worker->statistics.rnnoise_errors = worker->local_rnnoise.errors;
	worker->statistics.rnnoise_vad_probability = worker->local_rnnoise.vad_probability;
	/* Echo recording/playback is part of the worker-owned native processing
	 * stream.  Detect receive transitions here rather than in the callback, so
	 * no callback mutates a recording that this worker is reading. */
	if (graphs->legacy_interface && graphs->echo_mode &&
	    urp_parrot_rx_transition(&worker->parrot, worker->previous_rxkeyed, input->rxkeyed))
		atomic_store_explicit(&o->echoing, 1, memory_order_release);
	worker->previous_rxkeyed = input->rxkeyed;
	/* Convert the PL-filtered local signal to the app_rpt rate exactly once. */
	memcpy(program, worker->local_native, sizeof(program));
	for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
		network_program[i] =
			urp_apply_gain((short)fmax(-32768.0, fmin(32767.0, program[i])), 1.0);
	}
	if (urp_rate_convert_prepared(worker->down, network_program, URP_NATIVE_SAMPLES,
				      URP_RATE_NATIVE, app_pcm, graphs->app_rpt_samples,
				      graphs->app_rpt_rate, &used, &made)) {
		worker->statistics.src_errors++;
	}

	memset(worker->link_native, 0, sizeof(worker->link_native));
	/* Legacy echo is a distinct source. Ordinary program PCM enters the shared
	 * source-rate ring, whose consumer renders directly at the hardware rate. */
	if (graphs->legacy_interface && atomic_load_explicit(&o->echoing, memory_order_acquire)) {
		int have_frame = 0;

		if (urp_sample_queue_samples(&o->echo_queue) >= graphs->app_rpt_samples) {
			for (i = 0; i < graphs->app_rpt_samples; ++i)
				(void)urp_sample_queue_pop_sample(&o->echo_queue,
								  &worker->link_app[i]);
			have_frame = 1;
		}
		if (!have_frame) {
			if (atomic_load_explicit(&o->txkeyed, memory_order_acquire))
				worker->statistics.link_queue_underflows++;
			urp_src_reset(worker->echo_up);
		} else if (graphs->app_rpt_rate == URP_RATE_NATIVE) {
			memcpy(worker->link_native, worker->link_app, sizeof(worker->link_native));
		} else {
			used = made = 0;
			if (urp_rate_convert_prepared(worker->echo_up, worker->link_app,
						      graphs->app_rpt_samples, graphs->app_rpt_rate,
						      worker->link_native, URP_NATIVE_SAMPLES,
						      URP_RATE_NATIVE, &used, &made) ||
			    used != graphs->app_rpt_samples) {
				worker->statistics.src_errors++;
				memset(worker->link_native, 0, sizeof(worker->link_native));
			} else if (made < URP_NATIVE_SAMPLES) {
				memset(worker->link_native + made, 0,
				       (URP_NATIVE_SAMPLES - made) * sizeof(*worker->link_native));
			}
		}
	} else if (!read_native_program_elastic(o, graphs, worker->link_native) &&
		   atomic_load_explicit(&o->txkeyed, memory_order_acquire)) {
		worker->statistics.link_queue_underflows++;
	}
	for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
		program[i] = worker->link_native[i];
	}
	memset(local_program, 0, sizeof(local_program));

	if (graphs->legacy_interface && worker->parrot.playing) {
		urp_parrot_play(&worker->parrot, local_program, URP_NATIVE_SAMPLES);
		/* The bounded parrot ring owns playback completion; mirror that state before
		 * releasing the echo admission gate. */
		worker->statistics.parrot_playback_frames++;
		if (!worker->parrot.playing) {
			atomic_store_explicit(&o->echoing, 0, memory_order_release);
		}
	} else if (graphs->legacy_interface && input->rxkeyed && graphs->software_repeat_enabled) {
		urp_native_repeat_prepare(local_program, worker->local_native, URP_NATIVE_SAMPLES,
					  1.0, input->usedtmf && input->has_dsp && input->toneflag);
		if (graphs->echo_mode && worker->parrot.audio) {
			urp_parrot_record(&worker->parrot, local_program, URP_NATIVE_SAMPLES,
					  worker->parrot.capacity);
		}
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			local_program[i] *= graphs->software_repeat_gain;
		}
	}
	{
		double peak = urp_double_peak(local_program, URP_NATIVE_SAMPLES);
		worker->statistics.preemphasis_input_peak_dbfs =
			peak > 0.0 ? 20.0 * log10(peak / 32768.0) : -INFINITY;
		if (worker->statistics.preemphasis_input_peak_dbfs >
		    worker->statistics.preemphasis_input_max_peak_dbfs)
			worker->statistics.preemphasis_input_max_peak_dbfs =
				worker->statistics.preemphasis_input_peak_dbfs;
	}
	/* Keep unlimited floating-point headroom through preemphasis, mixing, and
	 * final brick-wall band-pass. Low-frequency energy that will be removed
	 * must never hit a ceiling first and create broadband clipping products. */
	{
		double peak = urp_double_peak(local_program, URP_NATIVE_SAMPLES);
		worker->statistics.local_tx_peak_dbfs =
			peak > 0.0 ? 20.0 * log10(peak / 32768.0) : -INFINITY;
		if (worker->statistics.local_tx_peak_dbfs >
		    worker->statistics.local_tx_max_peak_dbfs) {
			worker->statistics.local_tx_max_peak_dbfs =
				worker->statistics.local_tx_peak_dbfs;
		}
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			program[i] += local_program[i];
		}
	}
	if (txagc_avfilter_process_prepared(&graphs->final, program, URP_NATIVE_SAMPLES) < 0)
		memset(program, 0, sizeof(program));
	/* Match the established deviation reference: bypass voice dynamics and emphasis,
	 * but retain configured output routing and safe PCM conversion. */
	if (input->test_tone_enabled) {
		const double step = 2.0 * M_PI * 1000.0 / URP_RATE_NATIVE;
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			/* The legacy 59/256 generator gain followed by its 8-to-48 kHz FIR
			 * produces exactly +/-7518 PCM codes at steady-state. */
			program[i] = URP_LEGACY_TEST_TONE_PEAK * sin(worker->test_tone_phase);
			worker->test_tone_phase += step;
			if (worker->test_tone_phase >= 2.0 * M_PI)
				worker->test_tone_phase -= 2.0 * M_PI;
		}
	} else {
		worker->test_tone_phase = 0.0;
	}
#ifdef URP_PROCESSING_TESTING
	/* Deterministic harness compatibility only; production keeps phase private. */
	o->plus_test_tone_phase = worker->test_tone_phase;
#endif
	{
		double peak = urp_double_peak(program, URP_NATIVE_SAMPLES);
		worker->statistics.tx_program_peak_dbfs =
			peak > 0.0 ? 20.0 * log10(peak / 32768.0) : -INFINITY;
		if (worker->statistics.tx_program_peak_dbfs >
		    worker->statistics.tx_program_max_peak_dbfs) {
			worker->statistics.tx_program_max_peak_dbfs =
				worker->statistics.tx_program_peak_dbfs;
		}
	}
	{
		short stats_stereo[URP_NATIVE_SAMPLES * 2];
		worker->statistics.tx_program_rail_samples += urp_render_transmit_block(
			program, ctcss, dcs, URP_NATIVE_SAMPLES, (enum urp_tx_output_mode)txmixa,
			(enum urp_tx_output_mode)txmixb, ctcss_peak_a, ctcss_bias_a, ctcss_peak_b,
			ctcss_bias_b, stereo, stats_stereo);
		/* Meter program audio before CM119 mixer gain, regardless of whether
		 * the configured voice/composite output is channel A or channel B. */
		native_worker_check_tx_audio(worker, stats_stereo, URP_NATIVE_SAMPLES * 2U);
	}
#ifdef URP_PROCESSING_TESTING
	/* Existing deterministic fixtures inspect these former workspaces directly.
	 * They are never a production communication path. */
	memcpy(o->plus_rx_native, worker->rx_native, sizeof(worker->rx_native));
	memcpy(o->plus_local_native, worker->local_native, sizeof(worker->local_native));
	memcpy(o->plus_link_native, worker->link_native, sizeof(worker->link_native));
	/* Fixtures run the worker synchronously.  Preserve their legacy observables
	 * without making these channel fields production cross-thread state. */
	o->plus_native_frames = worker->statistics.native_frames + 1U;
	o->plus_src_errors = worker->statistics.src_errors;
	o->plus_adc_peak_dbfs = worker->statistics.adc_peak_dbfs;
	o->plus_adc_max_peak_dbfs = worker->statistics.adc_max_peak_dbfs;
	o->plus_adc_rail_samples = worker->statistics.adc_rail_samples;
	o->plus_preemphasis_input_peak_dbfs = worker->statistics.preemphasis_input_peak_dbfs;
	o->plus_preemphasis_input_max_peak_dbfs =
		worker->statistics.preemphasis_input_max_peak_dbfs;
	o->plus_local_tx_peak_dbfs = worker->statistics.local_tx_peak_dbfs;
	o->plus_local_tx_max_peak_dbfs = worker->statistics.local_tx_max_peak_dbfs;
	o->plus_tx_program_peak_dbfs = worker->statistics.tx_program_peak_dbfs;
	o->plus_tx_program_max_peak_dbfs = worker->statistics.tx_program_max_peak_dbfs;
	o->plus_tx_program_rail_samples = worker->statistics.tx_program_rail_samples;
	o->plus_link_queue_underflows = worker->statistics.link_queue_underflows;
	o->plus_parrot_playback_frames = worker->statistics.parrot_playback_frames;
	o->plus_local_rnnoise.errors = worker->statistics.rnnoise_errors;
	o->plus_local_rnnoise.rnnoise_frames = worker->statistics.rnnoise_frames;
	o->plus_local_rnnoise.output_samples = worker->statistics.rnnoise_output_samples;
	o->plus_local_rnnoise.startup_samples = worker->statistics.rnnoise_startup_samples;
	o->plus_local_rnnoise.vad_probability = worker->statistics.rnnoise_vad_probability;
	o->txaudiostats = worker->tx_audio_statistics;
	o->plus_parrot_playing = worker->parrot.playing;
	o->plus_parrot_count = worker->parrot.count;
	o->plus_parrot_play = worker->parrot.play;
	o->plus_parrot_truncated = worker->parrot.truncated;
#endif
	worker->statistics.native_frames++;
	worker->statistics.parrot_samples = worker->parrot.count;
	worker->statistics.parrot_playing = worker->parrot.playing;
	worker->statistics.parrot_truncated = worker->parrot.truncated;
	native_worker_copy_queue_statistics(worker);
	native_worker_copy_filter_statistics(&worker->statistics.local_filter,
					     &graphs->local_dynamics);
	native_worker_copy_filter_statistics(&worker->statistics.receive_deemphasis_filter,
					     &graphs->receive_deemphasis);
	native_worker_copy_filter_statistics(&worker->statistics.final_filter, &graphs->final);
	native_worker_publish_statistics(worker);
}

/** @brief Copy one hardware callback's signaling result into an SPSC record.
 * @param snapshot Destination SPSC metadata record.
 * @param channel Callback-owned radio channel.
 * @param graphs Immutable graph generation retained with the record.
 */
static void native_worker_snapshot(struct native_worker_input *snapshot,
				   struct chan_usbradio_pvt *channel,
				   struct usbradioplus_native_graph_set *graphs)
{
	const urp_radio_state *radio = channel->radio;

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->graphs = graphs;
	snapshot->rxkeyed = channel->rxkeyed;
	snapshot->toneflag = channel->toneflag;
	snapshot->usedtmf = channel->usedtmf;
	snapshot->has_dsp = channel->dsp != NULL;
	if (radio && radio->rxCtcss) {
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

/** @brief Submit one complete callback frame without waiting for the worker.
 * @param worker Worker receiving the callback's input frame.
 */
static void native_worker_submit(struct usbradioplus_native_worker *worker)
{
	struct chan_usbradio_pvt *channel = worker->channel;
	struct usbradioplus_native_graph_set *graphs;
	struct native_worker_input *snapshot;
	const short *adc_pcm;
	unsigned int write;
	size_t index;

	if (!channel || !channel->radio)
		return;
	if (native_worker_control_count(&worker->input_read, &worker->input_write) >=
		    URP_NATIVE_WORKER_QUEUE_FRAMES ||
	    urp_sample_queue_samples(&worker->input_pcm) + URP_NATIVE_WORKER_STEREO_SAMPLES >
		    worker->input_pcm.capacity) {
		atomic_fetch_add_explicit(&worker->input_overflows, 1U, memory_order_relaxed);
		return;
	}
	graphs = usbradioplus_native_graphs_acquire(channel);
	if (!graphs)
		return;
	write = atomic_load_explicit(&worker->input_write, memory_order_relaxed);
	snapshot = &worker->input[write % URP_NATIVE_WORKER_QUEUE_FRAMES];
	native_worker_snapshot(snapshot, channel, graphs);
	adc_pcm = (const short *)(channel->usbradio_read_buf + AST_FRIENDLY_OFFSET);
	for (index = 0; index < URP_NATIVE_WORKER_STEREO_SAMPLES; ++index)
		(void)urp_sample_queue_push_sample(&worker->input_pcm, adc_pcm[index]);
	/* Publish metadata last: the consumer cannot observe a partial PCM frame. */
	atomic_store_explicit(&worker->input_write, write + 1U, memory_order_release);
}

/** @brief Render one complete queued frame if output queues have room.
 * @param worker Worker owning the queue endpoints and DSP state.
 * @return Nonzero when one frame was consumed and rendered.
 */
static int native_worker_process_one(struct usbradioplus_native_worker *worker)
{
	struct chan_usbradio_pvt *channel = worker->channel;
	struct native_worker_input *input;
	struct native_worker_output *output;
	struct native_worker_tx_output *tx_output;
	short adc_pcm[URP_NATIVE_WORKER_STEREO_SAMPLES];
	short app_pcm[URP_NATIVE_SAMPLES];
	short stereo[URP_NATIVE_WORKER_STEREO_SAMPLES];
	unsigned int read;
	unsigned int write;
	unsigned int tx_write;
	unsigned int app_samples;
	size_t index;

	if (!channel ||
	    native_worker_control_count(&worker->input_read, &worker->input_write) == 0U ||
	    native_worker_control_count(&worker->output_read, &worker->output_write) >=
		    URP_NATIVE_WORKER_QUEUE_FRAMES ||
	    native_worker_control_count(&worker->tx_read, &worker->tx_write) >=
		    URP_NATIVE_WORKER_TX_QUEUE_FRAMES ||
	    urp_sample_queue_samples(&worker->input_pcm) < URP_NATIVE_WORKER_STEREO_SAMPLES ||
	    worker->receive_pcm.capacity - urp_sample_queue_samples(&worker->receive_pcm) <
		    URP_NATIVE_SAMPLES ||
	    worker->transmit_pcm.capacity - urp_sample_queue_samples(&worker->transmit_pcm) <
		    URP_NATIVE_WORKER_STEREO_SAMPLES)
		return 0;
	read = atomic_load_explicit(&worker->input_read, memory_order_relaxed);
	input = &worker->input[read % URP_NATIVE_WORKER_QUEUE_FRAMES];
	app_samples = input->graphs ? input->graphs->app_rpt_samples : 0U;
	if (!input->graphs || app_samples == 0U || app_samples > URP_NATIVE_SAMPLES) {
		/* A malformed/retired control record still owns one complete raw frame.
		 * Consume it before advancing metadata so subsequent frames stay paired. */
		for (index = 0; index < URP_NATIVE_WORKER_STEREO_SAMPLES; ++index)
			(void)urp_sample_queue_pop_sample(&worker->input_pcm, &adc_pcm[index]);
		if (input->graphs)
			usbradioplus_native_graphs_release(channel);
		atomic_store_explicit(&worker->input_read, read + 1U, memory_order_release);
		return 0;
	}
	for (index = 0; index < URP_NATIVE_WORKER_STEREO_SAMPLES; ++index)
		(void)urp_sample_queue_pop_sample(&worker->input_pcm, &adc_pcm[index]);
	memset(app_pcm, 0, sizeof(app_pcm));
	memset(stereo, 0, sizeof(stereo));
	native_worker_render_block(worker, input, adc_pcm, app_pcm, stereo);
	for (index = 0; index < app_samples; ++index)
		(void)urp_sample_queue_push_sample(&worker->receive_pcm, app_pcm[index]);
	for (index = 0; index < URP_NATIVE_WORKER_STEREO_SAMPLES; ++index)
		(void)urp_sample_queue_push_sample(&worker->transmit_pcm, stereo[index]);
	write = atomic_load_explicit(&worker->output_write, memory_order_relaxed);
	tx_write = atomic_load_explicit(&worker->tx_write, memory_order_relaxed);
	output = &worker->output[write % URP_NATIVE_WORKER_QUEUE_FRAMES];
	tx_output = &worker->tx_output[tx_write % URP_NATIVE_WORKER_TX_QUEUE_FRAMES];
	output->app_samples = app_samples;
	/* Publish the TX token before its app-facing partner.  The callback can
	 * invalidate a malformed partner without racing a later TX read. */
	tx_output->sequence = write;
	tx_output->valid = 1;
	usbradioplus_native_graphs_release(channel);
	atomic_store_explicit(&worker->input_read, read + 1U, memory_order_release);
	atomic_store_explicit(&worker->tx_write, tx_write + 1U, memory_order_release);
	atomic_store_explicit(&worker->output_write, write + 1U, memory_order_release);
	return 1;
}

#ifndef URP_PROCESSING_TESTING
/** @brief Continuously execute queued graph work outside the hardware callback.
 * @param opaque Native worker supplied to pthread_create().
 * @return Always NULL after the worker stops.
 */
static void *native_worker_thread(void *opaque)
{
	struct usbradioplus_native_worker *worker = opaque;

	while (!atomic_load_explicit(&worker->stopping, memory_order_acquire)) {
		if (!native_worker_process_one(worker))
			native_worker_wait();
	}
	while (native_worker_process_one(worker)) {
	}
	return NULL;
}
#endif

/** @brief Fill hardware-facing buffers with a continuous silent frame.
 * @param channel Native channel whose app and DAC buffers are cleared.
 */
static void native_worker_silence(struct chan_usbradio_pvt *channel)
{
	memset(channel->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET, 0,
	       channel->plus_app_rpt_samples * sizeof(short));
	memset(channel->usbradio_write_buf, 0, URP_NATIVE_WORKER_STEREO_SAMPLES * sizeof(short));
}

/** @brief Discard up to one known SPSC PCM span without resetting either cursor.
 * @param queue Consumer-owned SPSC PCM queue.
 * @param samples Maximum number of samples to discard.
 *
 * The caller owns the queue consumer endpoint.  A malformed metadata record
 * cannot justify resetting a live SPSC queue: its producer may already be
 * preparing the following frame.  Bounded popping lets the next descriptor
 * make progress while preserving the queue's monotonic-cursor contract.
 */
static void native_worker_discard_pcm(struct urp_sample_queue *queue, unsigned int samples)
{
	short ignored;

	while (samples-- > 0U) {
		if (!urp_sample_queue_pop_sample(queue, &ignored))
			break;
	}
}

/** @brief Mark one published TX record unusable after its app counterpart fails validation.
 * @param worker Worker owning the transmit record.
 * @param sequence Output sequence whose transmit record is rejected.
 *
 * The worker publishes this token before the app-facing descriptor.  Once the
 * callback has observed that descriptor, its token is initialized and cannot
 * be reused until the callback advances the separate TX cursor.
 */
static void native_worker_reject_tx_output(struct usbradioplus_native_worker *worker,
					   unsigned int sequence)
{
	struct native_worker_tx_output *output;
	unsigned int tx_write = atomic_load_explicit(&worker->tx_write, memory_order_acquire);

	if (tx_write == sequence)
		return;
	output = &worker->tx_output[sequence % URP_NATIVE_WORKER_TX_QUEUE_FRAMES];
	if (output->sequence == sequence)
		output->valid = 0;
}

/** @brief Return the oldest DAC frame without advancing its SPSC cursor.
 * @param worker Worker owning the transmit queue.
 * @return Contiguous frame head, or NULL when no complete valid frame is available.
 *
 * A physical-device write is the acknowledgement for this queue.  Keeping the
 * cursor at the frame head lets a temporarily full DAC retry precisely the
 * same rendered frame instead of creating a time-compressed audio hole.
 */
static const short *native_worker_tx_head(struct usbradioplus_native_worker *worker)
{
	unsigned int pcm_read;

	while (native_worker_control_count(&worker->tx_read, &worker->tx_write) != 0U) {
		unsigned int read = atomic_load_explicit(&worker->tx_read, memory_order_relaxed);
		const struct native_worker_tx_output *output =
			&worker->tx_output[read % URP_NATIVE_WORKER_TX_QUEUE_FRAMES];
		if (output->sequence != read || !output->valid) {
			/* An invalid record was never offered to the DAC.  Its fixed PCM
			 * span may now be discarded so a later valid frame can make progress. */
			if (urp_sample_queue_samples(&worker->transmit_pcm) <
			    URP_NATIVE_WORKER_STEREO_SAMPLES)
				return NULL;
			native_worker_discard_pcm(&worker->transmit_pcm,
						  URP_NATIVE_WORKER_STEREO_SAMPLES);
			atomic_store_explicit(&worker->tx_read, read + 1U, memory_order_release);
			continue;
		}
		if (urp_sample_queue_samples(&worker->transmit_pcm) <
		    URP_NATIVE_WORKER_STEREO_SAMPLES)
			return NULL;
		pcm_read = atomic_load_explicit(&worker->transmit_pcm.read, memory_order_relaxed);
		/* TX storage is an integral number of native frames, so a frame head
		 * must be contiguous. Treat a violated invariant as unavailable rather
		 * than exposing a wrapped buffer to the sound device. */
		if ((pcm_read % worker->transmit_pcm.capacity) + URP_NATIVE_WORKER_STEREO_SAMPLES >
		    worker->transmit_pcm.capacity)
			return NULL;
		return worker->transmit_pcm.samples + pcm_read % worker->transmit_pcm.capacity;
	}
	return NULL;
}

/** @brief Copy the retriable DAC head into the hardware-facing output buffer.
 * @param worker Worker owning the retriable transmit queue.
 */
static void native_worker_stage_tx(struct usbradioplus_native_worker *worker)
{
	const short *source = native_worker_tx_head(worker);
	unsigned int read;

	/* A write acknowledgement is meaningful only for a frame this callback
	 * actually copied. A worker can publish a new head after this check and
	 * before the device call, so never let a later acknowledgement infer one. */
	worker->tx_staged = 0;
	if (!source)
		return;
	read = atomic_load_explicit(&worker->tx_read, memory_order_relaxed);
	memcpy(worker->channel->usbradio_write_buf, source,
	       URP_NATIVE_WORKER_STEREO_SAMPLES * sizeof(*source));
	worker->tx_staged_sequence = read;
	worker->tx_staged = 1;
}

/** @brief Retire exactly the DAC frame copied for a successful physical write.
 * @param worker Worker owning the acknowledged transmit queue head.
 *
 * A device adapter may deliberately replace the staged PCM with silence while
 * the transmitter is idle. That is an intentional discard of this exact
 * staged frame, never permission to retire a worker frame published later.
 */
static void native_worker_ack_tx(struct usbradioplus_native_worker *worker)
{
	const struct native_worker_tx_output *output;
	unsigned int read;

	if (!worker->tx_staged)
		return;
	read = atomic_load_explicit(&worker->tx_read, memory_order_relaxed);
	if (read != worker->tx_staged_sequence ||
	    native_worker_control_count(&worker->tx_read, &worker->tx_write) == 0U) {
		worker->tx_staged = 0;
		return;
	}
	output = &worker->tx_output[read % URP_NATIVE_WORKER_TX_QUEUE_FRAMES];
	if (output->sequence != read || !output->valid ||
	    urp_sample_queue_samples(&worker->transmit_pcm) < URP_NATIVE_WORKER_STEREO_SAMPLES) {
		worker->tx_staged = 0;
		return;
	}
	native_worker_discard_pcm(&worker->transmit_pcm, URP_NATIVE_WORKER_STEREO_SAMPLES);
	atomic_store_explicit(&worker->tx_read, read + 1U, memory_order_release);
	worker->tx_staged = 0;
}

/** @brief Retire one malformed rendered-frame record without wedging output.
 * @param worker Worker owning the malformed output record.
 * @param output Malformed app-facing descriptor.
 * @param read Sequence number of the malformed descriptor.
 *
 * Native output always has one fixed stereo DAC span and normal records use the
 * current configured app-frame size.  A zero or oversized descriptor therefore
 * discards that bounded expected app span plus the fixed DAC span before
 * advancing metadata.  The callback renders silence while signaling continues
 * independently, and the following valid record remains app/DAC aligned.
 */
static void native_worker_discard_malformed_output(struct usbradioplus_native_worker *worker,
						   const struct native_worker_output *output,
						   unsigned int read)
{
	unsigned int expected_app_samples = worker->channel->plus_app_rpt_samples;

	(void)output;
	/* A worker publishes every normal record at the current configured app rate.
	 * If metadata is zero or oversized, that rate is the only bounded pairing
	 * information left. Dropping it restores app/DAC chronology for the next
	 * valid descriptor instead of replaying stale app PCM. Its paired TX token
	 * is rejected independently so a delayed physical write cannot emit it. */
	if (expected_app_samples > URP_NATIVE_SAMPLES)
		expected_app_samples = URP_NATIVE_SAMPLES;
	native_worker_discard_pcm(&worker->receive_pcm, expected_app_samples);
	native_worker_reject_tx_output(worker, read);
	atomic_store_explicit(&worker->output_read, read + 1U, memory_order_release);
	atomic_fetch_add_explicit(&worker->output_malformed, 1U, memory_order_relaxed);
}

/** @brief Consume the next rendered frame, or retain cadence with silence.
 * @param worker Worker whose callback-facing output is consumed.
 */
static void native_worker_consume(struct usbradioplus_native_worker *worker)
{
	struct chan_usbradio_pvt *channel = worker->channel;
	const struct native_worker_output *output;
	short *app_pcm = (short *)(channel->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET);
	unsigned int read;
	size_t index;

	/* The legacy signaling pass may have filled this buffer.  The native TX
	 * queue is the sole DAC source, so start with silence before staging its
	 * unacknowledged head below. */
	native_worker_silence(channel);
	if (native_worker_control_count(&worker->output_read, &worker->output_write) == 0U) {
		atomic_fetch_add_explicit(&worker->output_underflows, 1U, memory_order_relaxed);
		native_worker_stage_tx(worker);
		return;
	}
	read = atomic_load_explicit(&worker->output_read, memory_order_relaxed);
	output = &worker->output[read % URP_NATIVE_WORKER_QUEUE_FRAMES];
	if (output->app_samples == 0U || output->app_samples > URP_NATIVE_SAMPLES) {
		/* A worker never publishes zero or oversized app spans.  Retire the
		 * corrupt descriptor now: retaining it would permanently wedge every
		 * later output frame. */
		native_worker_discard_malformed_output(worker, output, read);
		native_worker_stage_tx(worker);
		return;
	}
	if (urp_sample_queue_samples(&worker->receive_pcm) < output->app_samples) {
		/* Receive PCM is published before its descriptor, so an incomplete
		 * app span is corrupt rather than a normal producer race.  TX is kept
		 * on its own acknowledged queue and is rejected with this record. */
		native_worker_discard_malformed_output(worker, output, read);
		native_worker_stage_tx(worker);
		return;
	}
	if (output->app_samples != channel->plus_app_rpt_samples) {
		/* A rate change can leave one fully rendered old-rate record in flight.
		 * Consume both PCM halves before advancing its metadata so the next
		 * record remains aligned; never leave this valid-but-unusable record at
		 * the head of the queue indefinitely. */
		short discarded;

		for (index = 0; index < output->app_samples; ++index)
			(void)urp_sample_queue_pop_sample(&worker->receive_pcm, &discarded);
		native_worker_reject_tx_output(worker, read);
		atomic_store_explicit(&worker->output_read, read + 1U, memory_order_release);
		atomic_fetch_add_explicit(&worker->output_malformed, 1U, memory_order_relaxed);
		native_worker_stage_tx(worker);
		return;
	}
	for (index = 0; index < output->app_samples; ++index)
		(void)urp_sample_queue_pop_sample(&worker->receive_pcm, &app_pcm[index]);
	atomic_store_explicit(&worker->output_read, read + 1U, memory_order_release);
	/* RX/app delivery never waits for a DAC write.  The independent TX cursor
	 * advances only when its frame is acknowledged by the physical sink. */
	native_worker_stage_tx(worker);
}

#ifdef URP_PROCESSING_TESTING
/** @brief Execute all currently queued worker frames outside the callback in a test build.
 *
 * Production starts a dedicated worker thread.  The deterministic harness does
 * not create that thread, so tests must explicitly advance it between callback
 * invocations.  Keeping this pump separate proves that the callback has the
 * same bounded copy-only contract in every build.
 */
void usbradioplus_native_worker_test_process_all(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker;

	if (!channel || !(worker = channel->plus_native_worker))
		return;
	while (native_worker_process_one(worker)) {
	}
}

/** @brief Exercise the coherent-snapshot retry after a synthetic writer update.
 * @param channel Channel whose hardware routing snapshot is read.
 *
 * The production callback never schedules a helper while it samples these
 * atomics.  Test builds instead perturb one local post-load generation value.
 */
void usbradioplus_native_worker_test_hardware_snapshot_race(const struct chan_usbradio_pvt *channel)
{
	enum radio_tx_mix txmixa;
	enum radio_tx_mix txmixb;

	atomic_store_explicit(&native_worker_test_hardware_snapshot_delta, 2U,
			      memory_order_release);
	read_hardware_snapshot(channel, &txmixa, &txmixb);
}

/** @brief Inject one deliberately paired or malformed result into a stopped test worker.
 *
 * This hook never exists in production.  It uses the same SPSC endpoints as a
 * real renderer, but test builds have no native worker thread, so it can create
 * otherwise impossible metadata/PCM pairings deterministically.
 */
int usbradioplus_native_worker_test_inject_output(struct chan_usbradio_pvt *channel,
						  unsigned int descriptor_app_samples,
						  unsigned int paired_app_samples, short fill)
{
	struct usbradioplus_native_worker *worker;
	struct native_worker_output *output;
	struct native_worker_tx_output *tx_output;
	unsigned int write;
	unsigned int tx_write;
	unsigned int index;
	unsigned int receive_capacity;
	unsigned int transmit_capacity;
	unsigned int push_fault;

	if (!channel || !(worker = channel->plus_native_worker) ||
	    paired_app_samples > URP_NATIVE_SAMPLES ||
	    native_worker_control_count(&worker->output_read, &worker->output_write) >=
		    URP_NATIVE_WORKER_QUEUE_FRAMES ||
	    native_worker_control_count(&worker->tx_read, &worker->tx_write) >=
		    URP_NATIVE_WORKER_TX_QUEUE_FRAMES ||
	    worker->receive_pcm.capacity - urp_sample_queue_samples(&worker->receive_pcm) <
		    paired_app_samples ||
	    worker->transmit_pcm.capacity - urp_sample_queue_samples(&worker->transmit_pcm) <
		    URP_NATIVE_WORKER_STEREO_SAMPLES)
		return -1;
	receive_capacity = worker->receive_pcm.capacity;
	transmit_capacity = worker->transmit_pcm.capacity;
	push_fault = atomic_exchange_explicit(&native_worker_test_output_push_fault, 0U,
					      memory_order_acq_rel);
	if (push_fault == 1U)
		worker->receive_pcm.capacity = 0U;
	else if (push_fault == 2U)
		worker->transmit_pcm.capacity = 0U;
	for (index = 0U; index < paired_app_samples; ++index)
		if (!urp_sample_queue_push_sample(&worker->receive_pcm, fill)) {
			worker->receive_pcm.capacity = receive_capacity;
			worker->transmit_pcm.capacity = transmit_capacity;
			urp_sample_queue_reset(&worker->receive_pcm);
			urp_sample_queue_reset(&worker->transmit_pcm);
			return -1;
		}
	for (index = 0U; index < URP_NATIVE_WORKER_STEREO_SAMPLES; ++index)
		if (!urp_sample_queue_push_sample(&worker->transmit_pcm, fill)) {
			worker->receive_pcm.capacity = receive_capacity;
			worker->transmit_pcm.capacity = transmit_capacity;
			urp_sample_queue_reset(&worker->receive_pcm);
			urp_sample_queue_reset(&worker->transmit_pcm);
			return -1;
		}
	worker->receive_pcm.capacity = receive_capacity;
	worker->transmit_pcm.capacity = transmit_capacity;
	write = atomic_load_explicit(&worker->output_write, memory_order_relaxed);
	tx_write = atomic_load_explicit(&worker->tx_write, memory_order_relaxed);
	output = &worker->output[write % URP_NATIVE_WORKER_QUEUE_FRAMES];
	tx_output = &worker->tx_output[tx_write % URP_NATIVE_WORKER_TX_QUEUE_FRAMES];
	output->app_samples = descriptor_app_samples;
	tx_output->sequence = write;
	tx_output->valid = descriptor_app_samples != 0U &&
			   descriptor_app_samples <= URP_NATIVE_SAMPLES &&
			   descriptor_app_samples == paired_app_samples;
	atomic_store_explicit(&worker->tx_write, tx_write + 1U, memory_order_release);
	atomic_store_explicit(&worker->output_write, write + 1U, memory_order_release);
	return 0;
}

/** @brief Inject one producer failure after normal output preflight succeeds.
 * @param channel Channel with a deterministic native worker.
 * @param fault One for receive PCM or two for transmitter PCM.
 * @return Synthetic output-injection result.
 */
int usbradioplus_native_worker_test_output_push_failure(struct chan_usbradio_pvt *channel,
							unsigned int fault)
{
	atomic_store_explicit(&native_worker_test_output_push_fault, fault, memory_order_release);
	return usbradioplus_native_worker_test_inject_output(channel, URP_LINK_SAMPLES, 1U, 0);
}

/** @brief Exercise ordinary output consumption and PTT publication in a test build. */
void usbradioplus_native_worker_test_consume(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker;

	if (!channel || !(worker = channel->plus_native_worker))
		return;
	native_worker_consume(worker);
	/* The deterministic harness has no sound device.  Model one complete
	 * successful device write so existing waveform tests keep their normal
	 * callback-to-output cadence. Retention-specific tests use the unacked
	 * hook below. */
	native_worker_ack_tx(worker);
	/* No worker thread exists in this build. Publish callback counters immediately
	 * so the public snapshot remains the observable test contract. */
	native_worker_copy_queue_statistics(worker);
	native_worker_publish_statistics(worker);
	usbradioplus_publish_hardware_ptt(channel, channel->radio ? channel->radio->txPttOut : 0);
}

/** @brief Exercise output consumption without acknowledging the staged DAC frame. */
void usbradioplus_native_worker_test_consume_unacked(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker;

	if (!channel || !(worker = channel->plus_native_worker))
		return;
	native_worker_consume(worker);
	native_worker_copy_queue_statistics(worker);
	native_worker_publish_statistics(worker);
	usbradioplus_publish_hardware_ptt(channel, channel->radio ? channel->radio->txPttOut : 0);
}

/** @brief Exercise worker diagnostics guards without a concurrent callback.
 * @param channel Channel with a deterministic native worker.
 * @return Published diagnostics index after the deliberately pinned publish attempt.
 *
 * The normal producer and control-plane reader cannot be scheduled deterministically
 * between these two instructions.  This test-only hook pins the inactive snapshot,
 * proving that the worker leaves the last complete diagnostics block in place.
 */
unsigned int usbradioplus_native_worker_test_statistics_guards(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker = channel->plus_native_worker;
	struct txagc_avfilter source = {0};
	unsigned int active = atomic_load_explicit(&worker->statistics_index, memory_order_acquire);
	unsigned int inactive = active ^ 1U;

	atomic_store_explicit(&worker->statistics_readers[inactive], 1U, memory_order_release);
	native_worker_publish_statistics(worker);
	atomic_store_explicit(&worker->statistics_readers[inactive], 0U, memory_order_release);
	native_worker_copy_filter_statistics(NULL, &source);
	native_worker_copy_filter_statistics(&worker->statistics.local_filter, NULL);
	native_worker_reset_filter_statistics(NULL);
	return atomic_load_explicit(&worker->statistics_index, memory_order_acquire);
}

/** @brief Render a record with no graph generation in the deterministic harness.
 * @param channel Channel with a deterministic native worker.
 * @return Sum of sentinel output words, which remains nonzero when rendering is skipped.
 */
int usbradioplus_native_worker_test_render_without_graph(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker = channel->plus_native_worker;
	struct native_worker_input input = {0};
	const short adc_pcm[URP_NATIVE_WORKER_STEREO_SAMPLES] = {0};
	short app_pcm[URP_NATIVE_SAMPLES] = {1};
	short stereo[URP_NATIVE_WORKER_STEREO_SAMPLES] = {1};

	usbradioplus_native_worker_stats_reset(channel);
	native_worker_apply_requests(worker, NULL);
	native_worker_render_block(worker, &input, adc_pcm, app_pcm, stereo);
	return app_pcm[0] + stereo[0];
}

/** @brief Force the two transmitter graph failure fallbacks in a test build.
 * @param channel Channel with a deterministic native worker and prepared graphs.
 * @return Sum of output words after each fallback has silenced the corresponding block.
 */
int usbradioplus_native_worker_test_graph_failure_paths(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker = channel->plus_native_worker;
	struct usbradioplus_native_graph_set *graphs = usbradioplus_native_graphs_acquire(channel);
	struct native_worker_input input = {.graphs = graphs};
	const short adc_pcm[URP_NATIVE_WORKER_STEREO_SAMPLES] = {0};
	short app_pcm[URP_NATIVE_SAMPLES] = {0};
	short stereo[URP_NATIVE_WORKER_STEREO_SAMPLES] = {0};
	int dcs_configured = graphs->dcs.configured;
	int final_configured = graphs->final.configured;
	int legacy_interface = graphs->legacy_interface;
	int echo_mode = graphs->echo_mode;
	int software_repeat_enabled = graphs->software_repeat_enabled;
	double *parrot_audio = worker->parrot.audio;
	int result;

	graphs->dcs.configured = 0;
	native_worker_render_block(worker, &input, adc_pcm, app_pcm, stereo);
	result = stereo[0];
	graphs->dcs.configured = dcs_configured;
	graphs->final.configured = 0;
	native_worker_render_block(worker, &input, adc_pcm, app_pcm, stereo);
	result += stereo[0];
	graphs->final.configured = final_configured;
	/* Missing recording storage cannot occur after a successful worker start,
	 * but must not bypass the repeat frame when a future partial setup fails. */
	graphs->legacy_interface = 1;
	graphs->echo_mode = 1;
	graphs->software_repeat_enabled = 1;
	worker->parrot.audio = NULL;
	input.rxkeyed = 1;
	native_worker_render_block(worker, &input, adc_pcm, app_pcm, stereo);
	worker->parrot.audio = parrot_audio;
	graphs->legacy_interface = legacy_interface;
	graphs->echo_mode = echo_mode;
	graphs->software_repeat_enabled = software_repeat_enabled;
	usbradioplus_native_graphs_release(channel);
	return result;
}

/** @brief Push one raw test input record without waiting for the audio callback.
 * @param worker Deterministic native worker receiving the synthetic record.
 * @param graphs Graph reference paired with the record, if any.
 */
static void native_worker_test_push_input(struct usbradioplus_native_worker *worker,
					  struct usbradioplus_native_graph_set *graphs)
{
	unsigned int write = atomic_load_explicit(&worker->input_write, memory_order_relaxed);
	struct native_worker_input *input = &worker->input[write % URP_NATIVE_WORKER_QUEUE_FRAMES];
	unsigned int index;

	memset(input, 0, sizeof(*input));
	input->graphs = graphs;
	for (index = 0U; index < URP_NATIVE_WORKER_STEREO_SAMPLES; ++index)
		(void)urp_sample_queue_push_sample(&worker->input_pcm, 0);
	atomic_store_explicit(&worker->input_write, write + 1U, memory_order_release);
}

/** @brief Exercise malformed input-record rejection without a callback race.
 * @param channel Channel with a deterministic native worker and prepared graphs.
 * @return Number of input records left after malformed records were retired.
 */
unsigned int usbradioplus_native_worker_test_malformed_inputs(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker = channel->plus_native_worker;
	struct usbradioplus_native_graph_set *graphs;
	unsigned int app_samples;

	native_worker_test_push_input(worker, NULL);
	(void)native_worker_process_one(worker);
	graphs = usbradioplus_native_graphs_acquire(channel);
	app_samples = graphs->app_rpt_samples;
	graphs->app_rpt_samples = 0U;
	native_worker_test_push_input(worker, graphs);
	(void)native_worker_process_one(worker);
	graphs->app_rpt_samples = app_samples;
	graphs = usbradioplus_native_graphs_acquire(channel);
	graphs->app_rpt_samples = URP_NATIVE_SAMPLES + 1U;
	native_worker_test_push_input(worker, graphs);
	(void)native_worker_process_one(worker);
	graphs->app_rpt_samples = app_samples;
	return native_worker_control_count(&worker->input_read, &worker->input_write);
}

/** @brief Exercise callback submission overflow while no worker is scheduled.
 * @param channel Channel with a deterministic native worker and prepared graphs.
 * @return Number of input overflows counted by the callback.
 */
uint64_t usbradioplus_native_worker_test_input_overflow(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker = channel->plus_native_worker;

	native_worker_submit(worker);
	native_worker_submit(worker);
	native_worker_submit(worker);
	native_worker_submit(worker);
	usbradioplus_native_worker_test_process_all(channel);
	native_worker_consume(worker);
	native_worker_ack_tx(worker);
	native_worker_consume(worker);
	native_worker_ack_tx(worker);
	native_worker_consume(worker);
	native_worker_ack_tx(worker);
	return atomic_load_explicit(&worker->input_overflows, memory_order_relaxed);
}

/** @brief Reset deterministic TX queues between synthetic corruption cases.
 * @param worker Deterministic native worker whose callback-owned queues are idle.
 */
static void native_worker_test_reset_tx(struct usbradioplus_native_worker *worker)
{
	urp_sample_queue_reset(&worker->transmit_pcm);
	memset(worker->tx_output, 0, sizeof(worker->tx_output));
	atomic_store_explicit(&worker->tx_read, 0U, memory_order_release);
	atomic_store_explicit(&worker->tx_write, 0U, memory_order_release);
	worker->tx_staged = 0;
	worker->tx_staged_sequence = 0U;
}

/** @brief Exercise malformed TX token, short PCM, wrap, and acknowledgement paths.
 * @param channel Channel with a deterministic native worker.
 * @return Nonzero only if a corrupted record was incorrectly staged.
 */
int usbradioplus_native_worker_test_tx_guards(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker = channel->plus_native_worker;
	struct native_worker_tx_output *output;
	unsigned int wrap_read =
		worker->transmit_pcm.capacity - URP_NATIVE_WORKER_STEREO_SAMPLES + 1U;
	int staged;

	native_worker_test_reset_tx(worker);
	native_worker_reject_tx_output(worker, 0U);
	atomic_store_explicit(&worker->tx_write, 1U, memory_order_release);
	output = &worker->tx_output[0];
	output->sequence = 1U;
	output->valid = 1;
	native_worker_reject_tx_output(worker, 0U);
	native_worker_stage_tx(worker);
	staged = worker->tx_staged;

	native_worker_test_reset_tx(worker);
	atomic_store_explicit(&worker->tx_write, 1U, memory_order_release);
	output = &worker->tx_output[0];
	output->sequence = 0U;
	output->valid = 1;
	native_worker_stage_tx(worker);
	staged += worker->tx_staged;

	native_worker_test_reset_tx(worker);
	atomic_store_explicit(&worker->tx_write, 1U, memory_order_release);
	output = &worker->tx_output[0];
	output->sequence = 0U;
	output->valid = 1;
	atomic_store_explicit(&worker->transmit_pcm.read, wrap_read, memory_order_release);
	atomic_store_explicit(&worker->transmit_pcm.write,
			      wrap_read + URP_NATIVE_WORKER_STEREO_SAMPLES, memory_order_release);
	native_worker_stage_tx(worker);
	staged += worker->tx_staged;

	native_worker_test_reset_tx(worker);
	worker->tx_staged = 1;
	worker->tx_staged_sequence = 1U;
	atomic_store_explicit(&worker->tx_write, 1U, memory_order_release);
	native_worker_ack_tx(worker);
	worker->tx_staged = 1;
	worker->tx_staged_sequence = 0U;
	native_worker_test_reset_tx(worker);
	worker->tx_staged = 1;
	worker->tx_staged_sequence = 0U;
	native_worker_ack_tx(worker);

	native_worker_test_reset_tx(worker);
	atomic_store_explicit(&worker->tx_write, 1U, memory_order_release);
	worker->tx_staged = 1;
	output = &worker->tx_output[0];
	output->sequence = 1U;
	output->valid = 1;
	native_worker_ack_tx(worker);
	worker->tx_staged = 1;
	output->sequence = 0U;
	output->valid = 0;
	native_worker_ack_tx(worker);
	worker->tx_staged = 1;
	output->valid = 1;
	native_worker_ack_tx(worker);
	staged += worker->tx_staged;
	native_worker_test_reset_tx(worker);
	return staged;
}

/** @brief Reset every deterministic queue before an intentionally invalid pairing.
 * @param worker Worker whose test-only queue state is idle.
 */
static void native_worker_test_reset_queues(struct usbradioplus_native_worker *worker)
{
	urp_sample_queue_reset(&worker->input_pcm);
	urp_sample_queue_reset(&worker->receive_pcm);
	native_worker_test_reset_tx(worker);
	memset(worker->input, 0, sizeof(worker->input));
	memset(worker->output, 0, sizeof(worker->output));
	atomic_store_explicit(&worker->input_read, 0U, memory_order_release);
	atomic_store_explicit(&worker->input_write, 0U, memory_order_release);
	atomic_store_explicit(&worker->output_read, 0U, memory_order_release);
	atomic_store_explicit(&worker->output_write, 0U, memory_order_release);
}

/** @brief Exercise paired-SPSC corruption guards without a callback race.
 * @param channel Channel with an idle deterministic worker.
 * @return Sum of the four rejected synthetic output injections.
 *
 * Normal SPSC ownership keeps these metadata and PCM cursors paired.  The
 * harness creates only the otherwise impossible mismatch states, then restores
 * every queue before it returns.
 */
int usbradioplus_native_worker_test_queue_guards(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker = channel->plus_native_worker;
	struct native_worker_input snapshot;
	urp_radio_state *radio = channel->radio;
	void *rx_ctcss = radio->rxCtcss;
	size_t input_capacity = worker->input_pcm.capacity;
	size_t receive_capacity = worker->receive_pcm.capacity;
	size_t transmit_capacity = worker->transmit_pcm.capacity;
	int result = 0;

	native_worker_test_reset_queues(worker);
	channel->radio = NULL;
	native_worker_snapshot(&snapshot, channel, NULL);
	channel->radio = radio;
	radio->rxCtcss = NULL;
	native_worker_snapshot(&snapshot, channel, NULL);
	radio->rxCtcss = rx_ctcss;
	worker->channel = NULL;
	native_worker_submit(worker);
	result += native_worker_process_one(worker);
	worker->channel = channel;
	worker->input_pcm.capacity = URP_NATIVE_WORKER_STEREO_SAMPLES - 1U;
	native_worker_submit(worker);
	worker->input_pcm.capacity = input_capacity;

	native_worker_test_reset_queues(worker);
	atomic_store_explicit(&worker->input_write, 1U, memory_order_release);
	atomic_store_explicit(&worker->output_write, URP_NATIVE_WORKER_QUEUE_FRAMES,
			      memory_order_release);
	result += native_worker_process_one(worker);

	native_worker_test_reset_queues(worker);
	atomic_store_explicit(&worker->input_write, 1U, memory_order_release);
	atomic_store_explicit(&worker->tx_write, URP_NATIVE_WORKER_TX_QUEUE_FRAMES,
			      memory_order_release);
	result += native_worker_process_one(worker);

	native_worker_test_reset_queues(worker);
	atomic_store_explicit(&worker->input_write, 1U, memory_order_release);
	result += native_worker_process_one(worker);

	native_worker_test_reset_queues(worker);
	native_worker_test_push_input(worker, NULL);
	worker->receive_pcm.capacity = 0U;
	result += native_worker_process_one(worker);
	worker->receive_pcm.capacity = receive_capacity;

	native_worker_test_reset_queues(worker);
	native_worker_test_push_input(worker, NULL);
	worker->transmit_pcm.capacity = 0U;
	result += native_worker_process_one(worker);
	worker->transmit_pcm.capacity = transmit_capacity;

	native_worker_test_reset_queues(worker);
	atomic_store_explicit(&worker->output_write, URP_NATIVE_WORKER_QUEUE_FRAMES,
			      memory_order_release);
	result += usbradioplus_native_worker_test_inject_output(channel, URP_LINK_SAMPLES, 1U, 0);

	native_worker_test_reset_queues(worker);
	atomic_store_explicit(&worker->tx_write, URP_NATIVE_WORKER_TX_QUEUE_FRAMES,
			      memory_order_release);
	result += usbradioplus_native_worker_test_inject_output(channel, URP_LINK_SAMPLES, 1U, 0);

	native_worker_test_reset_queues(worker);
	worker->receive_pcm.capacity = 0U;
	result += usbradioplus_native_worker_test_inject_output(channel, URP_LINK_SAMPLES, 1U, 0);
	worker->receive_pcm.capacity = receive_capacity;

	native_worker_test_reset_queues(worker);
	worker->transmit_pcm.capacity = 0U;
	result += usbradioplus_native_worker_test_inject_output(channel, URP_LINK_SAMPLES, 1U, 0);
	worker->transmit_pcm.capacity = transmit_capacity;
	native_worker_test_reset_queues(worker);
	return result;
}

/** @brief Exercise worker teardown after a callback retains one graph reference.
 * @param channel Channel with a deterministic native worker and prepared graphs.
 * @return Nonzero when teardown detached the worker from the channel.
 *
 * Test builds do not create a real worker thread.  The linked pthread wrapper
 * accepts this sentinel so the normal joined-teardown branch can release a
 * callback-owned graph reference deterministically.
 */
int usbradioplus_native_worker_test_stop_started(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker = channel->plus_native_worker;

	native_worker_submit(worker);
	/* A retired graph record cannot arise from the paired producer, but teardown
	 * must still advance it without releasing a reference it does not own. */
	native_worker_test_push_input(worker, NULL);
	worker->started = 1;
	worker->thread = (pthread_t)(uintptr_t)1U;
	usbradioplus_native_worker_stop(channel);
	return channel->plus_native_worker == NULL;
}
#endif

int usbradioplus_native_worker_start(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker;

	if (!channel || channel->plus_native_worker)
		return channel ? 0 : -1;
	worker = ast_calloc(1, sizeof(*worker));
	if (!worker)
		return -1;
	worker->channel = channel;
	native_worker_statistics_init(worker);
	urp_sample_queue_init(&worker->input_pcm, worker->input_samples,
			      ARRAY_LEN(worker->input_samples));
	urp_sample_queue_init(&worker->receive_pcm, worker->receive_samples,
			      ARRAY_LEN(worker->receive_samples));
	urp_sample_queue_init(&worker->transmit_pcm, worker->transmit_samples,
			      ARRAY_LEN(worker->transmit_samples));
	atomic_init(&worker->stopping, 0);
	atomic_init(&worker->input_read, 0U);
	atomic_init(&worker->input_write, 0U);
	atomic_init(&worker->output_read, 0U);
	atomic_init(&worker->output_write, 0U);
	atomic_init(&worker->tx_read, 0U);
	atomic_init(&worker->tx_write, 0U);
	atomic_init(&worker->statistics_index, 0U);
	atomic_init(&worker->statistics_readers[0], 0U);
	atomic_init(&worker->statistics_readers[1], 0U);
	atomic_init(&worker->statistics_reset_request, 0U);
	atomic_init(&worker->parrot_clear_request, 0U);
	atomic_init(&worker->legacy_echo_clear_request, 0U);
	atomic_init(&worker->input_overflows, 0U);
	atomic_init(&worker->output_underflows, 0U);
	atomic_init(&worker->output_malformed, 0U);
	native_worker_publish_statistics(worker);
	urp_dcs_init(&worker->dcs);
	worker->dcs_code = -2;
	worker->dcs_inverted = -1;
	worker->echo_up = urp_src_create(SRC_SINC_BEST_QUALITY, 1);
	worker->down = urp_src_create(SRC_SINC_BEST_QUALITY, 1);
	worker->parrot.capacity = (size_t)DEFAULT_ECHO_MAX * URP_NATIVE_SAMPLES;
	worker->parrot.audio = ast_calloc(worker->parrot.capacity, sizeof(*worker->parrot.audio));
	txagc_rnnoise_init(&worker->local_rnnoise);
	if (!worker->echo_up || !worker->down || !worker->parrot.audio ||
	    urp_src_reserve(worker->echo_up, URP_NATIVE_SAMPLES, URP_NATIVE_SAMPLES) ||
	    urp_src_reserve(worker->down, URP_NATIVE_SAMPLES, URP_NATIVE_SAMPLES) ||
	    txagc_rnnoise_prepare(&worker->local_rnnoise, URP_RATE_NATIVE)) {
		txagc_rnnoise_destroy(&worker->local_rnnoise);
		urp_src_destroy(worker->echo_up);
		urp_src_destroy(worker->down);
		ast_free(worker->parrot.audio);
		ast_free(worker);
		return -1;
	}
	channel->plus_native_worker = worker;
#ifndef URP_PROCESSING_TESTING
	if (pthread_create(&worker->thread, NULL, native_worker_thread, worker)) {
		channel->plus_native_worker = NULL;
		txagc_rnnoise_destroy(&worker->local_rnnoise);
		urp_src_destroy(worker->echo_up);
		urp_src_destroy(worker->down);
		ast_free(worker->parrot.audio);
		ast_free(worker);
		return -1;
	}
	worker->started = 1;
#endif
	return 0;
}

void usbradioplus_native_worker_stop(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker;
	unsigned int read;

	if (!channel || !(worker = channel->plus_native_worker))
		return;
	atomic_store_explicit(&worker->stopping, 1, memory_order_release);
	if (worker->started)
		(void)pthread_join(worker->thread, NULL);
	/* Release graph references retained by a callback that raced teardown. */
	read = atomic_load_explicit(&worker->input_read, memory_order_relaxed);
	while (native_worker_control_count(&worker->input_read, &worker->input_write) != 0U) {
		const struct native_worker_input *input =
			&worker->input[read % URP_NATIVE_WORKER_QUEUE_FRAMES];

		if (input->graphs)
			usbradioplus_native_graphs_release(channel);
		++read;
		atomic_store_explicit(&worker->input_read, read, memory_order_release);
	}
	channel->plus_native_worker = NULL;
	txagc_rnnoise_destroy(&worker->local_rnnoise);
	urp_src_destroy(worker->echo_up);
	urp_src_destroy(worker->down);
	ast_free(worker->parrot.audio);
	ast_free(worker);
}

int usbradioplus_native_worker_stats_read(struct chan_usbradio_pvt *channel,
					  struct usbradioplus_native_worker_stats *statistics)
{
	struct usbradioplus_native_worker *worker;
	unsigned int attempt;

	if (!channel || !statistics || !(worker = channel->plus_native_worker))
		return -1;
	for (attempt = 0U; attempt < 3U; ++attempt) {
		unsigned int index =
			atomic_load_explicit(&worker->statistics_index, memory_order_acquire);

		atomic_fetch_add_explicit(&worker->statistics_readers[index], 1U,
					  memory_order_acquire);
#ifdef URP_PROCESSING_TESTING
		native_worker_test_invalidate_statistics_read(worker, index);
#endif
		if (index ==
		    atomic_load_explicit(&worker->statistics_index, memory_order_acquire)) {
			*statistics = worker->published_statistics[index].statistics;
			atomic_fetch_sub_explicit(&worker->statistics_readers[index], 1U,
						  memory_order_release);
			return 0;
		}
		atomic_fetch_sub_explicit(&worker->statistics_readers[index], 1U,
					  memory_order_release);
	}
	return -1;
}

int usbradioplus_native_worker_tx_audio_stats_read(struct chan_usbradio_pvt *channel,
						   struct audiostatistics *statistics)
{
	struct usbradioplus_native_worker *worker;
	unsigned int attempt;

	if (!channel || !statistics || !(worker = channel->plus_native_worker))
		return -1;
	for (attempt = 0U; attempt < 3U; ++attempt) {
		unsigned int index =
			atomic_load_explicit(&worker->statistics_index, memory_order_acquire);

		atomic_fetch_add_explicit(&worker->statistics_readers[index], 1U,
					  memory_order_acquire);
#ifdef URP_PROCESSING_TESTING
		native_worker_test_invalidate_statistics_read(worker, index);
#endif
		if (index ==
		    atomic_load_explicit(&worker->statistics_index, memory_order_acquire)) {
			*statistics = worker->published_statistics[index].tx_audio_statistics;
			atomic_fetch_sub_explicit(&worker->statistics_readers[index], 1U,
						  memory_order_release);
			return 0;
		}
		atomic_fetch_sub_explicit(&worker->statistics_readers[index], 1U,
					  memory_order_release);
	}
	return -1;
}

#ifdef URP_PROCESSING_TESTING
/** @brief Force a bounded worker-statistics reader to retry until it fails.
 * @param channel Channel with a deterministic native worker.
 * @return Public reader result after three deliberately invalidated attempts.
 */
int usbradioplus_native_worker_test_statistics_retry(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker = channel->plus_native_worker;
	struct usbradioplus_native_worker_stats statistics;
	unsigned int index = atomic_load_explicit(&worker->statistics_index, memory_order_acquire);
	int result;

	atomic_store_explicit(&native_worker_test_statistics_retry_count, 3U, memory_order_release);
	result = usbradioplus_native_worker_stats_read(channel, &statistics);
	atomic_store_explicit(&native_worker_test_statistics_retry_count, 0U, memory_order_release);
	atomic_store_explicit(&worker->statistics_index, index, memory_order_release);
	return result;
}

/** @brief Force a bounded transmitter-meter reader to retry until it fails.
 * @param channel Channel with a deterministic native worker.
 * @return Public reader result after three deliberately invalidated attempts.
 */
int usbradioplus_native_worker_test_tx_audio_statistics_retry(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker = channel->plus_native_worker;
	struct audiostatistics statistics;
	unsigned int index = atomic_load_explicit(&worker->statistics_index, memory_order_acquire);
	int result;

	atomic_store_explicit(&native_worker_test_statistics_retry_count, 3U, memory_order_release);
	result = usbradioplus_native_worker_tx_audio_stats_read(channel, &statistics);
	atomic_store_explicit(&native_worker_test_statistics_retry_count, 0U, memory_order_release);
	atomic_store_explicit(&worker->statistics_index, index, memory_order_release);
	return result;
}
#endif

void usbradioplus_native_worker_stats_reset(struct chan_usbradio_pvt *channel)
{
	if (channel && channel->plus_native_worker)
		atomic_fetch_add_explicit(&channel->plus_native_worker->statistics_reset_request,
					  1U, memory_order_release);
}

void usbradioplus_native_worker_clear_parrot(struct chan_usbradio_pvt *channel)
{
	if (channel && channel->plus_native_worker)
		atomic_fetch_add_explicit(&channel->plus_native_worker->parrot_clear_request, 1U,
					  memory_order_release);
}

void usbradioplus_native_worker_clear_legacy_echo(struct chan_usbradio_pvt *channel)
{
	if (!channel)
		return;
	if (channel->plus_native_worker) {
		atomic_fetch_add_explicit(&channel->plus_native_worker->legacy_echo_clear_request,
					  1U, memory_order_release);
		return;
	}
	/* No worker can own the consumer endpoint before startup or after teardown. */
	urp_sample_queue_discard(&channel->echo_queue);
}

void usbradioplus_native_tx_output_ack(struct chan_usbradio_pvt *channel)
{
	if (channel && channel->plus_native_worker)
		native_worker_ack_tx(channel->plus_native_worker);
}

/* Shared native-rate callback entry used by both hardware adapters. */
void usbradioplus_native_tick(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_worker *worker;

	if (!channel || !(worker = channel->plus_native_worker)) {
		if (channel) {
			native_worker_silence(channel);
			usbradioplus_publish_hardware_ptt(
				channel, channel->radio ? channel->radio->txPttOut : 0);
		}
		return;
	}
	native_worker_submit(worker);
	native_worker_consume(worker);
	/* PTT remains a signaling-engine decision.  A worker shortfall may mute the
	 * DAC frame, but it must never unkey a transmitter or alter its tail state. */
	usbradioplus_publish_hardware_ptt(channel, channel->radio ? channel->radio->txPttOut : 0);
}

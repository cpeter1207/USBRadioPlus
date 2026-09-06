/** @file
 * @brief Sample-rate conversion, elastic clock recovery, and receive-echo matching.
 */

#ifndef USBRADIOPLUS_DSP_H
#define USBRADIOPLUS_DSP_H

#include <stddef.h>
#include <stdint.h>

#define URP_APP_RPT_RATE_DEFAULT 8000

#define URP_RATE_LINK URP_APP_RPT_RATE_DEFAULT

#define URP_RATE_NATIVE 48000

#define URP_LINK_SAMPLES 160

#define URP_NATIVE_SAMPLES 960

#define URP_ECHO_HISTORY_FRAMES 32

#define URP_CLOCK_MAX_CORRECTION 0.005

/** Smoothed correction for independent app_rpt and USB audio clocks. */
struct urp_clock_recovery {
	/** Smoothed fractional rate correction around nominal conversion. */
	double correction;
};

/** @brief Reset the elastic-buffer clock correction to nominal rate.
 * @param clock Elastic clock-recovery state.
 */
void urp_clock_recovery_reset(struct urp_clock_recovery *clock);
/** @brief Slew the conversion ratio toward the FIFO target without abrupt frame corrections.
 * @param clock Elastic clock-recovery state.
 * @param queued_samples Current FIFO occupancy in samples.
 * @param target_samples Desired FIFO occupancy in samples.
 * @return Fractional rate correction bounded by URP_CLOCK_MAX_CORRECTION.
 */
double urp_clock_recovery_update(struct urp_clock_recovery *clock, size_t queued_samples,
				 size_t target_samples);

struct urp_src;

/** Paired app_rpt-rate and native-rate receive blocks for echo correlation. */
struct urp_echo_frame {
	/** App_rpt-rate receiver PCM for this history frame. */
	int16_t link[URP_LINK_SAMPLES];
	/** Corresponding native-rate receiver PCM. */
	int16_t native[URP_NATIVE_SAMPLES];
	/** Monotonic receive-frame sequence number. */
	uint64_t sequence;
};

/** Bounded receive history used to identify local audio in the app_rpt mix. */
struct urp_echo_replacer {
	/** Paired receive frames retained for correlation. */
	struct urp_echo_frame history[URP_ECHO_HISTORY_FRAMES];
	/** Next receive-history slot to replace. */
	unsigned int write_index;
	/** Monotonic receive-frame sequence number. */
	uint64_t sequence;
	/** Delay of the latest matched echo in app_rpt frames. */
	int last_delay_frames;
	/** Linear gain fitted to the latest matched receive echo. */
	double last_scale;
	/** Normalized correlation of the latest echo candidate. */
	double last_correlation;
	/** Number of successfully matched receive echoes. */
	uint64_t matches;
	/** Number of echo searches without a usable match. */
	uint64_t misses;
};

/** @brief Allocate a streaming libsamplerate converter and reusable conversion workspace.
 * @param converter libsamplerate converter type.
 * @param channels Number of interleaved audio channels.
 * @return Owned converter state, or NULL on invalid arguments or allocation failure.
 */
struct urp_src *urp_src_create(int converter, unsigned int channels);
/** @brief Release a converter and its input and output buffers.
 * @param src Streaming sample-rate converter.
 */
void urp_src_destroy(struct urp_src *src);
/** @brief Clear resampler history at a stream discontinuity.
 * @param src Streaming sample-rate converter.
 */
void urp_src_reset(struct urp_src *src);
/** @brief Convert an interleaved PCM block using a caller-supplied rate ratio.
 * @param src Streaming sample-rate converter.
 * @param input Input samples; the caller retains ownership.
 * @param input_count Number of input samples available.
 * @param output Destination sample buffer owned by the caller.
 * @param output_capacity Number of samples the output buffer can hold.
 * @param ratio Output/input sample-rate ratio.
 * @param input_used Receives the number of input samples consumed.
 * @param output_generated Receives the number of output samples produced.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int urp_src_process(struct urp_src *src, const int16_t *input, size_t input_count, int16_t *output,
		    size_t output_capacity, double ratio, size_t *input_used,
		    size_t *output_generated);
/** @brief Convert between sample rates, copying directly when the rates match.
 * @param src Streaming sample-rate converter.
 * @param input Input samples; the caller retains ownership.
 * @param input_count Number of input samples available.
 * @param input_rate Input sample rate in Hz.
 * @param output Destination sample buffer owned by the caller.
 * @param output_capacity Number of samples the output buffer can hold.
 * @param output_rate Output sample rate in Hz.
 * @param input_used Receives the number of input samples consumed.
 * @param output_generated Receives the number of output samples produced.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int urp_rate_convert(struct urp_src *src, const int16_t *input, size_t input_count,
		     unsigned int input_rate, int16_t *output, size_t output_capacity,
		     unsigned int output_rate, size_t *input_used, size_t *output_generated);

/** @brief Extract one channel from interleaved stereo PCM.
 * @param stereo Interleaved signed 16-bit stereo samples.
 * @param mono Mono sample buffer.
 * @param frames Number of audio frames.
 * @param channel Interleaved stereo channel index: 0 or 1.
 */
void urp_extract_mono(const int16_t *stereo, int16_t *mono, size_t frames, unsigned int channel);
/** @brief Route mono PCM to both stereo outputs with separate linear gains.
 * @param mono Mono sample buffer.
 * @param stereo Interleaved signed 16-bit stereo samples.
 * @param frames Number of audio frames.
 * @param gain_a Linear gain for output A.
 * @param gain_b Linear gain for output B.
 */
void urp_duplicate_mono(const int16_t *mono, int16_t *stereo, size_t frames, double gain_a,
			double gain_b);

/** @brief Clear the paired app_rpt/native receive history used for echo matching.
 * @param state Processor or stream state owned by the caller.
 */
void urp_echo_init(struct urp_echo_replacer *state);
/** @brief Store corresponding app_rpt and native receive frames in the echo history.
 * @param state Processor or stream state owned by the caller.
 * @param link One app_rpt-rate receive frame.
 * @param native Corresponding native-rate receive frame.
 */
void urp_echo_push(struct urp_echo_replacer *state, const int16_t *link, const int16_t *native);
/** @brief Subtract a correlated receive echo and return its matching native-rate frame.
 * @param state Processor or stream state owned by the caller.
 * @param mixed_link App_rpt mix from which correlated local audio is removed.
 * @param matched_native Receives the native frame corresponding to the removed echo.
 * @param minimum_correlation Minimum normalized correlation accepted as an echo match.
 * @return One when a qualifying echo was removed; zero when no match was usable.
 */
int urp_echo_remove(struct urp_echo_replacer *state, int16_t *mixed_link, int16_t *matched_native,
		    double minimum_correlation);

#endif

/** @name File-local and build-time constants
 * @{ */
/** @def URP_APP_RPT_RATE_DEFAULT
 * @brief Initial app_rpt sample rate in Hz.
 */
/** @def URP_RATE_LINK
 * @brief Reference app_rpt sample rate in Hz.
 */
/** @def URP_RATE_NATIVE
 * @brief CM119 native audio sample rate in Hz.
 */
/** @def URP_LINK_SAMPLES
 * @brief Samples in one reference app_rpt receive frame.
 */
/** @def URP_NATIVE_SAMPLES
 * @brief Samples in one native 20 ms audio frame.
 */
/** @def URP_ECHO_HISTORY_FRAMES
 * @brief Number of paired receive frames retained for correlation.
 */
/** @def URP_CLOCK_MAX_CORRECTION
 * @brief Maximum fractional elastic rate correction.
 */
/** @} */

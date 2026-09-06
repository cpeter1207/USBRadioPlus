/** @file
 * @brief Private RNNoise stream buffering and sample-conversion test interfaces.
 */

#ifndef TXAGC_RNNOISE_PROCESSOR_INTERNAL_H
#define TXAGC_RNNOISE_PROCESSOR_INTERNAL_H

#include "rnnoise_processor.h"

#ifdef URP_RNNOISE_TESTING
void reset_stream(struct txagc_rnnoise *state);
int configure_rate(struct txagc_rnnoise *state, unsigned int sample_rate);
int append(float *fifo, size_t *fifo_count, const float *data, size_t count);
void consume(float *fifo, size_t *fifo_count, size_t count);
int16_t pcm_from_double(double sample);
#endif

#endif

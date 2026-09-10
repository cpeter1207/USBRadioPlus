/* SPDX-License-Identifier: GPL-2.0-only */
/** @file
 * @brief Sample-at-a-time SPSC ring, conversion, and concealment tests.
 */
#include "rate_adjusting_pcm_ring.h"
#include <assert.h>
#include <limits.h>
#include <samplerate.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

enum src_test_mode {
  SRC_TEST_NORMAL,
  SRC_TEST_FAIL,
  SRC_TEST_NEGATIVE_INPUT,
  SRC_TEST_INVALID_INPUT,
  SRC_TEST_NEGATIVE_OUTPUT,
  SRC_TEST_INVALID_OUTPUT,
};

static unsigned int fail_calloc_call;
static unsigned int calloc_calls;
static bool fail_src_new;
static bool force_src_new_error;
static enum src_test_mode src_test_mode;
static long processed_input_frames;
static float captured_input[1024];
static long captured_input_frames;

void *__real_calloc(size_t count, size_t size);
SRC_STATE *__real_src_new(int converter_type, int channels, int *error);
int __real_src_process(SRC_STATE *state, SRC_DATA *data);

void *__wrap_calloc(size_t count, size_t size) {
  ++calloc_calls;
  return calloc_calls == fail_calloc_call ? NULL : __real_calloc(count, size);
}

SRC_STATE *__wrap_src_new(int converter_type, int channels, int *error) {
  if (fail_src_new) {
    *error = 1;
    return NULL;
  }
  SRC_STATE *state = __real_src_new(converter_type, channels, error);
  if (force_src_new_error)
    *error = 1;
  return state;
}

int __wrap_src_process(SRC_STATE *state, SRC_DATA *data) {
  processed_input_frames = data->input_frames;
  captured_input_frames = data->input_frames;
  assert(data->input_frames <=
         (long)(sizeof(captured_input) / sizeof(captured_input[0])));
  for (long index = 0; index < data->input_frames; ++index)
    captured_input[index] = data->data_in[index];
  if (src_test_mode == SRC_TEST_FAIL)
    return 1;
  int status = __real_src_process(state, data);
  if (src_test_mode == SRC_TEST_NEGATIVE_INPUT)
    data->input_frames_used = -1;
  if (src_test_mode == SRC_TEST_INVALID_INPUT)
    data->input_frames_used = data->input_frames + 1;
  if (src_test_mode == SRC_TEST_NEGATIVE_OUTPUT)
    data->output_frames_gen = -1;
  if (src_test_mode == SRC_TEST_INVALID_OUTPUT)
    data->output_frames_gen = data->output_frames + 1;
  return status;
}

static void fill_pcm(int16_t *samples, size_t count) {
  for (size_t index = 0; index < count; ++index)
    samples[index] = (int16_t)((int)(index % 200U) * 300 - 30000);
}

static void seed_history(struct rpcr_ring *ring, size_t period) {
  for (size_t index = 0; index < ring->capacity; ++index)
    ring->history[index] = (int16_t)((index % period) * 300 - 12000);
  ring->history_length = ring->capacity;
  ring->history_next = 0;
}

static bool render_until_real(struct rpcr_ring *ring, size_t target,
                              size_t attempts) {
  int16_t output = 0;
  for (size_t attempt = 0; attempt < attempts; ++attempt) {
    if (rpcr_consumer_render_sample(ring, &output, target))
      return true;
  }
  return false;
}

static void test_initialization_and_configuration(void) {
  struct rpcr_ring ring;
  struct rpcr_ring zero = {0};
  rpcr_destroy(NULL);
  rpcr_destroy(&zero);
  assert(rpcr_init(NULL, 1024, RPCR_SINC_BEST) != 0);
  assert(rpcr_init(&ring, 0, RPCR_SINC_BEST) != 0);
  assert(rpcr_init(&ring, 1, (enum rpcr_quality)99) != 0);
  for (unsigned int attempt = 1; attempt <= 4; ++attempt) {
    fail_calloc_call = attempt;
    calloc_calls = 0;
    assert(rpcr_init(&ring, 1024, RPCR_SINC_BEST) != 0);
  }
  fail_calloc_call = 0;
  fail_src_new = true;
  assert(rpcr_init(&ring, 1024, RPCR_SINC_BEST) != 0);
  fail_src_new = false;
  force_src_new_error = true;
  assert(rpcr_init(&ring, 1024, RPCR_SINC_BEST) != 0);
  force_src_new_error = false;
  assert(rpcr_init(&ring, 1024, RPCR_SINC_MEDIUM) == 0);
  rpcr_destroy(&ring);
  assert(rpcr_init(&ring, 1024, RPCR_SINC_FASTEST) == 0);
  rpcr_destroy(&ring);
  assert(rpcr_set_sample_rate(NULL, 8000) != 0);
  assert(rpcr_set_rates(&zero, 8000, 48000) != 0);
  assert(rpcr_init(&ring, 1024, RPCR_SINC_BEST) == 0);
  assert(rpcr_set_sample_rate(&ring, 0) != 0);
  assert(rpcr_set_rates(NULL, 8000, 48000) != 0);
  assert(rpcr_set_rates(&ring, 0, 48000) != 0);
  assert(rpcr_set_rates(&ring, 8000, 0) != 0);
  ring.input_pending = 1;
  ring.output_pending = 1;
  ring.plc_period = ring.plc_samples = ring.recovery_samples = 1;
  assert(rpcr_set_sample_rate(&ring, 8000) == 0);
  assert(ring.input_rate == 8000 && ring.output_rate == 8000);
  assert(!ring.input_pending && !ring.output_pending && !ring.plc_period &&
         !ring.plc_samples && !ring.recovery_samples);
  assert(rpcr_available(NULL) == 0);
  assert(rpcr_available(&ring) == 0);
  struct rpcr_observation observation = {0};
  rpcr_observe(NULL, &observation);
  assert(!observation.capacity_samples && !observation.available_samples &&
         !observation.ratio_correction_ppm);
  rpcr_observe(&ring, NULL);
  rpcr_observe(&ring, &observation);
  assert(observation.capacity_samples == ring.capacity &&
         !observation.available_samples && !observation.target_samples);
  rpcr_destroy(&ring);
}

static void test_sample_spsc_and_wrap(void) {
  struct rpcr_ring ring;
  struct rpcr_ring zero = {0};
  const int16_t bulk[] = {7, 8, 9, 10, 11};
  int16_t sample = 0;
  int16_t one_storage = 0;
  assert(!rpcr_producer_push_sample(NULL, 1));
  assert(!rpcr_consumer_pop_sample(NULL, &sample));
  assert(!rpcr_producer_push_sample(&zero, 1));
  assert(!rpcr_consumer_pop_sample(&zero, &sample));
  zero.storage = &one_storage;
  assert(!rpcr_producer_push_sample(&zero, 1));
  assert(!rpcr_consumer_pop_sample(&zero, &sample));
  zero.storage = NULL;
  assert(!rpcr_available(&zero));
  assert(rpcr_init(&ring, 4, RPCR_SINC_BEST) == 0);
  assert(!rpcr_consumer_pop_sample(&ring, NULL));
  assert(!rpcr_consumer_pop_sample(&ring, &sample));
  for (int16_t value = 1; value <= 4; ++value)
    assert(rpcr_producer_push_sample(&ring, value));
  assert(!rpcr_producer_push_sample(&ring, 5));
  assert(atomic_load(&ring.discarded) == 1);
  assert(rpcr_available(&ring) == 4);
  assert(rpcr_consumer_pop_sample(&ring, &sample) && sample == 1);
  assert(rpcr_consumer_pop_sample(&ring, &sample) && sample == 2);
  assert(rpcr_producer_push_sample(&ring, 5));
  assert(rpcr_producer_push_sample(&ring, 6));
  for (int16_t value = 3; value <= 6; ++value)
    assert(rpcr_consumer_pop_sample(&ring, &sample) && sample == value);
  assert(!rpcr_consumer_pop_sample(&ring, &sample));
  atomic_store(&ring.written, 10);
  atomic_store(&ring.read, 0);
  assert(rpcr_available(&ring) == ring.capacity);
  assert(!rpcr_consumer_pop_sample(&ring, &sample));
  atomic_store(&ring.written, 0);
  atomic_store(&ring.read, 0);
  rpcr_write(NULL, bulk, 1);
  rpcr_write(&ring, NULL, 1);
  rpcr_write(&ring, bulk, sizeof(bulk) / sizeof(bulk[0]));
  assert(rpcr_available(&ring) == ring.capacity);
  assert(atomic_load(&ring.discarded) == 2);
  for (int16_t value = 7; value <= 10; ++value)
    assert(rpcr_consumer_pop_sample(&ring, &sample) && sample == value);
  rpcr_destroy(&ring);
}

static void test_no_priming_and_compatibility(void) {
  struct rpcr_ring ring;
  int16_t input[1024];
  int16_t output[1024];
  fill_pcm(input, sizeof(input) / sizeof(input[0]));
  assert(rpcr_init(&ring, 1024, RPCR_SINC_BEST) == 0);
  assert(rpcr_set_sample_rate(&ring, 8000) == 0);
  rpcr_write(&ring, input, sizeof(input) / sizeof(input[0]));
  processed_input_frames = 0;
  (void)rpcr_consumer_render_sample(&ring, output, 50000);
  assert(processed_input_frames > 0);
  assert(atomic_load(&ring.read) > 0);
  assert(render_until_real(&ring, 50000, 4096));
  struct rpcr_observation observation = {0};
  rpcr_observe(&ring, &observation);
  assert(observation.target_samples == 50000 &&
         observation.filtered_occupancy_samples);
  rpcr_destroy(&ring);

  assert(rpcr_init(&ring, 1024, RPCR_SINC_BEST) == 0);
  assert(rpcr_set_sample_rate(&ring, 8000) == 0);
  rpcr_write(&ring, input, sizeof(input) / sizeof(input[0]));
  assert(!rpcr_render(NULL, output, 1, 0, 1));
  assert(!rpcr_render(&ring, NULL, 1, 0, 1));
  assert(!rpcr_render(&ring, output, 0, 123, 456));
  rpcr_observe(&ring, &observation);
  assert(observation.reserve_samples == 123 &&
         observation.target_samples == 456);
  (void)rpcr_render(&ring, output, sizeof(output) / sizeof(output[0]),
                    ring.capacity, 100000);
  assert(atomic_load(&ring.read) > 0);
  rpcr_observe(&ring, &observation);
  assert(observation.reserve_samples == ring.capacity &&
         observation.target_samples == 100000);
  rpcr_destroy(&ring);
}

static void test_conversion_and_clock_correction(void) {
  struct rpcr_ring ring;
  int16_t input[2048];
  int16_t output = 0;
  fill_pcm(input, sizeof(input) / sizeof(input[0]));
  assert(rpcr_init(&ring, 1024, RPCR_SINC_BEST) == 0);
  assert(rpcr_set_rates(&ring, 8000, 48000) == 0);
  rpcr_write(&ring, input, 1024);
  processed_input_frames = 0;
  (void)rpcr_consumer_render_sample(&ring, &output, 0);
  assert(processed_input_frames > 0 && processed_input_frames <= 256);
  assert(captured_input_frames == processed_input_frames);
  assert(captured_input[0] == (float)input[0] / 32768.0F);
  assert(ring.ratio > 5.99 && ring.ratio < 6.01);
  assert(render_until_real(&ring, 480, 4096));
  ring.output_pending = 0;
  ring.ratio = 6.0;
  ring.occupancy_milli = (uint64_t)ring.capacity * 3000U;
  (void)rpcr_consumer_render_sample(&ring, &output, 1);
  assert(atomic_load(&ring.ratio_correction_ppm) < 0);
  ring.output_pending = 0;
  ring.ratio = 6.0;
  ring.occupancy_milli = 1;
  (void)rpcr_consumer_render_sample(&ring, &output, 102400);
  assert(atomic_load(&ring.ratio_correction_ppm) > 0);
  rpcr_destroy(&ring);

  assert(rpcr_init(&ring, 8, RPCR_SINC_BEST) == 0);
  assert(rpcr_set_sample_rate(&ring, 8000) == 0);
  ring.input_offset = 4;
  ring.input_pending = 2;
  ring.input[4] = 0.25F;
  ring.input[5] = -0.25F;
  (void)rpcr_consumer_render_sample(&ring, &output, 1);
  assert(ring.input_offset == 0 || ring.input_offset < ring.capacity);
  rpcr_destroy(&ring);

  assert(rpcr_init(&ring, 1024, RPCR_SINC_BEST) == 0);
  assert(rpcr_set_sample_rate(&ring, 8000) == 0);
  ring.input_offset = 1;
  ring.input_pending = 1;
  ring.input[1] = 0.25F;
  (void)rpcr_consumer_render_sample(&ring, &output, 1);
  rpcr_destroy(&ring);
}

static void test_shortfall_statistics_and_plc(void) {
  struct rpcr_ring ring;
  int16_t output = 0;
  assert(!rpcr_consumer_render_sample(NULL, &output, 1));
  assert(rpcr_init(&ring, 1024, RPCR_SINC_BEST) == 0);
  assert(!rpcr_consumer_render_sample(&ring, NULL, 1));
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  assert(!output);
  ring.input_rate = 8000;
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  assert(rpcr_set_sample_rate(&ring, 8000) == 0);
  struct SRC_STATE_tag *converter = ring.converter;
  ring.converter = NULL;
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  ring.converter = converter;
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  assert(!output);
  ring.plc_period = ring.plc_samples = ring.recovery_samples = 0;
  seed_history(&ring, 80);
  assert(!rpcr_consumer_render_sample(&ring, &output, 480));
  assert(!rpcr_consumer_render_sample(&ring, &output, 480));
  assert(!rpcr_consumer_render_sample(&ring, &output, 480));
  assert(ring.plc_period == 80 && output != 0);
  assert(atomic_load(&ring.missing) == 7);
  assert(atomic_load(&ring.consecutive_underruns) == 7);
  for (size_t index = 0; index < 500; ++index)
    (void)rpcr_consumer_render_sample(&ring, &output, 480);
  assert(!output);
  size_t crossfade = 64;
  ring.output_pending = crossfade;
  ring.output_offset = 0;
  for (size_t index = 0; index < crossfade; ++index)
    ring.output[index] = 0.5F;
  for (size_t index = 0; index < crossfade; ++index)
    assert(rpcr_consumer_render_sample(&ring, &output, 480));
  assert(!ring.plc_samples && !ring.recovery_samples);
  assert(!atomic_load(&ring.consecutive_underruns));
  rpcr_record_shortfall(NULL, 1, 1, 8000);
  rpcr_record_shortfall(&ring, 2, 160, 8000);
  assert(atomic_load(&ring.consecutive_underruns) == 2);
  assert(atomic_load(&ring.underrun_average_milli) > 0);
  rpcr_record_shortfall(&ring, 0, 160, 8000);
  assert(!atomic_load(&ring.consecutive_underruns));
  rpcr_record_shortfall(&ring, 1, 90000, 8000);
  rpcr_record_shortfall(&ring, 1, 160, 0);
  rpcr_destroy(&ring);
}

static void test_plc_boundaries_and_pcm_saturation(void) {
  struct rpcr_ring ring;
  int16_t output = 0;
  assert(rpcr_init(&ring, 1024, RPCR_SINC_BEST) == 0);
  assert(rpcr_set_sample_rate(&ring, 1) == 0);
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  ring.plc_period = ring.plc_samples = 1;
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  ring.plc_period = ring.plc_samples = 1;
  ring.output_pending = 1;
  ring.output[0] = 0.5F;
  assert(rpcr_consumer_render_sample(&ring, &output, 1));
  assert(!ring.plc_samples);
  rpcr_destroy(&ring);

  assert(rpcr_init(&ring, 1024, RPCR_SINC_BEST) == 0);
  assert(rpcr_set_sample_rate(&ring, 8000) == 0);
  for (size_t index = 0; index < ring.capacity; ++index)
    ring.history[index] = INT16_MAX;
  ring.history_length = ring.capacity;
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  assert(output == INT16_MAX);
  for (size_t index = 0; index < ring.capacity; ++index)
    ring.history[index] = INT16_MIN;
  ring.history_next = 0;
  ring.plc_period = ring.plc_samples = 0;
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  assert(output == INT16_MIN);
  ring.plc_period = ring.plc_samples = ring.recovery_samples = 0;
  ring.output_pending = 3;
  ring.output_offset = 0;
  ring.output[0] = 1.25F;
  ring.output[1] = -1.25F;
  ring.output[2] = 0.5F;
  assert(rpcr_consumer_render_sample(&ring, &output, 1) && output == INT16_MAX);
  assert(rpcr_consumer_render_sample(&ring, &output, 1) && output == INT16_MIN);
  assert(rpcr_consumer_render_sample(&ring, &output, 1) && output == 16384);
  rpcr_destroy(&ring);

  assert(rpcr_init(&ring, 1024, RPCR_SINC_BEST) == 0);
  assert(rpcr_set_sample_rate(&ring, 96000) == 0);
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  rpcr_destroy(&ring);
}

static void test_converter_failures(void) {
  struct rpcr_ring ring;
  int16_t input[1024];
  int16_t output = 0;
  fill_pcm(input, sizeof(input) / sizeof(input[0]));
  assert(rpcr_init(&ring, 1024, RPCR_SINC_BEST) == 0);
  assert(rpcr_set_sample_rate(&ring, 8000) == 0);
  rpcr_write(&ring, input, sizeof(input) / sizeof(input[0]));
  src_test_mode = SRC_TEST_FAIL;
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  src_test_mode = SRC_TEST_NEGATIVE_INPUT;
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  src_test_mode = SRC_TEST_INVALID_INPUT;
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  src_test_mode = SRC_TEST_NEGATIVE_OUTPUT;
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  src_test_mode = SRC_TEST_INVALID_OUTPUT;
  assert(!rpcr_consumer_render_sample(&ring, &output, 1));
  src_test_mode = SRC_TEST_NORMAL;
  assert(render_until_real(&ring, 1, 4096));
  rpcr_destroy(&ring);
}

int main(void) {
  test_initialization_and_configuration();
  test_sample_spsc_and_wrap();
  test_no_priming_and_compatibility();
  test_conversion_and_clock_correction();
  test_shortfall_statistics_and_plc();
  test_plc_boundaries_and_pcm_saturation();
  test_converter_failures();
  puts("rate-adjusting PCM ring tests passed");
  return 0;
}

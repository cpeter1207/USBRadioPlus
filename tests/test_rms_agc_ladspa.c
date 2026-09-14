/* SPDX-License-Identifier: MIT */
/** @file
 * @brief Streaming, gain-law, safety and real-time invariants for the RMS LADSPA gain rider.
 */

#include "fixtures/rms_agc_ladspa.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** @brief Maximum samples processed in one fixture call. */
#define BLOCK 512
/** @brief Number of scalar ports. */
#define CONTROLS (USBRADIOPLUS_AGC_PORT_COUNT - USBRADIOPLUS_AGC_TARGET_DBFS)

/** @brief Frozen C implementation, linked into tests only.
 * @param index Plugin index.
 * @return Reference descriptor, or NULL for a nonzero index.
 */
const LADSPA_Descriptor *rms_agc_reference_descriptor(unsigned long index);

/** @brief Fail the next allocation to exercise host-visible initialization failure. */
static int allocation_failure;
/** @brief Number of allocations observed, also checked around real-time processing. */
static unsigned int allocations;

/** @brief Linker's allocation implementation.
 * @param count Element count.
 * @param size Element size.
 * @return Allocated zeroed storage or NULL.
 */
void *__real_calloc(size_t count, size_t size);

/** @brief Inject initialization failures and verify that run never allocates.
 * @param count Element count.
 * @param size Element size.
 * @return Allocated storage unless failure is requested.
 */
void *__wrap_calloc(size_t count, size_t size)
{
	allocations++;
	if (allocation_failure) {
		allocation_failure = 0;
		return NULL;
	}
	return __real_calloc(count, size);
}

/** @brief A plugin instance and all host-owned port buffers. */
struct fixture {
	const LADSPA_Descriptor *descriptor; /**< Plugin function table. */
	LADSPA_Handle handle;		     /**< Allocated streaming state. */
	unsigned long rate;		     /**< Samples per second. */
	float control[CONTROLS];	     /**< Explicit configuration in descriptor order. */
	float program[BLOCK];		     /**< Unfiltered program samples. */
	float detector[BLOCK];		     /**< Independent detector samples. */
	float output[BLOCK];		     /**< Processed samples. */
};

/** @brief Create a fully connected plugin using documented defaults.
 * @param fixture Fixture to initialize in place.
 * @param rate Sample rate.
 * @param descriptor Production or frozen-reference host interface.
 */
static void initialize_descriptor(struct fixture *fixture, unsigned long rate,
				  const LADSPA_Descriptor *descriptor)
{
	static const float defaults[CONTROLS] = {-24, 200, 2, 6, 6, 6, -50, 3, 500, 1};
	memset(fixture, 0, sizeof(*fixture));
	fixture->descriptor = descriptor;
	fixture->handle = fixture->descriptor->instantiate(fixture->descriptor, rate);
	assert(fixture->handle);
	fixture->rate = rate;
	memcpy(fixture->control, defaults, sizeof(defaults));
	fixture->descriptor->connect_port(fixture->handle, USBRADIOPLUS_AGC_INPUT,
					  fixture->program);
	fixture->descriptor->connect_port(fixture->handle, USBRADIOPLUS_AGC_DETECTOR,
					  fixture->detector);
	fixture->descriptor->connect_port(fixture->handle, USBRADIOPLUS_AGC_OUTPUT,
					  fixture->output);
	for (unsigned int index = 0; index < CONTROLS; ++index)
		fixture->descriptor->connect_port(fixture->handle,
						  index + USBRADIOPLUS_AGC_TARGET_DBFS,
						  fixture->control + index);
}

/** @brief Initialize the production Rust implementation with the published defaults.
 * @param fixture Fixture to initialize.
 * @param rate Sample rate.
 */
static void initialize(struct fixture *fixture, unsigned long rate)
{
	initialize_descriptor(fixture, rate, ladspa_descriptor(0));
}

/** @brief Set a scalar using its public port number.
 * @param fixture Test fixture.
 * @param port Public control port.
 * @param value Value to connect.
 */
static void control(struct fixture *fixture, enum usbradioplus_agc_port port, float value)
{
	fixture->control[port - USBRADIOPLUS_AGC_TARGET_DBFS] = value;
}

/** @brief Feed an exact number of samples, preserving leveler state across blocks.
 * @param fixture Test fixture.
 * @param samples Sample count.
 * @param detector Detector amplitude; alternating signs give exact constant RMS.
 * @param program Program amplitude, independent of the detector.
 * @return Last output sample divided by the positive program amplitude.
 */
static double drive(struct fixture *fixture, unsigned long samples, float detector, float program)
{
	double result = 0.0;
	unsigned int before = allocations;
	while (samples) {
		unsigned long count = samples < BLOCK ? samples : BLOCK;
		for (unsigned long index = 0; index < count; ++index) {
			fixture->program[index] = program;
			fixture->detector[index] = index % 2 ? detector : -detector;
		}
		fixture->descriptor->run(fixture->handle, count);
		result = (double)fixture->output[count - 1] / program;
		samples -= count;
	}
	assert(allocations == before);
	return result;
}

/** @brief Exercise descriptor metadata, allocation failures and disconnected ports. */
static void test_host_contract(void)
{
	const LADSPA_Descriptor *descriptor = ladspa_descriptor(0);
	LADSPA_Handle handle;
	float input[2] = {0.25F, -0.25F};
	float output[2] = {99, 99};
	assert(!ladspa_descriptor(1));
	assert(!strcmp(descriptor->Label, "usbradioplus_agc"));
	assert(descriptor->Properties & LADSPA_PROPERTY_HARD_RT_CAPABLE);
	assert(descriptor->PortCount == USBRADIOPLUS_AGC_PORT_COUNT);
	assert(!descriptor->instantiate(descriptor, 0));
	assert(!descriptor->instantiate(descriptor, 384001));
	allocation_failure = 1;
	/* Link-time calloc wrapping applies only to the frozen C reference. Rust's
	 * fallible allocation and allocation-free run are covered by its unit tests. */
	assert(!rms_agc_reference_descriptor(0)->instantiate(descriptor, 8000));
	handle = descriptor->instantiate(descriptor, 8000);
	assert(handle);
	descriptor->run(handle, 1);
	descriptor->connect_port(handle, USBRADIOPLUS_AGC_INPUT, input);
	descriptor->run(handle, 1);
	descriptor->connect_port(handle, USBRADIOPLUS_AGC_DETECTOR, input);
	descriptor->run(handle, 1);
	descriptor->connect_port(handle, USBRADIOPLUS_AGC_OUTPUT, output);
	descriptor->connect_port(handle, USBRADIOPLUS_AGC_PORT_COUNT, input);
	descriptor->run(handle, 0);
	descriptor->run(handle, 2);
	/* Initial program samples are returned immediately, not buffered for analysis. */
	assert(output[0] == input[0] && output[1] == input[1]);
	descriptor->activate(handle);
	descriptor->run(handle, 1);
	assert(output[0] == input[0]);
	descriptor->cleanup(handle);
}

/** @brief Verify hold, gain caps, downward rate and noise freezing at each audio rate.
 * @param rate Sample rate under test.
 */
static void test_gain_policy(unsigned long rate)
{
	struct fixture fixture;
	double gain;
	double frozen;
	initialize(&fixture, rate);
	/* An unreachable target still reaches the gain cap with a one-dB deadband. */
	control(&fixture, USBRADIOPLUS_AGC_DEADBAND_DB, 1);
	gain = drive(&fixture, rate / 4, 0.01F, 0.1F);
	assert(fabs(gain - 1.0) < 1e-6);
	gain = drive(&fixture, rate / 2, 0.01F, 0.1F);
	assert(20.0 * log10(gain) > 0.4 && 20.0 * log10(gain) < 0.6);
	gain = drive(&fixture, 4 * rate, 0.01F, 0.1F);
	assert(fabs(20.0 * log10(gain) - 6.0) < 0.001);
	/* Inactivity holds the last gain; it does not lift receiver noise between words. */
	frozen = drive(&fixture, rate, 0.0001F, 0.1F);
	gain = drive(&fixture, 2 * rate, 0.0001F, 0.1F);
	assert(gain == frozen);
	/* Strong activity can reduce gain immediately; it does not wait for the hold timer. */
	gain = drive(&fixture, rate / 2, 0.5F, 0.1F);
	assert(20.0 * log10(gain) < 3.2 && 20.0 * log10(gain) > 2.8);
	gain = drive(&fixture, 3 * rate, 0.5F, 0.1F);
	assert(fabs(20.0 * log10(gain) + 6.0) < 0.001);
	/* A tightened configured gain cap is honored, without clipping the program. */
	control(&fixture, USBRADIOPLUS_AGC_MAX_ATTENUATION_DB, 0);
	gain = drive(&fixture, 1, 0.5F, 2.0F);
	assert(gain == 1.0 && fixture.output[0] == 2.0F);
	fixture.descriptor->cleanup(fixture.handle);
}

/** @brief Verify detector independence, hysteresis, deadband and reacquisition protection. */
static void test_activity_and_detector(void)
{
	struct fixture fixture;
	double gain;
	double previous;
	initialize(&fixture, 48000);
	control(&fixture, USBRADIOPLUS_AGC_HOLD_MS, 0);
	control(&fixture, USBRADIOPLUS_AGC_MAX_BOOST_DB, 30);
	/* Loud program with quiet detector does not affect gain or invoke a limiter. */
	assert(drive(&fixture, 48000, 0.001F, 4.0F) == 1.0);
	previous = drive(&fixture, 24000, 0.006F, 0.1F);
	/* -51 dBFS remains active after opening at -44 dBFS with three dB hysteresis. */
	gain = drive(&fixture, 24000, 0.0028F, 0.1F);
	assert(gain > previous);
	previous = drive(&fixture, 48000, 0.001F, 0.1F);
	gain = drive(&fixture, 48000, 0.001F, 0.1F);
	assert(gain == previous);
	/* Reacquired loud audio must not request boost from the previous silent average. */
	gain = drive(&fixture, 4800, 0.5F, 0.1F);
	assert(gain < previous);
	fixture.descriptor->activate(fixture.handle);
	control(&fixture, USBRADIOPLUS_AGC_DEADBAND_DB, 1);
	gain = drive(&fixture, 3 * 48000, 0.063095734F, 0.1F);
	assert(fabs(20.0 * log10(gain)) < 0.1);
	fixture.descriptor->cleanup(fixture.handle);
}

/** @brief Verify sustained qualification resets during pauses and level reductions. */
static void test_hold_resets(void)
{
	struct fixture fixture;
	double before;
	double after;
	initialize(&fixture, 8000);
	for (unsigned int iteration = 0; iteration < 4; ++iteration) {
		assert(fabs(drive(&fixture, 2000, 0.01F, 0.1F) - 1.0) < 1e-6);
		assert(fabs(drive(&fixture, 4000, 0.0001F, 0.1F) - 1.0) < 1e-6);
	}
	before = drive(&fixture, 8000, 0.5F, 0.1F);
	/* Fast RMS must fall and qualify anew, rather than reusing a previous hold. */
	after = drive(&fixture, 2000, 0.01F, 0.1F);
	assert(after <= before);
	fixture.descriptor->cleanup(fixture.handle);
}

/** @brief Ensure malformed controls/audio cannot poison subsequent processing. */
static void test_invalid_and_runtime_controls(void)
{
	struct fixture fixture;
	double gain;
	initialize(&fixture, 1000);
	for (unsigned int index = 0; index < CONTROLS; ++index)
		fixture.control[index] = NAN;
	assert(isfinite(drive(&fixture, 1000, 0.1F, 0.1F)));
	for (unsigned int index = 0; index < CONTROLS; ++index)
		fixture.control[index] = -FLT_MAX;
	assert(isfinite(drive(&fixture, 1000, 0.1F, 0.1F)));
	for (unsigned int index = 0; index < CONTROLS; ++index)
		fixture.control[index] = FLT_MAX;
	assert(isfinite(drive(&fixture, 1000, 0.1F, 0.1F)));
	fixture.program[0] = NAN;
	fixture.program[1] = INFINITY;
	fixture.detector[0] = NAN;
	fixture.detector[1] = -INFINITY;
	fixture.descriptor->run(fixture.handle, 2);
	assert(fixture.output[0] == 0 && fixture.output[1] == 0);
	fixture.descriptor->cleanup(fixture.handle);
	initialize(&fixture, 8000);
	control(&fixture, USBRADIOPLUS_AGC_HOLD_MS, 0);
	gain = drive(&fixture, 16000, 0.01F, 0.1F);
	assert(gain > 1.0);
	fixture.program[0] = FLT_MAX;
	fixture.detector[0] = 0.01F;
	fixture.descriptor->run(fixture.handle, 1);
	assert(fixture.output[0] == 0);
	assert(isfinite(drive(&fixture, 8000, FLT_MAX, 0.1F)));
	control(&fixture, USBRADIOPLUS_AGC_MAX_BOOST_DB, 0);
	control(&fixture, USBRADIOPLUS_AGC_MAX_ATTENUATION_DB, 0);
	assert(fabs(drive(&fixture, 8000, 0.01F, 0.1F) - 1.0) < 1e-6);
	fixture.descriptor->cleanup(fixture.handle);
}

/** @brief Exact output must not depend on host block boundaries, including odd rates. */
static void test_block_independence(void)
{
	static const unsigned long rates[] = {8000, 16000, 44100, 48000};
	for (unsigned int rate_index = 0; rate_index < sizeof(rates) / sizeof(rates[0]);
	     ++rate_index) {
		struct fixture bulk;
		struct fixture single;
		initialize(&bulk, rates[rate_index]);
		initialize(&single, rates[rate_index]);
		/* LADSPA allows input/output aliasing unless the plugin advertises otherwise. */
		single.descriptor->connect_port(single.handle, USBRADIOPLUS_AGC_OUTPUT,
						single.program);
		control(&bulk, USBRADIOPLUS_AGC_HOLD_MS, 0);
		control(&single, USBRADIOPLUS_AGC_HOLD_MS, 0);
		for (unsigned long block = 0; block < 400; ++block) {
			for (unsigned long index = 0; index < BLOCK; ++index) {
				bulk.program[index] =
					(float)(0.2 * sin((double)(block * BLOCK + index) * 0.07));
				bulk.detector[index] =
					bulk.program[index] * (block < 100 ? 0.1F : 1.0F);
			}
			bulk.descriptor->run(bulk.handle, BLOCK);
			for (unsigned long index = 0; index < BLOCK; ++index) {
				single.program[0] = bulk.program[index];
				single.detector[0] = bulk.detector[index];
				single.descriptor->run(single.handle, 1);
				assert(bulk.output[index] == single.program[0]);
			}
		}
		bulk.descriptor->cleanup(bulk.handle);
		single.descriptor->cleanup(single.handle);
	}
}

/** @brief Report a conservative processing cost including host buffer preparation. */
static void benchmark_processing(void)
{
	struct fixture fixture;
	clock_t start;
	double seconds;
	initialize(&fixture, 48000);
	start = clock();
	(void)drive(&fixture, 4800000, 0.01F, 0.1F);
	seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
	printf("RMS AGC: 100 seconds at 48 kHz processed in %.3f CPU seconds\n", seconds);
	start = clock();
	(void)drive(&fixture, 4800000, 0.0F, 0.1F);
	seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
	printf("RMS AGC: 100 seconds of silence processed in %.3f CPU seconds\n", seconds);
	fixture.descriptor->cleanup(fixture.handle);
}

/** @brief Compare every public descriptor field and streamed output to the frozen C plugin. */
static void test_reference_parity(void)
{
	static const unsigned long rates[] = {1000, 1001, 8000, 16000, 44100, 48000, 384000};
	static const float signals[] = {0.0F,  -0.0F,	     0.0001F,  0.0028F, 0.006F,
					0.01F, 0.063095734F, 0.5F,     2.0F,	FLT_MAX,
					NAN,   INFINITY,     -INFINITY};
	const LADSPA_Descriptor *rust = ladspa_descriptor(0);
	const LADSPA_Descriptor *reference = rms_agc_reference_descriptor(0);
	unsigned long compared = 0, rounded = 0;
	double maximum_error = 0.0;
	assert(rust->UniqueID == reference->UniqueID);
	assert(rust->Properties == reference->Properties);
	assert(rust->PortCount == reference->PortCount);
	assert(!strcmp(rust->Label, reference->Label));
	assert(!strcmp(rust->Name, reference->Name));
	assert(!strcmp(rust->Maker, reference->Maker));
	assert(!strcmp(rust->Copyright, reference->Copyright));
	assert(!rust->ImplementationData && !rust->run_adding && !rust->set_run_adding_gain &&
	       !rust->deactivate);
	for (unsigned long port = 0; port < rust->PortCount; ++port) {
		assert(rust->PortDescriptors[port] == reference->PortDescriptors[port]);
		assert(!strcmp(rust->PortNames[port], reference->PortNames[port]));
		assert(rust->PortRangeHints[port].HintDescriptor ==
		       reference->PortRangeHints[port].HintDescriptor);
		assert(!memcmp(&rust->PortRangeHints[port].LowerBound,
			       &reference->PortRangeHints[port].LowerBound, sizeof(float)));
		assert(!memcmp(&rust->PortRangeHints[port].UpperBound,
			       &reference->PortRangeHints[port].UpperBound, sizeof(float)));
	}
	for (unsigned int rate_index = 0; rate_index < sizeof(rates) / sizeof(rates[0]);
	     ++rate_index) {
		struct fixture actual, expected;
		initialize_descriptor(&actual, rates[rate_index], rust);
		initialize_descriptor(&expected, rates[rate_index], reference);
		for (unsigned int phase = 0; phase < 20; ++phase) {
			if (phase == 4) {
				actual.control[8] = expected.control[8] = 0;
			} else if (phase >= 8 && phase < 13) {
				static const float values[] = {NAN, -FLT_MAX, FLT_MAX, 0.0F, -0.0F};
				for (unsigned int control_index = 0; control_index < CONTROLS;
				     ++control_index)
					actual.control[control_index] =
						expected.control[control_index] = values[phase - 8];
			} else if (phase == 13) {
				for (unsigned long port = 3; port < rust->PortCount; ++port) {
					rust->connect_port(actual.handle, port, NULL);
					reference->connect_port(expected.handle, port, NULL);
				}
			} else if (phase == 14) {
				for (unsigned long port = 3; port < rust->PortCount; ++port) {
					rust->connect_port(actual.handle, port,
							   actual.control + port - 3);
					reference->connect_port(expected.handle, port,
								expected.control + port - 3);
				}
			} else if (phase == 15) {
				rust->activate(actual.handle);
				reference->activate(expected.handle);
			}
			for (unsigned int block = 0; block < 80; ++block) {
				unsigned long count =
					block % 7 == 0 ? 0 : (block % 3 == 0 ? 1 : BLOCK);
				for (unsigned long index = 0; index < count; ++index) {
					float input =
						(float)(0.2 * sin((double)(block * BLOCK + index) *
								  0.07));
					if (phase >= 8)
						input = signals[index % (sizeof(signals) /
									 sizeof(signals[0]))];
					actual.program[index] = expected.program[index] = input;
					actual.detector[index] = expected.detector[index] =
						phase < 8 ? signals[phase]
							  : signals[(index + phase) %
								    (sizeof(signals) /
								     sizeof(signals[0]))];
				}
				rust->run(actual.handle, count);
				reference->run(expected.handle, count);
				for (unsigned long index = 0; index < count; ++index) {
					double error = fabs((double)actual.output[index] -
							    expected.output[index]);
					assert(isfinite(actual.output[index]));
					assert(error <=
					       4 * FLT_EPSILON *
						       fmax(1.0, fabs(expected.output[index])));
					maximum_error = fmax(maximum_error, error);
					rounded +=
						memcmp(&actual.output[index],
						       &expected.output[index], sizeof(float)) != 0;
					compared++;
				}
			}
		}
		rust->cleanup(actual.handle);
		reference->cleanup(expected.handle);
	}
	printf("Rust/C RMS AGC parity: %lu samples, %lu rounded differences, max error %.9g\n",
	       compared, rounded, maximum_error);
}

/** @brief Run all standalone LADSPA gain-rider checks.
 * @return Zero after all assertions pass.
 */
int main(void)
{
	test_host_contract();
	test_gain_policy(8000);
	test_gain_policy(16000);
	test_gain_policy(48000);
	test_activity_and_detector();
	test_hold_resets();
	test_invalid_and_runtime_controls();
	test_block_independence();
	test_reference_parity();
	benchmark_processing();
	puts("RMS AGC LADSPA checks passed");
	return 0;
}

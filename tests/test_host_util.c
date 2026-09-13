/** @file
 * @brief Deterministic host clock, tuning cancellation, and meter text regressions.
 */

#include "asterisk.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "asterisk/cli.h"
#include <rptadvradio/rptadvradio.h>

#include "usbradioplus_host_util.h"

/** @brief CLI descriptor used by the deterministic test boundary. */
#define TEST_CLI_FD 23
/** @brief Text emitted by the host helper under test. */
static char cli_output[1024];
/** @brief Number of bytes already written to the captured CLI text. */
static size_t cli_length;
/** @brief Poll timeout sequence requested by the calibration helper. */
static int poll_timeouts[64];
/** @brief Number of poll calls in the current case. */
static size_t poll_calls;
/** @brief One-based poll call on which an input or error result is returned. */
static size_t poll_ready_call;
/** @brief Nonzero poll result selected for the scripted call. */
static int poll_ready_result;
/** @brief Delay received by the noninteractive wait boundary. */
static useconds_t sleep_microseconds;
/** @brief Number of sleep calls in the current case. */
static unsigned int sleep_calls;
/** @brief Clock sample selected by the current case. */
static struct timespec clock_sample;
/** @brief Whether the clock boundary should fail without writing a sample. */
static int clock_failure;

/** @brief Capture the exact Asterisk CLI text produced by the real helper. */
void ast_cli(int fd, const char *format, ...)
{
	va_list arguments;
	int written;

	assert(fd == TEST_CLI_FD);
	va_start(arguments, format);
	written = vsnprintf(cli_output + cli_length, sizeof(cli_output) - cli_length, format,
			    arguments);
	va_end(arguments);
	assert(written >= 0);
	assert((size_t)written < sizeof(cli_output) - cli_length);
	cli_length += (size_t)written;
}

/** @brief Record timeout and input flags without consuming a keyboard byte. */
int __wrap_poll(struct pollfd *descriptors, nfds_t count, int milliseconds)
{
	assert(count == 1U);
	assert(descriptors[0].fd == TEST_CLI_FD);
	assert(descriptors[0].events == POLLIN);
	assert(descriptors[0].revents == 0);
	assert(poll_calls < sizeof(poll_timeouts) / sizeof(poll_timeouts[0]));
	poll_timeouts[poll_calls++] = milliseconds;
	return poll_calls == poll_ready_call ? poll_ready_result : 0;
}

/** @brief Capture noninteractive delays without sleeping during the test. */
int __wrap_usleep(useconds_t microseconds)
{
	sleep_microseconds = microseconds;
	sleep_calls++;
	return 0;
}

/** @brief Supply monotonic samples and a deterministic unavailable-clock case. */
int __wrap_clock_gettime(clockid_t clock, struct timespec *sample)
{
	assert(clock == CLOCK_MONOTONIC);
	if (clock_failure) {
		errno = EINVAL;
		return -1;
	}
	*sample = clock_sample;
	return 0;
}

/** @brief Clear all observable host output between independent cases. */
static void reset_host(void)
{
	memset(cli_output, 0, sizeof(cli_output));
	cli_length = 0U;
	memset(poll_timeouts, 0, sizeof(poll_timeouts));
	poll_calls = 0U;
	poll_ready_call = 0U;
	poll_ready_result = 1;
	sleep_microseconds = 0U;
	sleep_calls = 0U;
	clock_sample = (struct timespec){.tv_sec = 12345, .tv_nsec = 987654321};
	clock_failure = 0;
}

/** @brief Preserve monotonic whole seconds and truncation to microseconds. */
static void test_monotonic_time(void)
{
	struct timeval now;
	time_t seconds = 0;

	reset_host();
	now = usbradioplus_host_tvnow();
	assert(now.tv_sec == 12345 && now.tv_usec == 987654);
	usbradioplus_host_time(&seconds);
	assert(seconds == 12345);
	clock_failure = 1;
	now = usbradioplus_host_tvnow();
	assert(now.tv_sec == 0 && now.tv_usec == 0);
	usbradioplus_host_time(&seconds);
	assert(seconds == 0);
}

/** @brief Propagate timeout, keyboard readiness, and host polling errors unchanged. */
static void test_poll_input(void)
{
	reset_host();
	assert(usbradioplus_host_poll_input(TEST_CLI_FD, 1000) == 0);
	assert(poll_timeouts[0] == 1000);
	poll_ready_call = 2U;
	assert(usbradioplus_host_poll_input(TEST_CLI_FD, 100) == 1);
	poll_ready_call = 3U;
	poll_ready_result = -1;
	assert(usbradioplus_host_poll_input(TEST_CLI_FD, 10) == -1);
	assert(!cli_length && !sleep_calls);
}

/** @brief Retain quiet noninteractive delays and established interactive carriage returns. */
static void test_calibration_wait(void)
{
	size_t index;

	reset_host();
	assert(!usbradioplus_host_wait_or_poll(TEST_CLI_FD, 5000, 0));
	assert(sleep_calls == 1U && sleep_microseconds == 5000000U);
	assert(!poll_calls && !cli_length);

	reset_host();
	assert(!usbradioplus_host_wait_or_poll(TEST_CLI_FD, 10, 1));
	assert(poll_calls == 1U && poll_timeouts[0] == 10);
	assert(!strcmp(cli_output, "\r"));
	assert(!sleep_calls);

	reset_host();
	assert(!usbradioplus_host_wait_or_poll(TEST_CLI_FD, 1000, 1));
	assert(poll_calls == 11U && poll_timeouts[10] == 0);
	assert(cli_length == 11U);
	for (index = 0U; index < 10U; ++index) {
		assert(poll_timeouts[index] == 100);
		assert(cli_output[index] == '\r');
	}
	assert(cli_output[10] == '\r');

	reset_host();
	assert(!usbradioplus_host_wait_or_poll(TEST_CLI_FD, 250, 1));
	assert(poll_calls == 3U && poll_timeouts[0] == 100 && poll_timeouts[1] == 100 &&
	       poll_timeouts[2] == 50);
	assert(!strcmp(cli_output, "\r\r\r"));

	reset_host();
	poll_ready_call = 2U;
	assert(usbradioplus_host_wait_or_poll(TEST_CLI_FD, 1000, 1) == 1);
	assert(poll_calls == 2U && !strcmp(cli_output, "\r\r"));

	reset_host();
	poll_ready_call = 1U;
	poll_ready_result = -1;
	assert(usbradioplus_host_wait_or_poll(TEST_CLI_FD, 10, 1) == 1);
	assert(poll_calls == 1U && !cli_length);
}

/** @brief Keep silence, scaling, extrema, and clip totals byte-for-byte compatible. */
static void test_audio_statistics_text(void)
{
	struct rptadv_radio_audio_statistics statistics = {0};
	struct rptadv_radio_audio_statistics original;
	size_t index;

	reset_host();
	usbradioplus_host_print_audio_stats(TEST_CLI_FD, &statistics, "Rx");
	assert(!strcmp(cli_output,
		       "RxAudioStats: Pk -96.0  Avg Pwr -96  Min -96  Max -96  dBFS  ClipCnt 0\n"));

	for (index = 0U; index < RPTADV_RADIO_AUDIO_STATS_LEN; ++index) {
		statistics.maxbuf[index] = 32768U;
		statistics.pwrbuf[index] = 268435456U;
		statistics.clipbuf[index] = 2U;
	}
	statistics.index = RPTADV_RADIO_AUDIO_STATS_LEN - 1U;
	original = statistics;
	reset_host();
	usbradioplus_host_print_audio_stats(TEST_CLI_FD, &statistics, "Tx");
	assert(!strcmp(
		cli_output,
		"TxAudioStats: Pk   0.0  Avg Pwr  -6  Min  -6  Max  -6  dBFS  ClipCnt 100\n"));
	assert(!memcmp(&statistics, &original, sizeof(statistics)));

	memset(&statistics, 0, sizeof(statistics));
	statistics.maxbuf[0] = 16384U;
	statistics.pwrbuf[0] = 268435456U;
	statistics.clipbuf[0] = 3U;
	reset_host();
	usbradioplus_host_print_audio_stats(TEST_CLI_FD, &statistics, "Rx");
	assert(!strcmp(cli_output,
		       "RxAudioStats: Pk  -6.0  Avg Pwr -23  Min -96  Max  -6  dBFS  ClipCnt 3\n"));
	assert(!poll_calls && !sleep_calls);
}

int main(void)
{
	test_monotonic_time();
	test_poll_input();
	test_calibration_wait();
	test_audio_statistics_text();
	return 0;
}

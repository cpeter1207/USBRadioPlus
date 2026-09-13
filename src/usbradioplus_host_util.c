/** @file
 * @brief Host timing, keyboard polling, and portable meter presentation.
 */

#include "asterisk.h"

#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <unistd.h>

#include "asterisk/cli.h"
#include <rptadvradio/rptadvradio.h>

#include "usbradioplus_host_util.h"

struct timeval usbradioplus_host_tvnow(void)
{
	struct timespec now = {0};
	struct timeval result;

	clock_gettime(CLOCK_MONOTONIC, &now);
	result.tv_sec = now.tv_sec;
	result.tv_usec = now.tv_nsec / 1000;
	return result;
}

void usbradioplus_host_time(time_t *seconds)
{
	*seconds = usbradioplus_host_tvnow().tv_sec;
}

int usbradioplus_host_poll_input(int fd, int milliseconds)
{
	struct pollfd input = {.fd = fd, .events = POLLIN};

	return poll(&input, 1, milliseconds);
}

int usbradioplus_host_wait_or_poll(int fd, int milliseconds, int interactive)
{
	int remaining = milliseconds;

	if (!interactive) {
		usleep((useconds_t)milliseconds * 1000U);
		return 0;
	}
	while (remaining >= 100) {
		ast_cli(fd, "\r");
		if (usbradioplus_host_poll_input(fd, 100))
			return 1;
		remaining -= 100;
	}
	if (usbradioplus_host_poll_input(fd, remaining))
		return 1;
	ast_cli(fd, "\r");
	return 0;
}

/** @brief Convert a signed-16 mean-square measurement to the tuning CLI's dBFS units.
 * @param power Mean-square sample power or squared peak.
 * @return Power relative to full scale, retaining the established silence indication.
 */
static double audio_power_dbfs(double power)
{
	const double normalized = power / 1073741824.0;

	/* Integer-derived powers are zero or at least one unit per statistics window, so
	 * normalization cannot underflow a positive value into the silence case. */
	if (!(normalized > 0.0))
		return -96.0;
	return 10.0 * log10(normalized);
}

void usbradioplus_host_print_audio_stats(int fd,
					 const struct rptadv_radio_audio_statistics *statistics,
					 const char *prefix)
{
	uint32_t peak = 0U;
	uint32_t minimum_power = 1073741824U;
	uint32_t maximum_power = 0U;
	unsigned int clips = 0U;
	double total_power = 0.0;
	size_t index;

	for (index = 0U; index < RPTADV_RADIO_AUDIO_STATS_LEN; ++index) {
		const uint32_t power = statistics->pwrbuf[index];

		if (statistics->maxbuf[index] > peak)
			peak = statistics->maxbuf[index];
		if (power < minimum_power)
			minimum_power = power;
		if (power > maximum_power)
			maximum_power = power;
		total_power += power;
		clips += statistics->clipbuf[index];
	}
	ast_cli(fd,
		"%sAudioStats: Pk %5.1f  Avg Pwr %3.0f  Min %3.0f  Max %3.0f  dBFS  ClipCnt %u\n",
		prefix, audio_power_dbfs((double)peak * peak),
		audio_power_dbfs(total_power / RPTADV_RADIO_AUDIO_STATS_LEN),
		audio_power_dbfs(minimum_power), audio_power_dbfs(maximum_power), clips);
}

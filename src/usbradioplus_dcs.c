/** @file
 * @brief DCS configuration and portable receive-decoder binding.
 */

#include "usbradioplus_dcs.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int urp_dcs_code_supported(int code)
{
	/* DCS assignments beyond published tables are common in deployed radios. */
	return code >= 0 && code <= 0777;
}

int urp_dcs_parse_code(const char *text, int *code, int *inverted)
{
	int value;

	if (!text || strlen(text) != 4U || !code || !inverted)
		return -1;
	if (text[0] < '0' || text[0] > '7' || text[1] < '0' || text[1] > '7' || text[2] < '0' ||
	    text[2] > '7')
		return -1;
	/* Three validated octal digits always represent a supported 000..777 code. */
	value = ((text[0] - '0') << 6) | ((text[1] - '0') << 3) | (text[2] - '0');
	if (toupper((unsigned char)text[3]) == 'N')
		*inverted = 0;
	else if (toupper((unsigned char)text[3]) == 'I')
		*inverted = 1;
	else
		return -1;
	*code = value;
	return 0;
}

void urp_dcs_format_code(char *text, size_t size, int code, int inverted)
{
	if (!text || !size)
		return;
	(void)snprintf(text, size, "%03o%c", code & 0777, inverted ? 'I' : 'N');
}

void urp_dcs_init(struct urp_dcs_state *state)
{
	if (state)
		memset(state, 0, sizeof(*state));
}

void urp_dcs_configure(struct urp_dcs_state *state, int receive_code, int receive_inverted,
		       int transmit_code, int transmit_inverted)
{
	if (!state)
		return;
	state->receive_code = urp_dcs_code_supported(receive_code) ? receive_code : -1;
	state->receive_inverted = !!receive_inverted;
	state->transmit_code = urp_dcs_code_supported(transmit_code) ? transmit_code : -1;
	state->transmit_inverted = !!transmit_inverted;
	state->enabled_receive = state->receive_code >= 0;
	state->enabled_transmit = state->transmit_code >= 0;
	/* The bound Rust object observes this configuration at the next callback. */
	state->valid = 0;
}

void urp_dcs_set_receive_callback(struct urp_dcs_state *state, urp_dcs_receive_callback callback,
				  void *context)
{
	if (!state)
		return;
	state->receive_callback = callback;
	state->receive_callback_context = context;
	if (!callback)
		state->valid = 0;
}

int urp_dcs_process(struct urp_dcs_state *state, const int16_t *samples, size_t count,
		    size_t stride, unsigned int sample_rate)
{
	int decoded = 0;

	if (!state || !samples || !stride || !sample_rate || !state->enabled_receive)
		return 0;
	if (!state->receive_callback ||
	    state->receive_callback(state->receive_callback_context, samples, count, stride,
				    sample_rate, &decoded) != 0) {
		/* A renderer can only be absent while closed or during teardown. */
		state->valid = 0;
		return 0;
	}
	state->valid = !!decoded;
	return state->valid;
}

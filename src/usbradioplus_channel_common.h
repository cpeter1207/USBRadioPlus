#ifndef USBRADIOPLUS_CHANNEL_COMMON_H
#define USBRADIOPLUS_CHANNEL_COMMON_H

#include "asterisk/channel.h"
#include "asterisk/config.h"

#include "usbradioplus_channel_private.h"

extern int8_t pp_pulsemask;
extern int8_t pp_lastmask;

extern int8_t pp_pulsemask;
extern int8_t pp_lastmask;

int hidhdwconfig(struct chan_usbradio_pvt *o);

void destroy_unlinked_channel(struct chan_usbradio_pvt *o);

void kickptt(const struct chan_usbradio_pvt *o);

int load_tune_config(struct chan_usbradio_pvt *o, const struct ast_config *cfg, int reload);

int usbradio_digit_begin(struct ast_channel *c, char digit);

int usbradio_digit_end(struct ast_channel *c, char digit, unsigned int duration);

int usbradio_answer(struct ast_channel *c);

void usbradioplus_queue_program(struct chan_usbradio_pvt *o, const short *samples, size_t count);

int usbradio_fixup(struct ast_channel *oldchan, struct ast_channel *newchan);

int usbradio_indicate(struct ast_channel *c, int cond_in, const void *data, size_t datalen);

int usbradio_setoption(struct ast_channel *chan, int option, void *data, int datalen);

int console_key(int fd, int argc, const char *const *argv);

int console_unkey(int fd, int argc, const char *const *argv);

int radio_tune(int fd, int argc, const char *const *argv);

int set_txctcss_level(struct chan_usbradio_pvt *o);

int parse_tune_level(const char *text, int *level);

int radio_set_dsp_debug(int fd, int argc, const char *const *argv);

void store_rxdemod(struct chan_usbradio_pvt *o, const char *s);

void store_txmixa(struct chan_usbradio_pvt *o, const char *s);

void store_txmixb(struct chan_usbradio_pvt *o, const char *s);

void store_rxcdtype(struct chan_usbradio_pvt *o, const char *s);

void store_rxsdtype(struct chan_usbradio_pvt *o, const char *s);

void store_rxvoiceadj(struct chan_usbradio_pvt *o, const char *s);

int effective_txmixaset(const struct chan_usbradio_pvt *o);

int effective_txmixbset(const struct chan_usbradio_pvt *o);

enum radio_carrier_detect effective_rxcdtype(const struct chan_usbradio_pvt *o);

void store_txtoctype(struct chan_usbradio_pvt *o, const char *s);

void tune_txoutput(struct chan_usbradio_pvt *o, int value, int fd, int intflag);

void mult_set(struct chan_usbradio_pvt *o);

void usbradioplus_program_radio(struct chan_usbradio_pvt *o);

void usbradioplus_parallel_program_write(void *opaque, uint8_t value);

void usbradioplus_set_channel(uint8_t channel);

int radio_config(struct chan_usbradio_pvt *o);

int store_cutoff(struct chan_usbradio_pvt *o, const char *name, const char *text);

int apply_processing_config_overrides(struct chan_usbradio_pvt *o, const char *category);

int usbradioplus_dsp_init(struct chan_usbradio_pvt *o);

void usbradioplus_dsp_destroy(struct chan_usbradio_pvt *o);

void usbradioplus_prepare_squelch_audio(struct chan_usbradio_pvt *o);

int usbradioplus_carrier_detected(const struct chan_usbradio_pvt *o,
				  enum radio_carrier_detect source);

int usbradioplus_ctcss_detected(const struct chan_usbradio_pvt *o);

void usbradioplus_refresh_ctcss_decode(struct chan_usbradio_pvt *o);

void usbradioplus_wait_for_eeprom_idle(struct chan_usbradio_pvt *o);

void usbradioplus_parrot_rx_transition(struct chan_usbradio_pvt *o, int was_keyed);

int load_config(int reload);

int reload_module(void);

void *pulserthread(void *arg);

struct chan_usbradio_pvt *store_config(struct ast_config *cfg, const char *ctg);
void radio_dump(struct chan_usbradio_pvt *o, int fd);
int usb_device_swap(int fd, const char *other);

#endif

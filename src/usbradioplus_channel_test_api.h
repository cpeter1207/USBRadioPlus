#ifndef USBRADIOPLUS_CHANNEL_TEST_API_H
#define USBRADIOPLUS_CHANNEL_TEST_API_H

#include "usbradioplus_channel_private.h"
#include "usbradioplus_channel_common.h"

extern char hasout;
extern const int ppinshift[];

int setformat(struct chan_usbradio_pvt *o, int mode);
int usbradio_text(struct ast_channel *c, const char *text);
int usbradio_hangup(struct ast_channel *c);
int usbradio_call(struct ast_channel *c, const char *dest, int timeout);
int usbradio_write(struct ast_channel *c, struct ast_frame *f);
int radio_active(int fd, int argc, const char *const *argv);
int used_blocks(struct chan_usbradio_pvt *o);
int soundcard_writeframe(struct chan_usbradio_pvt *o, short *data);
int load_module(void);
int unload_module(void);
void usbradioplus_test_set_module_info(struct ast_module_info *info);
char *find_installed_usb_match(void);
void *hidthread(void *arg);
struct ast_frame *usbradio_read(struct ast_channel *c);
struct ast_channel *usbradio_new(struct chan_usbradio_pvt *o, char *ext, char *ctx, int state,
				 const struct ast_assigned_ids *assignedids,
				 const struct ast_channel *requestor);
struct ast_channel *usbradio_request(const char *type, struct ast_format_cap *cap,
				     const struct ast_assigned_ids *assignedids,
				     const struct ast_channel *requestor, const char *data,
				     int *cause);
char *res2cli(int result);
char *handle_console_key(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *handle_console_unkey(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *handle_radio_tune(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *handle_radio_active(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *handle_show_settings(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *handle_set_dsp_debug(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *handle_radioplus_native_stats(struct ast_cli_entry *entry, int command,
				    struct ast_cli_args *args);
void usbradio_start_parallel_pulser(void);

#ifdef URP_CHANNEL_MODERN
extern short silence_buf[];
int usbradio_log_fault(struct chan_usbradio_pvt *o, int already_logged, const char *format, ...)
	__attribute__((format(printf, 3, 4)));
void usbradio_device_identity(struct chan_usbradio_pvt *o, char *devstr, size_t devstr_size,
			      char *serial, size_t serial_size, int *alsa_card);
void usbradio_log_usb_recovered(struct chan_usbradio_pvt *o);
void usbradio_adjust_txmix_for_mono(struct chan_usbradio_pvt *o);
void usbradio_release_device(struct chan_usbradio_pvt *o);
void usbradio_swap_begin(struct chan_usbradio_pvt *o);
void usbradio_swap_audio_stopped(struct chan_usbradio_pvt *o);
int usbradio_swap_hid_wait(struct chan_usbradio_pvt *o);
int usbradio_swap_ready(struct chan_usbradio_pvt *o);
void usbradio_swap_finish(struct chan_usbradio_pvt *o);
void usbradio_mixer_limits(struct chan_usbradio_pvt *o, int *rx_max, int *tx_max,
			   int *sidetone_max);
void usbradio_set_sidetone_switch(struct chan_usbradio_pvt *o, int enabled);
void usbradio_set_rx_mixer(struct chan_usbradio_pvt *o, long volume);
int init_audio_device(struct chan_usbradio_pvt *o);
int usbradio_start_audio(struct chan_usbradio_pvt *o);
PaError usbradio_read_pa_stereo(struct chan_usbradio_pvt *o);
void stream_cleanup(struct chan_usbradio_pvt *o);
void *usbradio_audio_thread(void *arg);
#endif

#endif

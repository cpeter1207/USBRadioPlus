#ifndef USBRADIOPLUS_CHANNEL_PRIVATE_H
#define USBRADIOPLUS_CHANNEL_PRIVATE_H

#define PLUS_LINK_NATIVE_TARGET_SAMPLES (URP_NATIVE_SAMPLES * 3)
#define DUPLEX3_LEVEL_MAX 999
#define DEFAULT_ECHO_MAX 1000
#define URP_LEGACY_TEST_TONE_PEAK 7518.0
#define RX_ON_DELAY_MAX 60000
#define TX_OFF_DELAY_MAX 60000
#define MS_PER_FRAME 20
#define MS_TO_FRAMES(ms) ((ms) / MS_PER_FRAME)
#define DEFAULT_TX_SOFT_LIMITER_SETPOINT 12000
#define READERR_THRESHOLD 50
#define QUEUE_SIZE 20 /* 400 milliseconds of sound-card output buffering. */
#define plus_mix_has_program(mix) urp_tx_output_has_program((enum urp_tx_output_mode)(mix))
#define PP_PORT "/dev/parport0"
#define PP_IOPORT 0x378
#define RPT_TO_STRING(x) #x
#define N_FMT(duf) "%30" #duf
#define CONFIG "usbradioplus.conf"
#define RX_CAP_RAW_FILE "/tmp/rx_cap_in.pcm"
#define RX_CAP_TRACE_FILE "/tmp/rx_trace.pcm"
#define TX_CAP_RAW_FILE "/tmp/tx_cap_in.pcm"
#define TX_CAP_TRACE_FILE "/tmp/tx_trace.pcm"

extern const char *const cd_signal_type[];
extern const char *const sd_signal_type[];
extern struct chan_usbradio_pvt usbradio_default;
extern struct ast_jb_conf global_jbconf;
extern FILE *frxcapraw;
extern FILE *frxcaptrace;
extern FILE *frxoutraw;
extern FILE *ftxcapraw;
extern FILE *ftxcaptrace;
extern FILE *ftxoutraw;
extern ast_mutex_t pp_lock;
extern int8_t pp_val;
extern int8_t pp_pulsemask;
extern int8_t pp_lastmask;
extern int pp_pulsetimer[32];
extern int haspp;
extern int ppfd;
extern char pport[50];
extern int pbase;
extern char stoppulser;
extern char *usbradio_active;

enum duplex3_mode {
	DUPLEX3_MODE_HARDWARE = 0,
	DUPLEX3_MODE_SOFTWARE,
};

#ifdef URP_CHANNEL_MODERN
#include "usbradioplus_channel_modern_private.h"
#else
#include "usbradioplus_channel_legacy_private.h"
#endif

#define plus_parrot plus_parrot_state.audio
#define plus_parrot_capacity plus_parrot_state.capacity
#define plus_parrot_count plus_parrot_state.count
#define plus_parrot_play plus_parrot_state.play
#define plus_parrot_playing plus_parrot_state.playing
#define plus_parrot_truncated plus_parrot_state.truncated
#define usbradioplus_native_echo(channel)                                                          \
	urp_native_echo_enabled((channel)->duplex3, (channel)->duplex3mode == DUPLEX3_MODE_SOFTWARE)

double effective_rx_input_gain_db(const struct chan_usbradio_pvt *channel);
enum radio_tx_mix effective_txmixa(const struct chan_usbradio_pvt *channel);
enum radio_tx_mix effective_txmixb(const struct chan_usbradio_pvt *channel);
void refresh_processing_hardware(struct chan_usbradio_pvt *channel);
void plus_link_native_push(struct chan_usbradio_pvt *channel, const short *samples, size_t count);
int plus_link_native_pop(struct chan_usbradio_pvt *channel, short *samples);
void usbradioplus_check_tx_audio(struct chan_usbradio_pvt *channel, short *samples, size_t count);
void usbradioplus_native_tick(struct chan_usbradio_pvt *channel);
void usbradioplus_tune_mixer_limits(struct chan_usbradio_pvt *channel, int *microphone_max,
				    int *speaker_max, int *microphone_playback_max);
void tune_menusupport(int fd, struct chan_usbradio_pvt *channel, const char *command);
void mixer_write(struct chan_usbradio_pvt *o);
void tune_write(struct chan_usbradio_pvt *o);
void tune_rxinput(int fd, struct chan_usbradio_pvt *o, int setsql, int intflag);
void _menu_rxvoice(int fd, struct chan_usbradio_pvt *o, const char *str);
void _menu_print(int fd, struct chan_usbradio_pvt *o);
void tune_flash(int fd, struct chan_usbradio_pvt *channel, int interactive);
int validate_tx_soft_limiter_setpoint(struct chan_usbradio_pvt *channel, int setpoint);
float effective_rx_decoder_gain(const struct chan_usbradio_pvt *channel);
int effective_rxmixerset(const struct chan_usbradio_pvt *channel);
void tune_rxdisplay(int fd, struct chan_usbradio_pvt *channel);
void tune_rxtx_status(int fd, struct chan_usbradio_pvt *channel);
void _menu_rxsquelch(int fd, struct chan_usbradio_pvt *channel, const char *value);
void _menu_txvoice(int fd, struct chan_usbradio_pvt *channel, const char *value);
void _menu_auxvoice(int fd, struct chan_usbradio_pvt *channel, const char *value);
void _menu_txtone(int fd, struct chan_usbradio_pvt *channel, const char *value);
void tune_rxvoice(int fd, struct chan_usbradio_pvt *channel, int interactive);
void tune_rxctcss(int fd, struct chan_usbradio_pvt *channel, int interactive);
int usbradioplus_ensure_parrot_capacity(struct chan_usbradio_pvt *channel);
struct chan_usbradio_pvt *usbradioplus_channel_first(void);
struct chan_usbradio_pvt *find_desc(const char *device);

#endif

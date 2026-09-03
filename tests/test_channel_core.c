#define URP_CHANNEL_UNIT_TEST 1
#define AST_MODULE_SELF_SYM test_module_self
#define AST_MODULE "chan_usbradioplus"

#ifdef URP_TEST_MODERN
#include "../src/chan_usbradioplus_modern.c"
#else
#include "../src/chan_usbradioplus.c"
#endif

#include <assert.h>

#undef pthread_mutex_lock
#undef pthread_mutex_unlock
#undef free
#undef realloc
#undef calloc

struct ast_flags64 ast_options;
int option_debug;
static int fail_realloc;

unsigned int ast_debug_get_by_module(const char *module)
{
	(void)module;
	return 0;
}

void ast_log(int level, const char *file, int line, const char *function, const char *format, ...)
{
	(void)level;
	(void)file;
	(void)line;
	(void)function;
	(void)format;
}

int __ast_pthread_mutex_lock(const char *file, int line, const char *function,
			     const char *mutex_name, ast_mutex_t *mutex)
{
	(void)file;
	(void)line;
	(void)function;
	(void)mutex_name;
	return pthread_mutex_lock(&mutex->mutex);
}

int __ast_pthread_mutex_unlock(const char *file, int line, const char *function,
			       const char *mutex_name, ast_mutex_t *mutex)
{
	(void)file;
	(void)line;
	(void)function;
	(void)mutex_name;
	return pthread_mutex_unlock(&mutex->mutex);
}

void __ast_free(void *pointer, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	free(pointer);
}

void *__ast_realloc(void *pointer, size_t size, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	return fail_realloc ? NULL : realloc(pointer, size);
}

void *__ast_calloc(size_t count, size_t size, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	return calloc(count, size);
}

#ifdef URP_TEST_MODERN
const struct ast_radio_mixer_element *
ast_radio_device_mixer_element(const struct ast_radio_device *device,
			       const struct ast_radio_mixer_path *path)
{
	(void)device;
	(void)path;
	return NULL;
}

long ast_radio_device_mixer_max(const struct ast_radio_device *device,
				const struct ast_radio_mixer_path *path, unsigned int capability)
{
	(void)device;
	(void)path;
	(void)capability;
	return 100;
}

long ast_radio_device_mixer_scale(const struct ast_radio_device *device,
				  const struct ast_radio_mixer_path *path, unsigned int capability,
				  int setting)
{
	(void)device;
	(void)path;
	(void)capability;
	return setting;
}

int ast_radio_device_set_mixer(const struct ast_radio_device *device,
			       const struct ast_radio_mixer_path *path, unsigned int capability,
			       long value)
{
	(void)device;
	(void)path;
	(void)capability;
	(void)value;
	return 0;
}

int ast_radio_device_set_mixer_paths(const struct ast_radio_device *device,
				     const struct ast_radio_mixer_path *paths, size_t path_count,
				     unsigned int capability, long value)
{
	(void)device;
	(void)paths;
	(void)path_count;
	(void)capability;
	(void)value;
	return 0;
}
#else
int ast_radio_setamixer(int device, char *parameter, int first, int second)
{
	(void)device;
	(void)parameter;
	(void)first;
	(void)second;
	return 0;
}

int ast_radio_make_spkr_playback_value(int maximum, int requested, int device_type)
{
	(void)maximum;
	(void)device_type;
	return requested;
}
#endif

#ifdef URP_TEST_MODERN
int ast_radio_check_audio(short *samples, struct audiostatistics *statistics, short count,
			  short mono)
#else
int ast_radio_check_audio(short *samples, struct audiostatistics *statistics, short count)
#endif
{
	(void)samples;
	(void)statistics;
	(void)count;
#ifdef URP_TEST_MODERN
	(void)mono;
#endif
	return 0;
}

static void test_legacy_option_decoders(void)
{
	struct chan_usbradio_pvt radio = {0};

	store_rxdemod(&radio, "no");
	assert(radio.rxdemod == RX_AUDIO_NONE);
	store_rxdemod(&radio, "speaker");
	assert(radio.rxdemod == RX_AUDIO_SPEAKER);
	store_rxdemod(&radio, "flat");
	assert(radio.rxdemod == RX_AUDIO_FLAT);

	store_txmixa(&radio, "no");
	assert(radio.txmixa == TX_OUT_OFF);
	store_txmixa(&radio, "voice");
	assert(radio.txmixa == TX_OUT_VOICE);
	store_txmixa(&radio, "tone");
	assert(radio.txmixa == TX_OUT_LSD);
	store_txmixa(&radio, "composite");
	assert(radio.txmixa == TX_OUT_COMPOSITE);
	store_txmixa(&radio, "auxvoice");
	assert(radio.txmixa == TX_OUT_AUX);

	store_txmixb(&radio, "no");
	assert(radio.txmixb == TX_OUT_OFF);
	store_txmixb(&radio, "voice");
	assert(radio.txmixb == TX_OUT_VOICE);
	store_txmixb(&radio, "tone");
	assert(radio.txmixb == TX_OUT_LSD);
	store_txmixb(&radio, "composite");
	assert(radio.txmixb == TX_OUT_COMPOSITE);
	store_txmixb(&radio, "auxvoice");
	assert(radio.txmixb == TX_OUT_AUX);

	store_rxcdtype(&radio, "no");
	assert(radio.rxcdtype == CD_IGNORE);
	store_rxcdtype(&radio, "usb");
	assert(radio.rxcdtype == CD_HID);
	store_rxcdtype(&radio, "dsp");
	assert(radio.rxcdtype == CD_XPMR_NOISE);
	store_rxcdtype(&radio, "vox");
	assert(radio.rxcdtype == CD_XPMR_VOX);
	store_rxcdtype(&radio, "usbinvert");
	assert(radio.rxcdtype == CD_HID_INVERT);
	store_rxcdtype(&radio, "pp");
	assert(radio.rxcdtype == CD_PP);
	store_rxcdtype(&radio, "ppinvert");
	assert(radio.rxcdtype == CD_PP_INVERT);

	store_rxsdtype(&radio, "no");
	assert(radio.rxsdtype == SD_IGNORE);
	store_rxsdtype(&radio, "SD_IGNORE");
	assert(radio.rxsdtype == SD_IGNORE);
	store_rxsdtype(&radio, "usb");
	assert(radio.rxsdtype == SD_HID);
	store_rxsdtype(&radio, "SD_HID");
	assert(radio.rxsdtype == SD_HID);
	store_rxsdtype(&radio, "usbinvert");
	assert(radio.rxsdtype == SD_HID_INVERT);
	store_rxsdtype(&radio, "SD_HID_INVERT");
	assert(radio.rxsdtype == SD_HID_INVERT);
	store_rxsdtype(&radio, "dsp");
	assert(radio.rxsdtype == SD_XPMR);
	store_rxsdtype(&radio, "SD_XPMR");
	assert(radio.rxsdtype == SD_XPMR);
	store_rxsdtype(&radio, "pp");
	assert(radio.rxsdtype == SD_PP);
	store_rxsdtype(&radio, "ppinvert");
	assert(radio.rxsdtype == SD_PP_INVERT);

	store_rxvoiceadj(&radio, "0.75");
	assert(fabs(radio.legacy_rxvoiceadj - 0.75F) < 0.0001F);
	assert(radio.legacy_rxvoiceadj_configured == 1);

	radio.rxdemod = RX_AUDIO_FLAT;
	store_rxdemod(&radio, "invalid");
	assert(radio.rxdemod == RX_AUDIO_FLAT);
	radio.txmixa = TX_OUT_VOICE;
	store_txmixa(&radio, "invalid");
	assert(radio.txmixa == TX_OUT_VOICE);
	radio.txmixb = TX_OUT_VOICE;
	store_txmixb(&radio, "invalid");
	assert(radio.txmixb == TX_OUT_VOICE);
	radio.rxcdtype = CD_HID;
	store_rxcdtype(&radio, "invalid");
	assert(radio.rxcdtype == CD_HID);
	radio.rxsdtype = SD_HID;
	store_rxsdtype(&radio, "invalid");
	assert(radio.rxsdtype == SD_HID);
	store_txtoctype(&radio, "no");
	assert(radio.txtoctype == TOC_NONE);
	store_txtoctype(&radio, "TOC_NONE");
	store_txtoctype(&radio, "phase");
	assert(radio.txtoctype == TOC_PHASE);
	store_txtoctype(&radio, "TOC_PHASE");
	store_txtoctype(&radio, "notone");
	assert(radio.txtoctype == TOC_NOTONE);
	store_txtoctype(&radio, "TOC_NOTONE");
	store_txtoctype(&radio, "invalid");
	assert(radio.txtoctype == TOC_NOTONE);
}

static void test_effective_processing_settings(void)
{
	struct chan_usbradio_pvt radio = {0};

	settings_defaults(&settings);
	radio.legacy_rxvoiceadj = 0.5F;
	assert(fabs(effective_rx_input_gain_db(&radio)) < 0.0001);
	settings.chains[TXAGC_LOCAL].input_gain_configured = 1;
	settings.chains[TXAGC_LOCAL].agc.input_gain_db = 6.0;
	assert(fabs(effective_rx_input_gain_db(&radio) - 6.0) < 0.0001);
	assert(fabs(effective_legacy_rxvoiceadj(&radio) - 0.997631F) < 0.0001F);

	radio.rxmixerset = 321;
	radio.txmixaset = 322;
	radio.txmixbset = 323;
	radio.txmixa = TX_OUT_VOICE;
	radio.txmixb = TX_OUT_LSD;
	radio.rxcdtype = CD_HID;
	assert(effective_rxmixerset(&radio) == 321);
	assert(effective_txmixaset(&radio) == 322);
	assert(effective_txmixbset(&radio) == 323);
	assert(effective_txmixa(&radio) == TX_OUT_VOICE);
	assert(effective_txmixb(&radio) == TX_OUT_LSD);
	assert(effective_rxcdtype(&radio) == CD_HID);

	settings.hardware.input_gain_configured = 1;
	settings.hardware.input_gain_db = 0.0;
	settings.hardware.output_a_gain_configured = 1;
	settings.hardware.output_a_gain_db = 0.0;
	settings.hardware.output_b_gain_configured = 1;
	settings.hardware.output_b_gain_db = 6.0;
	settings.hardware.output_a_assignment_configured = 1;
	settings.hardware.output_a_assignment = TX_OUT_COMPOSITE;
	settings.hardware.output_b_assignment_configured = 1;
	settings.hardware.output_b_assignment = TX_OUT_AUX;
	assert(effective_rxmixerset(&radio) == 500);
	assert(effective_txmixaset(&radio) == 500);
	assert(effective_txmixbset(&radio) == 998);
	assert(effective_txmixa(&radio) == TX_OUT_COMPOSITE);
	assert(effective_txmixb(&radio) == TX_OUT_AUX);

	settings.hardware.cos_assignment_configured = 1;
	strcpy(settings.hardware.cos_assignment, "usb");
	assert(effective_rxcdtype(&radio) == CD_HID);
	strcpy(settings.hardware.cos_assignment, "usbinvert");
	assert(effective_rxcdtype(&radio) == CD_HID_INVERT);
	strcpy(settings.hardware.cos_assignment, "dsp");
	assert(effective_rxcdtype(&radio) == CD_XPMR_NOISE);
	strcpy(settings.hardware.cos_assignment, "vox");
	assert(effective_rxcdtype(&radio) == CD_XPMR_VOX);
	strcpy(settings.hardware.cos_assignment, "pp");
	assert(effective_rxcdtype(&radio) == CD_PP);
	strcpy(settings.hardware.cos_assignment, "ppinvert");
	assert(effective_rxcdtype(&radio) == CD_PP_INVERT);
	strcpy(settings.hardware.cos_assignment, "no");
	assert(effective_rxcdtype(&radio) == CD_IGNORE);
}

static void test_numeric_helpers(void)
{
	double samples[] = {-0.25, 0.5, -0.75, 0.125};
	short integer_samples[] = {0, -10, 20, INT16_MIN};

	assert(urp_gain_db_to_mixer(0.0) == 500);
	assert(urp_gain_db_to_mixer(20.0) == 999);
	assert(fabs(urp_mixer_to_gain_db(500)) < 0.0001);
	assert(urp_mixer_to_gain_db(0) < -100.0);
	assert(urp_legacy_multiplier(0) == 64);
	assert(urp_legacy_multiplier(500) > 0);
	assert(urp_legacy_multiplier(999) > urp_legacy_multiplier(500));
	assert(urp_pcm_peak_dbfs(0) == -INFINITY);
	assert(fabs(urp_pcm_peak_dbfs(32768)) < 0.0001);
	assert(fabs(urp_double_peak(samples, ARRAY_LEN(samples)) - 0.75) < 0.0001);
	assert(urp_double_peak(samples, 0) == 0.0);
	assert(urp_pcm_peak(integer_samples, ARRAY_LEN(integer_samples)) == 32768U);
	assert(urp_pcm_peak(integer_samples, 0) == 0U);
	assert(urp_saturating_add(100, 200) == 300);
	assert(urp_saturating_add(30000, 30000) == 32767);
	assert(urp_saturating_add(-30000, -30000) == -32768);
	assert(urp_apply_gain(1000, 0.5) == 500);
	assert(urp_apply_gain(30000, 2.0) == 32767);
	assert(urp_apply_gain(-30000, 2.0) == -32768);
	assert(!plus_mix_has_program(TX_OUT_OFF));
	assert(plus_mix_has_program(TX_OUT_VOICE));
	assert(!plus_mix_has_program(TX_OUT_LSD));
	assert(plus_mix_has_program(TX_OUT_COMPOSITE));
	assert(plus_mix_has_program(TX_OUT_AUX));
	assert(usbradioplus_legacy_cutoff("rxlpf", 1) ==
	       urp_legacy_cutoff(URP_FILTER_RX_LOWPASS, 1));
	assert(usbradioplus_legacy_cutoff("rxhpf", 1) ==
	       urp_legacy_cutoff(URP_FILTER_RX_HIGHPASS, 1));
	assert(usbradioplus_legacy_cutoff("txlpf", 1) ==
	       urp_legacy_cutoff(URP_FILTER_TX_LOWPASS, 1));
	assert(usbradioplus_legacy_cutoff("txhpf", 1) ==
	       urp_legacy_cutoff(URP_FILTER_TX_HIGHPASS, 1));
}

static void test_native_fifo_and_squelch_copy(void)
{
	struct chan_usbradio_pvt radio = {0};
	short input[URP_NATIVE_FIFO_SAMPLES + 1];
	short output[URP_NATIVE_SAMPLES];
	short *capture = (short *)(radio.usbradio_read_buf + AST_FRIENDLY_OFFSET);
	size_t i;

	for (i = 0; i < ARRAY_LEN(input); i++)
		input[i] = (short)i;
	assert(!plus_link_native_pop(&radio, output));
	plus_link_native_push(&radio, input, ARRAY_LEN(input));
	assert(radio.plus_native_fifo.count == URP_NATIVE_FIFO_SAMPLES);
	assert(radio.plus_link_queue_overflows == 1);
	assert(plus_link_native_pop(&radio, output));
	assert(output[0] == input[1]);
	assert(radio.plus_native_fifo.count == URP_NATIVE_FIFO_SAMPLES - URP_NATIVE_SAMPLES);

	for (i = 0; i < ARRAY_LEN(radio.plus_squelch_native); i++)
		capture[i] = (short)(i - 100);
	usbradioplus_prepare_squelch_audio(&radio);
	assert(memcmp(capture, radio.plus_squelch_native, sizeof(radio.plus_squelch_native)) == 0);
}

static void test_parrot_transitions(void)
{
	struct chan_usbradio_pvt radio = {0};

	radio.rxkeyed = 1;
	radio.plus_parrot_count = 10;
	radio.plus_parrot_play = 8;
	radio.plus_parrot_playing = 1;
	radio.plus_parrot_truncated = 1;
	usbradioplus_parrot_rx_transition(&radio, 0);
	assert(!radio.plus_parrot_count);
	assert(!radio.plus_parrot_play);
	assert(!radio.plus_parrot_playing);
	assert(!radio.plus_parrot_truncated);
	radio.rxkeyed = 0;
	radio.plus_parrot_count = URP_NATIVE_SAMPLES;
	radio.plus_parrot_truncated = 1;
	usbradioplus_parrot_rx_transition(&radio, 1);
	assert(radio.plus_parrot_playing);
	assert(radio.echoing);
	radio.plus_parrot_playing = 0;
	usbradioplus_parrot_rx_transition(&radio, 0);
	assert(!radio.plus_parrot_playing);
}

static void test_program_queue_and_parrot_storage(void)
{
	struct chan_usbradio_pvt radio = {0};
	short samples[200];
	size_t i;

	radio.plus_app_rpt_samples = 160;
	for (i = 0; i < ARRAY_LEN(samples); ++i)
		samples[i] = (short)i;
	assert(!usbradioplus_program_pending(&radio));
	usbradioplus_queue_program(&radio, samples, ARRAY_LEN(samples));
	assert(radio.plus_program_queue.count ==
	       PLUS_LINK_NATIVE_TARGET_SAMPLES / URP_NATIVE_SAMPLES);
	assert(radio.plus_program_queue.high_water == radio.plus_program_queue.count);
	assert(radio.plus_program_queue.frames[radio.plus_program_queue.head][0] == 0);
	assert(radio.plus_program_queue
		       .frames[(radio.plus_program_queue.tail + URP_PROGRAM_QUEUE_FRAMES - 1) %
			       URP_PROGRAM_QUEUE_FRAMES][159] == 159);
	assert(usbradioplus_program_pending(&radio));
	radio.plus_native_fifo.count = 1;
	radio.plus_program_queue.count = 0;
	assert(usbradioplus_program_pending(&radio));

	radio.plus_native_fifo.count = 0;
	radio.plus_native_fifo.primed = 1;
	radio.plus_program_queue.count = URP_PROGRAM_QUEUE_FRAMES;
	radio.plus_program_queue.head = 0;
	radio.plus_program_queue.tail = 0;
	usbradioplus_queue_program(&radio, samples, 1);
	assert(radio.plus_link_queue_overflows == 1);
	assert(radio.plus_program_queue.count == URP_PROGRAM_QUEUE_FRAMES);
	assert(radio.plus_program_queue.head == 1);

	assert(!usbradioplus_native_echo(&radio));
	radio.duplex3 = 999;
	assert(!usbradioplus_native_echo(&radio));
	radio.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	assert(usbradioplus_native_echo(&radio));

	assert(usbradioplus_ensure_parrot_capacity(&radio) == 0);
	assert(radio.plus_parrot);
	assert(radio.plus_parrot_capacity == (size_t)DEFAULT_ECHO_MAX * URP_NATIVE_SAMPLES);
	assert(usbradioplus_ensure_parrot_capacity(&radio) == 0);
	free(radio.plus_parrot);
	radio.plus_parrot = NULL;
	radio.plus_parrot_capacity = 0;
	fail_realloc = 1;
	assert(usbradioplus_ensure_parrot_capacity(&radio) == -1);
	fail_realloc = 0;
}

static void test_native_tick_baseline(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio_config = {0};
	short *capture = (short *)(channel.usbradio_read_buf + AST_FRIENDLY_OFFSET);
	size_t i;

	settings_defaults(&settings);
	settings.enabled = 0;
	channel.name = "test";
	channel.plus_app_rpt_rate = 8000;
	channel.plus_app_rpt_samples = 160;
	channel.plus_hardware_applied = 1;
	channel.plus_emphasis_corner_hz = 300.0;
	radio_config.pRxCodeSrc = "0";
	radio_config.pTxCodeSrc = "0";
	radio_config.pTxCodeDefault = "0";
	channel.radio = urp_radio_create(&radio_config, 160);
	assert(channel.radio);
	assert(usbradioplus_dsp_init(&channel) == 0);
	for (i = 0; i < URP_NATIVE_SAMPLES; i++) {
		capture[i * 2] = (short)(1000.0 * sin(2.0 * M_PI * i / 48.0));
		capture[i * 2 + 1] = 0;
	}
	usbradioplus_native_tick(&channel);
	assert(channel.plus_native_frames == 1);
	assert(channel.plus_adc_peak_dbfs > -40.0);
	assert(channel.plus_app_rpt_samples == 160);
	usbradioplus_dsp_destroy(&channel);
	urp_radio_destroy(channel.radio);
}

int main(void)
{
	test_legacy_option_decoders();
	test_effective_processing_settings();
	test_numeric_helpers();
	test_native_fifo_and_squelch_copy();
	test_parrot_transitions();
	test_program_queue_and_parrot_storage();
	test_native_tick_baseline();
	puts("channel core tests passed");
	return 0;
}

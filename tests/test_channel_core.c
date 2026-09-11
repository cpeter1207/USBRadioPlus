/** @file
 * @brief Executable channel core regression and failure-path checks.
 */

#define URP_CHANNEL_UNIT_TEST 1

#define AST_MODULE_SELF_SYM test_module_self

#define AST_MODULE "chan_usbradioplus"

#include "asterisk.h"
#include <fcntl.h>
#include <search.h>
#include <sys/soundcard.h>
#include <usb.h>
#include "asterisk/audiohook.h"
#include "asterisk/causes.h"
#include "asterisk/cli.h"
#include "asterisk/devicestate.h"
#include "asterisk/frame.h"
#include "asterisk/logger.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/res_usbradio.h"
#include <libavutil/frame.h>
#include <rate_adjusting_pcm_ring.h>
#include <samplerate.h>
#include <sched.h>
#ifdef URP_TEST_MODERN
#include <libusb-1.0/libusb.h>
#include <sys/soundcard.h>
#include <usb.h>
#endif

struct usb_device;
struct usb_dev_handle;
struct usbradioplus_radio_program_request;

AVFrame *test_av_frame_alloc(void);
int test_src_process(SRC_STATE *state, SRC_DATA *data);
SRC_STATE *test_src_new(int converter_type, int channels, int *error);
/** @brief Linker entry point for the real av_frame_alloc operation behind the test wrapper.
 * @return Wrapped API result, including the failure selected by the harness.
 */
AVFrame *__real_av_frame_alloc(void);
/** @brief Linker entry point for the real src_process operation behind the test wrapper.
 * @param state Processor or stream state owned by the caller.
 * @param data Input payload or owned state being released, as declared.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __real_src_process(SRC_STATE *state, SRC_DATA *data);
/** @brief Linker entry point for the real src_new operation behind the test wrapper.
 * @param converter_type libsamplerate converter type.
 * @param channels Number of interleaved audio channels.
 * @param error Receives a diagnostic for invalid input.
 * @return Wrapped API result, including the failure selected by the harness.
 */
SRC_STATE *__real_src_new(int converter_type, int channels, int *error);
/** @brief Linker entry point for the shared playout-ring initializer.
 * @param ring Ring state to initialize.
 * @param capacity Sample capacity.
 * @param quality Converter quality selection.
 * @return Zero on success or nonzero when allocation fails.
 */
int __real_rpcr_init(struct rpcr_ring *ring, size_t capacity, enum rpcr_quality quality);
/** @brief Linker entry point for the shared playout-ring rate setup.
 * @param ring Ring state to configure.
 * @param input_rate Producer sample rate in Hz.
 * @param output_rate Consumer sample rate in Hz.
 * @return Zero on success or nonzero when rate setup fails.
 */
int __real_rpcr_set_rates(struct rpcr_ring *ring, unsigned int input_rate,
			  unsigned int output_rate);
/** @brief Linker entry point for the real pthread_join operation behind the test wrapper.
 * @param thread Worker thread identifier supplied by the harness.
 * @param result Receives the parsed value or supplies a CLI result, as declared.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __real_pthread_join(pthread_t thread, void **result);
int __wrap_usleep(unsigned int microseconds);
int __wrap_ioctl(int descriptor, unsigned long request, ...);
int __wrap_open(const char *path, int flags, ...);
int __wrap_close(int descriptor);
int __wrap_fcntl(int descriptor, int command, ...);
ssize_t __wrap_read(int descriptor, void *buffer, size_t count);
ssize_t __wrap_write(int descriptor, const void *buffer, size_t count);
int test_pthread_join(pthread_t thread, void **result);
int __wrap_poll(struct pollfd *descriptors, nfds_t count, int timeout);
int __wrap_pipe(int descriptors[2]);
int __wrap_pipe2(int descriptors[2], int flags);
int __wrap_ioperm(unsigned long from, unsigned long count, int turn_on);
struct usb_dev_handle *__wrap_usb_open(struct usb_device *device);
int __wrap_usb_close(struct usb_dev_handle *handle);
int __wrap_usb_claim_interface(struct usb_dev_handle *handle, int interface_number);
int __wrap_usb_detach_kernel_driver_np(struct usb_dev_handle *handle, int interface_number);
void test_ast_debug(int level, const char *format, ...);
void test_ast_log(int level, const char *format, ...);
#ifdef URP_TEST_MODERN
static struct chan_usbradio_pvt *modern_stop_hid_on_radio_time;
#endif
#ifdef URP_TEST_MODERN
int __wrap_libusb_open(libusb_device *device, libusb_device_handle **handle);
void __wrap_libusb_close(libusb_device_handle *handle);
int __wrap_libusb_claim_interface(libusb_device_handle *handle, int interface_number);
int __wrap_libusb_detach_kernel_driver(libusb_device_handle *handle, int interface_number);
const PaStreamInfo *__wrap_Pa_GetStreamInfo(PaStream *stream);
void hidthread_apply_ptt(struct chan_usbradio_pvt *o, struct libusb_device_handle *usb_handle,
			 unsigned char *buf, unsigned char *bufsave, int asserted,
			 const struct usbradioplus_radio_program_request *request, int reprogram);
#endif
#include "../src/txagc/avfilter_processor.h"
#include "../src/txagc/rnnoise_processor.h"
#include "../src/usbradioplus_channel_core.h"
#include "../src/usbradioplus_config.h"
#include "../src/usbradioplus_ctcss.h"
#include "../src/usbradioplus_dsp.h"
#include "../src/usbradioplus_hardware.h"
#include "../src/usbradioplus_processing.h"
#include "../src/usbradioplus_radio.h"
#include "../src/usbradioplus_repeat.h"
#include "../src/usbradioplus_channel_test_api.h"
#include "../src/usbradioplus_processing_internal.h"

#include <assert.h>
#include <limits.h>

/** @brief Linker entry point for the shared FFmpeg graph preparation operation. */
int __real_txagc_avfilter_prepare(struct txagc_avfilter *state, const struct txagc_config *config,
				  unsigned int sample_rate);
/** @brief Linker entry point for prepared FFmpeg graph processing. */
int __real_txagc_avfilter_process_prepared(struct txagc_avfilter *state, double *samples,
					   size_t sample_count);

/** @brief Linker entry point for one resolved processing option lookup. */
int __real_usbradioplus_processing_get_option(const char *channel, const char *section,
					      const char *name, char *value, size_t value_size);

/** @brief Linker entry point for one resolved hardware-profile lookup. */
int __real_usbradioplus_processing_get_hardware(const char *channel,
						struct usbradioplus_hardware_settings *hardware);

/** @brief Linker entry point for one resolved composite processing-chain lookup. */
int __real_usbradioplus_processing_get_composite(const char *channel, struct txagc_chain *chain);

/** @brief Linker entry point for the real ioctl operation behind the test wrapper.
 * @param descriptor Test file or CLI descriptor.
 * @param request Host I/O or locking operation.
 * @param ... Values required by the wrapped variadic API.
 * @return Wrapped API result, including the failure selected by the harness.
 */
extern int __real_ioctl(int descriptor, unsigned long request, ...);
/** @brief Linker entry point for the real open operation behind the test wrapper.
 * @param path Filesystem path to inspect or update.
 * @param flags Host API option bit mask.
 * @param ... Values required by the wrapped variadic API.
 * @return Wrapped API result, including the failure selected by the harness.
 */
extern int __real_open(const char *path, int flags, ...);
/** @brief Linker entry point for the real close operation behind the test wrapper.
 * @param descriptor Test file or CLI descriptor.
 * @return Wrapped API result, including the failure selected by the harness.
 */
extern int __real_close(int descriptor);
/** @brief Linker entry point for the real fcntl operation behind the test wrapper.
 * @param descriptor Test file or pipe descriptor.
 * @param command Requested descriptor operation.
 * @param ... Optional command argument, as required by the host API.
 * @return Wrapped API result, including the failure selected by the harness.
 */
extern int __real_fcntl(int descriptor, int command, ...);
/** @brief Linker entry point for the real read operation behind the test wrapper.
 * @param descriptor Test file or CLI descriptor.
 * @param buffer Caller-owned buffer filled or consumed by the stub.
 * @param count Number of elements available in the supplied block.
 * @return Wrapped API result, including the failure selected by the harness.
 */
extern ssize_t __real_read(int descriptor, void *buffer, size_t count);
/** @brief Linker entry point for the real write operation behind the test wrapper.
 * @param descriptor Test file or CLI descriptor.
 * @param buffer Caller-owned buffer filled or consumed by the stub.
 * @param count Number of elements available in the supplied block.
 * @return Wrapped API result, including the failure selected by the harness.
 */
extern ssize_t __real_write(int descriptor, const void *buffer, size_t count);
/** @brief Host-API test double for pthread_join; observable effects are recorded in harness state.
 * @param thread Worker thread identifier supplied by the harness.
 * @param result Receives the parsed value or supplies a CLI result, as declared.
 * @return Scripted host result for the current test scenario.
 */
extern int pthread_join(pthread_t thread, void **result);
/** @brief Linker entry point for the real poll operation behind the test wrapper.
 * @param descriptors Test descriptor array.
 * @param count Number of elements available in the supplied block.
 * @param timeout Asterisk call timeout.
 * @return Wrapped API result, including the failure selected by the harness.
 */
extern int __real_poll(struct pollfd *descriptors, nfds_t count, int timeout);
/** @brief Linker entry point for the real pipe operation behind the test wrapper.
 * @param descriptors Test descriptor array.
 * @return Wrapped API result, including the failure selected by the harness.
 */
extern int __real_pipe(int descriptors[2]);

/** Last opt-in timing record emitted by the shared channel code. */
static char tx_trace_message[512];

void test_ast_debug(int level, const char *format, ...)
{
	(void)level;
	(void)format;
}

void test_ast_log(int level, const char *format, ...)
{
	(void)level;
	(void)format;
}

/** @brief Verify pthread join.
 * @param thread Worker thread identifier supplied by the harness.
 * @param result Receives the parsed value or supplies a CLI result, as declared.
 * @return Result used by the test's assertions.
 */
int test_pthread_join(pthread_t thread, void **result)
{
	(void)thread;
	(void)result;
	return 0;
}

#undef pthread_mutex_lock
#undef pthread_mutex_unlock
#undef free
#undef malloc
#undef realloc
#undef calloc

/** Harness ast options used to script and verify host behavior. */
struct ast_flags64 ast_options;
/** Harness ast null frame used to script and verify host behavior. */
struct ast_frame ast_null_frame;
/** Harness ast format slin used to script and verify host behavior. */
struct ast_format *ast_format_slin;
/** @brief Verify module self.
 * @return Result used by the test's assertions.
 */
struct ast_module *test_module_self(void)
{
	return NULL;
}
/** Harness option debug used to script and verify host behavior. */
int option_debug;
/** Harness option verbose used to script and verify host behavior. */
int option_verbose;
/** Controls injected realloc failure for this test. */
static int fail_realloc;
/** Reallocation call selected for a deterministic allocation failure. */
static int fail_realloc_call;
/** Number of host reallocation calls observed by the test harness. */
static int realloc_calls;
/** Recorded ast calloc calls for assertions. */
static int ast_calloc_calls;
/** Controls injected ast calloc call failure for this test. */
static int fail_ast_calloc_call;
/** Controls injected radio state allocation failure for this test. */
static int fail_radio_state_allocation;
/** Controls injected av frame alloc call failure for this test. */
static int fail_av_frame_alloc_call;
/** Recorded av frame alloc calls for assertions. */
static int av_frame_alloc_calls;
/** Controls injected FFmpeg graph-prepare failure for this test. */
static int fail_avfilter_prepare_call;
/** Recorded FFmpeg graph-prepare calls for assertions. */
static int avfilter_prepare_calls;
/** Prepared FFmpeg graph selected to fail during a native callback. */
static struct txagc_avfilter *fail_avfilter_process_state;
/** Prepared graph whose use is counted by a native callback assertion. */
static const struct txagc_avfilter *observed_avfilter_process_state;
/** Calls received by the prepared graph selected for observation. */
static unsigned int observed_avfilter_process_calls;
/** Injects one resolved-option lookup failure for parser transaction tests. */
static int fail_processing_option_get_call;
/** Counts resolved-option lookups made by a parser transaction. */
static int processing_option_get_calls;
/** Injects one hardware-profile lookup failure for parser transaction tests. */
static int fail_processing_hardware_get_call;
/** Counts hardware-profile lookups made by a parser transaction. */
static int processing_hardware_get_calls;
/** Injects one composite-chain lookup failure for graph transaction tests. */
static int fail_processing_composite_get_call;
/** Counts composite-chain lookups made by a graph transaction. */
static int processing_composite_get_calls;
/** Controls injected src process call failure for this test. */
static int fail_src_process_call;
/** Controls injected src new call failure for this test. */
static int fail_src_new_call;
/** Recorded src new calls for assertions. */
static int src_new_calls;
/** Controls injected shared playout-ring initialization failure for this test. */
static int fail_rpcr_init_call;
/** Recorded shared playout-ring initialization calls for assertions. */
static int rpcr_init_calls;
/** Controls injected shared playout-ring rate-setup failure for this test. */
static int fail_rpcr_set_rates;
/** Recorded src process calls for assertions. */
static int src_process_calls;
/** Harness partial src process call used to script and verify host behavior. */
static int partial_src_process_call;
/** Harness partial src output call used to verify protected FIFO remainder handling. */
static int partial_src_output_call;
/** Harness config variables used to script and verify host behavior. */
static struct ast_variable *test_config_variables;
/** Harness config load result used to script and verify host behavior. */
static struct ast_config *test_config_load_result;
/** Harness separate processing config result used to script and verify host behavior. */
static int separate_processing_config_result;
/** Harness processing config load result used to script and verify host behavior. */
static struct ast_config *test_processing_config_load_result;
/** Harness config load second result used to script and verify host behavior. */
static struct ast_config *test_config_load_second_result;
/** Recorded test config load calls for assertions. */
static int test_config_load_calls;
/** Recorded config destroy calls for assertions. */
static int config_destroy_calls;
/** Harness channel private used to script and verify host behavior. */
static void *test_channel_private;
/** Recorded setstate calls for assertions. */
static int setstate_calls;
/** Recorded moh start calls for assertions. */
static int moh_start_calls;
/** Recorded moh stop calls for assertions. */
static int moh_stop_calls;
/** Recorded usleep calls for assertions. */
static int usleep_calls;
/** Recorded wait or poll calls for assertions. */
static int wait_or_poll_calls;
/** Harness wait or poll fail call used to script and verify host behavior. */
static int wait_or_poll_fail_call;
/** Harness poll successes before exit used to script and verify host behavior. */
static int poll_successes_before_exit;
/** Recorded audio-statistics display calls for assertions. */
static int radio_print_audio_stats_calls;
/** Harness toggle rxkey radio used to script and verify host behavior. */
static struct chan_usbradio_pvt *toggle_rxkey_radio;
/** Harness scripted measure stage used to script and verify host behavior. */
static urp_radio_stage *scripted_measure_stage;
/** Harness scripted measurements used to script and verify host behavior. */
static int scripted_measurements[32];
/** Recorded scripted measurement count for assertions. */
static size_t scripted_measurement_count;
/** Harness scripted measurement index used to script and verify host behavior. */
static size_t scripted_measurement_index;
/** Harness variable update result used to script and verify host behavior. */
static int variable_update_result;
/** Harness variable new failure used to script and verify host behavior. */
static int variable_new_failure;
/** Recorded variable append calls for assertions. */
static int variable_append_calls;
/** Most recent updated configuration variable name. */
static char updated_variable_name[64];
/** Most recent updated configuration variable value. */
static char updated_variable_value[512];
/** Recorded variable browse calls for assertions. */
static int variable_browse_calls;
/** Harness inject invalid override on browse call used to script and verify host behavior. */
static int inject_invalid_override_on_browse_call;
/** Harness category get result used to script and verify host behavior. */
static struct ast_category *test_category_get_result = (struct ast_category *)(uintptr_t)1;
/** Harness config save result used to script and verify host behavior. */
static int config_save_result;
/** Recorded parallel write calls for assertions. */
static int parallel_write_calls;
/** Optional configured parallel PTT mask observed by the output test double. */
static unsigned char mock_parallel_ptt_mask;
/** Nonzero once the observed configured parallel PTT mask was physically asserted. */
static int parallel_saw_ptt_asserted;
/** Harness jitter config result used to script and verify host behavior. */
static int jitter_config_result;
/** Recorded ast strdup calls for assertions. */
static int ast_strdup_calls;
/** Controls injected ast strdup call failure for this test. */
static int fail_ast_strdup_call;
/** Harness clear eeprom on usleep used to script and verify host behavior. */
static int clear_eeprom_on_usleep;
/** Harness clear eeprom target used to script and verify host behavior. */
static struct chan_usbradio_pvt *clear_eeprom_target;
/** Harness stop pulser on usleep used to script and verify host behavior. */
static int stop_pulser_on_usleep;
/** Harness stop hid radio on usleep used to script and verify host behavior. */
static struct chan_usbradio_pvt *stop_hid_radio_on_usleep;
/** Harness stop hid after usleeps used to script and verify host behavior. */
static int stop_hid_after_usleeps;
/** Recorded stop hid usleep count for assertions. */
static int stop_hid_usleep_count;
#ifdef URP_TEST_MODERN
static struct chan_usbradio_pvt *modern_swap_first;
static struct chan_usbradio_pvt *modern_swap_second;
static int modern_swap_wait_step;
static int modern_swap_staggered;
static struct chan_usbradio_pvt *modern_stop_audio_on_usleep;
static struct chan_usbradio_pvt *modern_drop_hasusb_on_time;
static struct chan_usbradio_pvt *modern_drop_hasusb_on_queue;
static int modern_radio_time_calls;
static int modern_drop_hasusb_on_time_call;
#endif
/** Controls injected channel alloc failure for this test. */
static int fail_channel_alloc;
/** Harness pbx start result used to script and verify host behavior. */
static int pbx_start_result;
/** Harness format compatible used to script and verify host behavior. */
static int format_compatible = 1;
/** Recorded hangup calls for assertions. */
static int hangup_calls;
/** Harness channel state used to script and verify host behavior. */
static enum ast_channel_state channel_state = AST_STATE_UP;
/** Harness dsp result type used to script and verify host behavior. */
static int dsp_result_type = -1;
/** Harness dsp result digit used to script and verify host behavior. */
static int dsp_result_digit;
/** Recorded frame free calls for assertions. */
static int frame_free_calls;
/** Harness config category used to script and verify host behavior. */
static char *test_config_category;
/** Recorded parallel load calls for assertions. */
static int parallel_load_calls;
/** Harness module debug level used to script and verify host behavior. */
static unsigned int module_debug_level;
/** Harness file debug level used to script and verify host behavior. */
static unsigned int file_debug_level;
/** Harness installed usb device used to script and verify host behavior. */
static const char *installed_usb_device;
/** Harness oss io used to script and verify host behavior. */
static int mock_oss_io;
/** Harness open result used to script and verify host behavior. */
static int mock_open_result = 7;
/** Harness ioctl failure used to script and verify host behavior. */
static unsigned long mock_ioctl_failure;
/** Harness oss fragments used to script and verify host behavior. */
static int mock_oss_fragments = 8;
/** Harness oss fragment total used to script and verify host behavior. */
static int mock_oss_fragment_total = 8;
/** Harness OSS fragment size used to script output-space accounting. */
static int mock_oss_fragment_size = 3840;
/** Counts output-space queries made by the OSS test wrapper. */
static int mock_oss_output_space_calls;
/** Fails one numbered output-space query in the OSS test wrapper. */
static int fail_oss_output_space_call;
/** Makes one output-space query report a capacity that saturates callback accounting. */
static int extreme_oss_output_space_call;
/** Optional scripted OSS output-space byte count; negative follows fragments. */
static int mock_oss_bytes = -1;
/** Optional scripted OSS queued-byte delay; negative makes GETODELAY fail. */
static int mock_oss_output_delay = -1;
/** Harness oss speed used to script and verify host behavior. */
static int mock_oss_speed = 48000;
/** Harness oss caps used to script and verify host behavior. */
static int mock_oss_caps = DSP_CAP_DUPLEX;
/** Harness read result used to script and verify host behavior. */
static ssize_t mock_read_result = -1;
/** Harness read errno used to script and verify host behavior. */
static int mock_read_errno = EAGAIN;
/** Harness write result used to script and verify host behavior. */
static int mock_write_result = -2;
/** OSS audio descriptor observed separately from PTT kick-pipe writes. */
static int mock_sound_descriptor = 7;
/** Number of mocked sound writes, including failed and short attempts. */
static unsigned int mock_sound_write_calls;
/** Byte count requested by the most recent mocked sound write. */
static size_t mock_sound_write_bytes;
/** Copy of the most recent mocked sound write's native stereo payload. */
static short mock_sound_write_data[FRAME_SIZE * 2 * 6];
/** Recorded mock close calls for assertions. */
static int mock_close_calls;
/** Recorded pthread create calls for assertions. */
static int pthread_create_calls;
/** Controls injected pthread create call failure for this test. */
static int fail_pthread_create_call;
#ifndef URP_TEST_MODERN
/** Harness usb device used to script and verify host behavior. */
static struct usb_device mock_usb_device;
/** Harness hid device available used to script and verify host behavior. */
static int mock_hid_device_available;
#endif
/** Harness usb open success used to script and verify host behavior. */
static int mock_usb_open_success;
/** Harness usb claim result used to script and verify host behavior. */
static int mock_usb_claim_result;
/** Harness usb second claim result used to script and verify host behavior. */
static int mock_usb_second_claim_result;
/** Recorded mock usb claim calls for assertions. */
static int mock_usb_claim_calls;
/** Harness usb detach result used to script and verify host behavior. */
static int mock_usb_detach_result;
#ifndef URP_TEST_MODERN
/** Harness stop hid on input used to script and verify host behavior. */
static struct chan_usbradio_pvt *stop_hid_on_input;
/**
 * Harness channel that publishes an unkey while its program snapshot is intentionally
 * unavailable.  This models a control-plane update in progress without another thread.
 */
static struct chan_usbradio_pvt *unkey_without_program_on_input;
/** Input-report count at which the unavailable-program unkey is published. */
static int unkey_without_program_after_inputs;
/**
 * Harness channel that publishes a PTT assertion while its program snapshot is intentionally
 * unavailable.  This proves signaling publication never prevents a physical key-up.
 */
static struct chan_usbradio_pvt *key_without_program_on_input;
/** Input-report count at which the unavailable-program key-up is published. */
static int key_without_program_after_inputs;
/** Harness channel that publishes a fresh program and a PTT assertion after HID setup. */
static struct chan_usbradio_pvt *program_and_key_on_input;
/** Input-report count at which the combined program and key-up is published. */
static int program_and_key_after_inputs;
/** Harness channel that publishes a PTT assertion after the current program is consumed. */
static struct chan_usbradio_pvt *key_after_program_on_input;
/** Input-report count at which the steady-program key-up is published. */
static int key_after_program_after_inputs;
/** Harness drop hid on input used to script and verify host behavior. */
static struct chan_usbradio_pvt *drop_hid_on_input;
/** Harness eeprom result used to script and verify host behavior. */
static unsigned short mock_eeprom_result;
/** Harness eeprom valid magic used to script and verify host behavior. */
static int mock_eeprom_valid_magic;
/** Harness eeprom write after read used to script and verify host behavior. */
static struct chan_usbradio_pvt *eeprom_write_after_read;
#endif
/** Controls injected format cap alloc failure for this test. */
static int fail_format_cap_alloc;
/** Harness channel register result used to script and verify host behavior. */
static int channel_register_result;
/** Inject failure only for the additional native channel registration. */
static int advanced_register_result;
/** Last registered advanced channel technology. */
static const struct ast_channel_tech *advanced_technology;
/** Inject format capability append failure. */
static int format_append_result;
/** Inject native receive-format selection failure. */
static int set_read_result;
/** Inject native transmit-format selection failure. */
static int set_write_result;
/** Recorded channel unregister calls for assertions. */
static int channel_unregister_calls;
/** Recorded cli register calls for assertions. */
static int cli_register_calls;
/** Harness cli register result used to script and verify host behavior. */
static int cli_register_result;
/** Recorded cli unregister calls for assertions. */
static int cli_unregister_calls;
/** Harness hid mklist result used to script and verify host behavior. */
static int hid_mklist_result;
/** Harness usb device number used to script and verify host behavior. */
static int mock_usb_device_number;
/** Harness usb serial result used to script and verify host behavior. */
static int mock_usb_serial_result;
/** Harness usb serial used to script and verify host behavior. */
static const char *mock_usb_serial = "serial-test";
/** Harness usb serial by device used to script and verify host behavior. */
static int mock_usb_serial_by_device;
/** Harness second usb device used to script and verify host behavior. */
static int mock_second_usb_device;
/** Harness no usb devices used to script and verify host behavior. */
static int mock_no_usb_devices;
#ifndef URP_TEST_MODERN
/** Harness amixer max used to script and verify host behavior. */
static int mock_amixer_max = 100;
/** Harness new mixer name used to script and verify host behavior. */
static int mock_new_mixer_name;
#endif
/** Harness dsp available used to script and verify host behavior. */
static int mock_dsp_available;
/** Recorded ast_dsp_free calls for lifecycle assertions. */
static int ast_dsp_free_calls;
/** Harness poll enabled used to script and verify host behavior. */
static int mock_poll_enabled;
/** Harness poll result used to script and verify host behavior. */
static int mock_poll_result;
/** Harness poll revents used to script and verify host behavior. */
static short mock_poll_revents;
#ifndef URP_TEST_MODERN
/** Harness hid inputs used to script and verify host behavior. */
static unsigned char mock_hid_inputs[4];
/** Recorded mock hid input calls for assertions. */
static int mock_hid_input_calls;
/** Harness stop hid after inputs used to script and verify host behavior. */
static int stop_hid_after_inputs = 1;
/** Harness toggle hid input index used to script and verify host behavior. */
static int toggle_hid_input_index;
/** Harness toggle hid inputs mask used to script and verify host behavior. */
static unsigned char toggle_hid_inputs_mask;
#endif
/** Harness parallel inputs used to script and verify host behavior. */
static unsigned char mock_parallel_inputs;
/** Harness toggle parallel inputs mask used to script and verify host behavior. */
static unsigned char toggle_parallel_inputs_mask;
/** Harness tvnow milliseconds used to script and verify host behavior. */
static long mock_tvnow_milliseconds = 1000;
/** Harness tvnow step used to script and verify host behavior. */
static long mock_tvnow_step;
/** Harness audio clipping used to script and verify host behavior. */
static int mock_audio_clipping;
/** Harness pipe failure used to script and verify host behavior. */
static int mock_pipe_failure;
/** Calls to the wrapped fcntl operation before the selected injected failure. */
static int mock_fcntl_calls;
/** Positive call number that fails the wrapped fcntl operation. */
static int mock_fcntl_fail_call;

/** @brief Record a mocked sound write without modifying the caller's samples.
 * @param buffer Samples supplied to the output API.
 * @param count Number of bytes requested from the output API.
 */
static void record_mock_sound_write(const void *buffer, size_t count)
{
	mock_sound_write_calls++;
	mock_sound_write_bytes = count;
	memcpy(mock_sound_write_data, buffer,
	       count < sizeof(mock_sound_write_data) ? count : sizeof(mock_sound_write_data));
}

/** @brief Test wrapper for pipe controlled by the harness's failure-injection state.
 * @param descriptors Test descriptor array.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_pipe(int descriptors[2])
{
	if (mock_pipe_failure) {
		errno = EMFILE;
		return -1;
	}
	return __real_pipe(descriptors);
}

/** @brief Test wrapper for pipe2 controlled by the harness's failure-injection state.
 * @param descriptors Test descriptor array.
 * @param flags Host API option bit mask.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_pipe2(int descriptors[2], int flags)
{
	(void)flags;
	return __wrap_pipe(descriptors);
}

/** @brief Test wrapper for fcntl controlled by the harness's failure-injection state.
 *
 * The legacy HID worker uses F_GETFL followed by F_SETFL while making its wake pipe
 * nonblocking.  Forward those two command forms to the real descriptor so successful
 * worker tests retain kernel semantics; only the explicitly selected call fails.
 * @param descriptor Test file or pipe descriptor.
 * @param command Requested descriptor operation.
 * @param ... Optional command argument, as required by the host API.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_fcntl(int descriptor, int command, ...)
{
	va_list arguments;
	int result;

	mock_fcntl_calls++;
	if (mock_fcntl_fail_call && mock_fcntl_calls == mock_fcntl_fail_call) {
		errno = EIO;
		return -1;
	}
	if (command == F_GETFL)
		return __real_fcntl(descriptor, command);
	va_start(arguments, command);
	result = __real_fcntl(descriptor, command, va_arg(arguments, int));
	va_end(arguments);
	return result;
}

/** @brief Test wrapper for ioperm controlled by the harness's failure-injection state.
 * @param from From supplied by the test scenario.
 * @param count Number of elements available in the supplied block.
 * @param turn_on Turn on supplied by the test scenario.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_ioperm(unsigned long from, unsigned long count, int turn_on)
{
	(void)from;
	(void)count;
	(void)turn_on;
	return 0;
}

/** @brief Test wrapper for poll controlled by the harness's failure-injection state.
 * @param descriptors Test descriptor array.
 * @param count Number of elements available in the supplied block.
 * @param timeout Asterisk call timeout.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_poll(struct pollfd *descriptors, nfds_t count, int timeout)
{
	if (mock_poll_enabled) {
		(void)timeout;
		if (count)
			descriptors[0].revents = mock_poll_revents;
		return mock_poll_result;
	}
	return __real_poll(descriptors, count, timeout);
}

/** @brief Test wrapper for usb_open controlled by the harness's failure-injection state.
 * @param device Channel name or USB device identifier.
 * @return Wrapped API result, including the failure selected by the harness.
 */
struct usb_dev_handle *__wrap_usb_open(struct usb_device *device)
{
	(void)device;
	return mock_usb_open_success ? (struct usb_dev_handle *)(uintptr_t)1 : NULL;
}

/** @brief Test wrapper for usb_close controlled by the harness's failure-injection state.
 * @param handle Simulated acquired USB handle.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_usb_close(struct usb_dev_handle *handle)
{
	(void)handle;
	return 0;
}

/** @brief Test wrapper for usb_claim_interface controlled by the harness's failure-injection state.
 * @param handle Simulated acquired USB handle.
 * @param interface_number Interface number supplied by the test scenario.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_usb_claim_interface(struct usb_dev_handle *handle, int interface_number)
{
	(void)handle;
	(void)interface_number;
	mock_usb_claim_calls++;
	return mock_usb_claim_calls == 1 ? mock_usb_claim_result : mock_usb_second_claim_result;
}

/** @brief Test wrapper for usb_detach_kernel_driver_np controlled by the harness's
 * failure-injection state.
 * @param handle Simulated acquired USB handle.
 * @param interface_number Interface number supplied by the test scenario.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_usb_detach_kernel_driver_np(struct usb_dev_handle *handle, int interface_number)
{
	(void)handle;
	(void)interface_number;
	return mock_usb_detach_result;
}

/** @brief Host-API test double for __ast_format_cap_alloc; observable effects are recorded in
 * harness state.
 * @param flags Host API option bit mask.
 * @param tag Host reference-tracking tag.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @return Scripted host result for the current test scenario.
 */
struct ast_format_cap *__ast_format_cap_alloc(enum ast_format_cap_flags flags, const char *tag,
					      const char *file, int line, const char *function)
{
	(void)flags;
	(void)tag;
	(void)file;
	(void)line;
	(void)function;
	return fail_format_cap_alloc ? NULL : (struct ast_format_cap *)(uintptr_t)1;
}

/** @brief Host-API test double for __ast_format_cap_append; observable effects are recorded in
 * harness state.
 * @param capabilities Asterisk audio-format capabilities.
 * @param format printf-style message format.
 * @param framing Requested Asterisk audio-frame duration.
 * @param tag Host reference-tracking tag.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @return Scripted host result for the current test scenario.
 */
int __ast_format_cap_append(struct ast_format_cap *capabilities, struct ast_format *format,
			    unsigned int framing, const char *tag, const char *file, int line,
			    const char *function)
{
	(void)capabilities;
	(void)format;
	(void)framing;
	(void)tag;
	(void)file;
	(void)line;
	(void)function;
	return format_append_result;
}

/** @brief Host-API test double for ast_channel_register; observable effects are recorded in harness
 * state.
 * @param technology Asterisk channel technology callbacks.
 * @return Scripted host result for the current test scenario.
 */
int ast_channel_register(const struct ast_channel_tech *technology)
{
	if (!strcmp(technology->type, "RadioPlusAdvanced")) {
		advanced_technology = technology;
		if (advanced_register_result)
			return advanced_register_result;
	}
	return channel_register_result;
}

/** @brief Host-API test double for ast_channel_unregister; observable effects are recorded in
 * harness state.
 * @param technology Asterisk channel technology callbacks.
 */
void ast_channel_unregister(const struct ast_channel_tech *technology)
{
	(void)technology;
	channel_unregister_calls++;
}

/** @brief Host-API test double for __ast_cli_register_multiple; observable effects are recorded in
 * harness state.
 * @param entries Entries supplied by the test scenario.
 * @param count Number of elements available in the supplied block.
 * @param module Asterisk module reference.
 * @return Scripted host result for the current test scenario.
 */
int __ast_cli_register_multiple(struct ast_cli_entry *entries, int count, struct ast_module *module)
{
	(void)entries;
	(void)count;
	(void)module;
	cli_register_calls++;
	return cli_register_result;
}

/** @brief Host-API test double for ast_cli_unregister_multiple; observable effects are recorded in
 * harness state.
 * @param entries Entries supplied by the test scenario.
 * @param count Number of elements available in the supplied block.
 * @return Scripted host result for the current test scenario.
 */
int ast_cli_unregister_multiple(struct ast_cli_entry *entries, int count)
{
	(void)entries;
	(void)count;
	cli_unregister_calls++;
	return 0;
}

/** @brief Host-API test double for ast_dsp_free; observable effects are recorded in harness state.
 * @param dsp Dsp supplied by the test scenario.
 */
void ast_dsp_free(struct ast_dsp *dsp)
{
	(void)dsp;
	ast_dsp_free_calls++;
}

/** @brief Host-API test double for ast_softhangup; observable effects are recorded in harness
 * state.
 * @param channel Radio channel or channel index, as declared.
 * @param cause Receives the Asterisk failure cause when channel creation fails.
 * @return Scripted host result for the current test scenario.
 */
int ast_softhangup(struct ast_channel *channel, int cause)
{
	(void)channel;
	(void)cause;
	return 0;
}

/** @brief Host-API test double for __ao2_cleanup_debug; observable effects are recorded in harness
 * state.
 * @param object Host reference-counted object.
 * @param tag Host reference-tracking tag.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 */
void __ao2_cleanup_debug(void *object, const char *tag, const char *file, int line,
			 const char *function)
{
	(void)object;
	(void)tag;
	(void)file;
	(void)line;
	(void)function;
}

/** @brief Test wrapper for ioctl controlled by the harness's failure-injection state.
 * @param descriptor Test file or CLI descriptor.
 * @param request Host I/O or locking operation.
 * @param ... Values required by the wrapped variadic API.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_ioctl(int descriptor, unsigned long request, ...)
{
	va_list arguments;
	void *argument;
	(void)descriptor;
	if (!mock_oss_io) {
		va_start(arguments, request);
		argument = va_arg(arguments, void *);
		va_end(arguments);
		return __real_ioctl(descriptor, request, argument);
	}
	if (request == mock_ioctl_failure)
		return -1;
	if (request == SNDCTL_DSP_RESET || request == SNDCTL_DSP_SETDUPLEX ||
	    request == SNDCTL_DSP_SETTRIGGER)
		return 0;
	va_start(arguments, request);
	argument = va_arg(arguments, void *);
	va_end(arguments);
	if (request == SNDCTL_DSP_GETOSPACE) {
		struct audio_buf_info *info = argument;

		mock_oss_output_space_calls++;
		if (fail_oss_output_space_call == mock_oss_output_space_calls)
			return -1;
		memset(info, 0, sizeof(*info));
		if (extreme_oss_output_space_call == mock_oss_output_space_calls) {
			info->fragstotal = INT_MAX;
			info->fragsize = INT_MAX;
			return 0;
		}
		info->fragstotal = mock_oss_fragment_total;
		info->fragments = mock_oss_fragments;
		info->fragsize = mock_oss_fragment_size;
		info->bytes =
			mock_oss_bytes >= 0 ? mock_oss_bytes : info->fragments * info->fragsize;
		return 0;
	}
#ifdef SNDCTL_DSP_GETODELAY
	if (request == SNDCTL_DSP_GETODELAY) {
		if (mock_oss_output_delay == INT_MIN) {
			*(int *)argument = -1;
			return 0;
		}
		if (mock_oss_output_delay < 0)
			return -1;
		*(int *)argument = mock_oss_output_delay;
		return 0;
	}
#endif
	if (request == SNDCTL_DSP_GETCAPS)
		*(int *)argument = mock_oss_caps;
	else if (request == SNDCTL_DSP_SPEED)
		*(int *)argument = mock_oss_speed;
	return 0;
}

/** @brief Test wrapper for open controlled by the harness's failure-injection state.
 * @param path Filesystem path to inspect or update.
 * @param flags Host API option bit mask.
 * @param ... Values required by the wrapped variadic API.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_open(const char *path, int flags, ...)
{
	if (mock_oss_io) {
		(void)path;
		(void)flags;
		return mock_open_result;
	}
	return __real_open(path, flags);
}

/** @brief Test wrapper for close controlled by the harness's failure-injection state.
 * @param descriptor Test file or CLI descriptor.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_close(int descriptor)
{
	if (mock_oss_io) {
		mock_close_calls++;
		return 0;
	}
	return __real_close(descriptor);
}

/** @brief Test wrapper for read controlled by the harness's failure-injection state.
 * @param descriptor Test file or CLI descriptor.
 * @param buffer Caller-owned buffer filled or consumed by the stub.
 * @param count Number of elements available in the supplied block.
 * @return Wrapped API result, including the failure selected by the harness.
 */
ssize_t __wrap_read(int descriptor, void *buffer, size_t count)
{
	ssize_t result;

	if (mock_oss_io) {
		result = mock_read_result;
		errno = mock_read_errno;
		if (result > 0) {
			size_t bytes = (size_t)result;
			memset(buffer, 0, bytes < count ? bytes : count);
		}
		/* The HID worker drains its nonblocking kick pipe until EAGAIN.  A
		 * scripted readable pipe therefore supplies one byte, then becomes empty;
		 * replaying a positive result forever would make the worker spin inside
		 * that drain loop instead of reaching its teardown path. */
		if (descriptor != mock_sound_descriptor && result > 0) {
			mock_read_result = -1;
			mock_read_errno = EAGAIN;
		}
		return result;
	}
	return __real_read(descriptor, buffer, count);
}

/** @brief Test wrapper for write controlled by the harness's failure-injection state.
 * @param descriptor Test file or CLI descriptor.
 * @param buffer Caller-owned buffer filled or consumed by the stub.
 * @param count Number of elements available in the supplied block.
 * @return Wrapped API result, including the failure selected by the harness.
 */
ssize_t __wrap_write(int descriptor, const void *buffer, size_t count)
{
	if (mock_oss_io) {
		if (descriptor == mock_sound_descriptor)
			record_mock_sound_write(buffer, count);
		if (mock_write_result != -2)
			return mock_write_result;
		return (ssize_t)count;
	}
	return __real_write(descriptor, buffer, count);
}

/** @brief Host-API test double for ast_jb_read_conf; observable effects are recorded in harness
 * state.
 * @param conf Conf supplied by the test scenario.
 * @param varname Varname supplied by the test scenario.
 * @param value Input value or writable result, as declared.
 * @return Scripted host result for the current test scenario.
 */
int ast_jb_read_conf(struct ast_jb_conf *conf, const char *varname, const char *value)
{
	(void)conf;
	(void)varname;
	(void)value;
	return jitter_config_result;
}

/** @brief Host-API test double for ast_true; observable effects are recorded in harness state.
 * @param value Input value or writable result, as declared.
 * @return Scripted host result for the current test scenario.
 */
int ast_true(const char *value)
{
	return value && (!strcasecmp(value, "yes") || !strcasecmp(value, "true") ||
			 !strcasecmp(value, "on") || !strcmp(value, "1"));
}

/** @brief Host-API test double for ast_false; observable effects are recorded in harness state.
 * @param value Input value or writable result, as declared.
 * @return Scripted host result for the current test scenario.
 */
int ast_false(const char *value)
{
	return value && (!strcasecmp(value, "no") || !strcasecmp(value, "false") ||
			 !strcasecmp(value, "off") || !strcmp(value, "0"));
}

/** @brief Host-API test double for __ast_strdup; observable effects are recorded in harness state.
 * @param value Input value or writable result, as declared.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @return Scripted host result for the current test scenario.
 */
char *__ast_strdup(const char *value, const char *file, int line, const char *function)
{
	char *copy;
	(void)file;
	(void)line;
	(void)function;
	ast_strdup_calls++;
	if (ast_strdup_calls == fail_ast_strdup_call)
		return NULL;
	copy = malloc(strlen(value) + 1);
	if (copy)
		strcpy(copy, value);
	return copy;
}

/** @brief Verify av frame alloc.
 * @return Result used by the test's assertions.
 */
AVFrame *test_av_frame_alloc(void)
{
	av_frame_alloc_calls++;
	if (fail_av_frame_alloc_call == av_frame_alloc_calls)
		return NULL;
	return __real_av_frame_alloc();
}

/** @brief Verify src process.
 * @param state Processor or stream state owned by the caller.
 * @param data Input payload or owned state being released, as declared.
 * @return Result used by the test's assertions.
 */
int test_src_process(SRC_STATE *state, SRC_DATA *data)
{
	int result;
	src_process_calls++;
	if (fail_src_process_call == src_process_calls)
		return 1;
	result = __real_src_process(state, data);
	if (!result && partial_src_process_call == src_process_calls && data->input_frames_used > 0)
		data->input_frames_used--;
	if (!result && partial_src_output_call == src_process_calls && data->output_frames_gen > 0)
		data->output_frames_gen--;
	return result;
}

/** @brief Verify src new.
 * @param converter_type libsamplerate converter type.
 * @param channels Number of interleaved audio channels.
 * @param error Receives a diagnostic for invalid input.
 * @return Result used by the test's assertions.
 */
SRC_STATE *test_src_new(int converter_type, int channels, int *error)
{
	src_new_calls++;
	if (fail_src_new_call == src_new_calls) {
		if (error)
			*error = 1;
		return NULL;
	}
	return __real_src_new(converter_type, channels, error);
}

/** @brief Test wrapper for av_frame_alloc controlled by the harness's failure-injection state.
 * @return Wrapped API result, including the failure selected by the harness.
 */
AVFrame *__wrap_av_frame_alloc(void)
{
	return test_av_frame_alloc();
}

/** @brief Test wrapper for src_process controlled by the harness's failure-injection state.
 * @param state Processor or stream state owned by the caller.
 * @param data Input payload or owned state being released, as declared.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_src_process(SRC_STATE *state, SRC_DATA *data)
{
	return test_src_process(state, data);
}

/** @brief Test wrapper for src_new controlled by the harness's failure-injection state.
 * @param converter_type libsamplerate converter type.
 * @param channels Number of interleaved audio channels.
 * @param error Receives a diagnostic for invalid input.
 * @return Wrapped API result, including the failure selected by the harness.
 */
SRC_STATE *__wrap_src_new(int converter_type, int channels, int *error)
{
	return test_src_new(converter_type, channels, error);
}

/** @brief Test wrapper for the playout-ring initializer's allocation failure.
 * @param ring Ring state to initialize.
 * @param capacity Sample capacity.
 * @param quality Converter quality selection.
 * @return Injected failure or the shared library's initializer result.
 */
int __wrap_rpcr_init(struct rpcr_ring *ring, size_t capacity, enum rpcr_quality quality)
{
	rpcr_init_calls++;
	if (fail_rpcr_init_call == rpcr_init_calls)
		return -1;
	return __real_rpcr_init(ring, capacity, quality);
}

/** @brief Test wrapper for the playout-ring rate configuration failure path.
 * @param ring Ring state to configure.
 * @param input_rate Producer sample rate in Hz.
 * @param output_rate Consumer sample rate in Hz.
 * @return Injected failure or the shared library's rate-setup result.
 */
int __wrap_rpcr_set_rates(struct rpcr_ring *ring, unsigned int input_rate, unsigned int output_rate)
{
	if (fail_rpcr_set_rates)
		return -1;
	return __real_rpcr_set_rates(ring, input_rate, output_rate);
}

/** @brief Test wrapper for one selected native FFmpeg graph preparation failure.
 * @param state Prepared graph state supplied by the native graph transaction.
 * @param config Immutable graph configuration supplied by the transaction.
 * @param sample_rate Native graph sample rate in hertz.
 * @return Injected failure for the selected preparation call or the real result.
 */
int __wrap_txagc_avfilter_prepare(struct txagc_avfilter *state, const struct txagc_config *config,
				  unsigned int sample_rate)
{
	avfilter_prepare_calls++;
	if (avfilter_prepare_calls == fail_avfilter_prepare_call)
		return -1;
	return __real_txagc_avfilter_prepare(state, config, sample_rate);
}

/** @brief Inject a selected prepared-graph processing failure in a native callback.
 * @param state Prepared graph selected by the test, or a normal graph.
 * @param samples PCM samples processed by the graph.
 * @param sample_count Number of samples in @p samples.
 * @return Injected failure for the selected graph or the real processing result.
 */
int __wrap_txagc_avfilter_process_prepared(struct txagc_avfilter *state, double *samples,
					   size_t sample_count)
{
	if (observed_avfilter_process_state && state == observed_avfilter_process_state) {
		observed_avfilter_process_calls++;
		return 0;
	}
	if (state == fail_avfilter_process_state)
		return -1;
	return __real_txagc_avfilter_process_prepared(state, samples, sample_count);
}

/** @brief Inject one absent processing option without changing the live settings snapshot. */
int __wrap_usbradioplus_processing_get_option(const char *channel, const char *section,
					      const char *name, char *value, size_t value_size)
{
	processing_option_get_calls++;
	if (processing_option_get_calls == fail_processing_option_get_call)
		return 1;
	return __real_usbradioplus_processing_get_option(channel, section, name, value, value_size);
}

/** @brief Inject one unavailable hardware profile without changing the live settings snapshot. */
int __wrap_usbradioplus_processing_get_hardware(const char *channel,
						struct usbradioplus_hardware_settings *hardware)
{
	processing_hardware_get_calls++;
	if (processing_hardware_get_calls == fail_processing_hardware_get_call)
		return 1;
	return __real_usbradioplus_processing_get_hardware(channel, hardware);
}

/** @brief Inject one unavailable composite chain while a graph transaction is still private. */
int __wrap_usbradioplus_processing_get_composite(const char *channel, struct txagc_chain *chain)
{
	processing_composite_get_calls++;
	if (processing_composite_get_calls == fail_processing_composite_get_call)
		return 1;
	return __real_usbradioplus_processing_get_composite(channel, chain);
}

/** @brief Test wrapper for pthread_join controlled by the harness's failure-injection state.
 * @param thread Worker thread identifier supplied by the harness.
 * @param result Receives the parsed value or supplies a CLI result, as declared.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_pthread_join(pthread_t thread, void **result)
{
	if ((uintptr_t)thread <= 100) {
		return test_pthread_join(thread, result);
	}
	return __real_pthread_join(thread, result);
}

/** @brief Test wrapper for usleep controlled by the harness's failure-injection state.
 * @param microseconds Requested sleep interval in microseconds.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_usleep(unsigned int microseconds)
{
	(void)microseconds;
	usleep_calls++;
	if (clear_eeprom_on_usleep && usbradio_default.next)
		usbradio_default.next->eepromctl = 0;
	if (clear_eeprom_target)
		clear_eeprom_target->eepromctl = 0;
	if (stop_pulser_on_usleep)
		stoppulser = 1;
	if (stop_hid_radio_on_usleep &&
	    ++stop_hid_usleep_count >= (stop_hid_after_usleeps ? stop_hid_after_usleeps : 1))
		stop_hid_radio_on_usleep->stophid = 1;
#ifdef URP_TEST_MODERN
	if (modern_stop_audio_on_usleep)
		modern_stop_audio_on_usleep->stopaudiothread = 1;
	if (modern_swap_first && modern_swap_second) {
		modern_swap_wait_step++;
		if (modern_swap_wait_step == 1) {
			modern_swap_first->swap_audio_ready = 1;
			if (!modern_swap_staggered)
				modern_swap_second->swap_audio_ready = 1;
			if (modern_swap_first != modern_swap_second) {
				modern_swap_first->swap_state = DEVICE_SWAP_READY;
				if (!modern_swap_staggered)
					modern_swap_second->swap_state = DEVICE_SWAP_READY;
			}
		} else {
			modern_swap_second->swap_audio_ready = 1;
			if (modern_swap_staggered) {
				modern_swap_second->swap_state = DEVICE_SWAP_READY;
			} else {
				modern_swap_first->swap_state = DEVICE_SWAP_IDLE;
				modern_swap_second->swap_state = DEVICE_SWAP_IDLE;
			}
		}
	}
#endif
	return 0;
}

/** @brief Host-API test double for ast_config_load2; observable effects are recorded in harness
 * state.
 * @param filename Configuration or diagnostic source filename.
 * @param who_asked Module requesting configuration.
 * @param flags Host API option bit mask.
 * @return Scripted host result for the current test scenario.
 */
struct ast_config *ast_config_load2(const char *filename, const char *who_asked,
				    struct ast_flags flags)
{
	(void)who_asked;
	(void)flags;
	if (separate_processing_config_result && !strcmp(filename, "usbradioplus.conf"))
		return test_processing_config_load_result;
	if (test_config_load_second_result && ++test_config_load_calls > 1)
		return test_config_load_second_result;
	return test_config_load_result;
}

/** @brief Host-API test double for ast_variable_browse; observable effects are recorded in harness
 * state.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 * @param category_name Configuration category name.
 * @return Scripted host result for the current test scenario.
 */
struct ast_variable *ast_variable_browse(const struct ast_config *config, const char *category_name)
{
	(void)category_name;
	variable_browse_calls++;
	if (inject_invalid_override_on_browse_call == variable_browse_calls) {
		struct section_override *entry =
			&settings.profiles[0].overrides[settings.profiles[0].override_count++];
		ast_copy_string(entry->section, "hardware", sizeof(entry->section));
		ast_copy_string(entry->name, "hardware_audio_fragment_count", sizeof(entry->name));
		ast_copy_string(entry->value, "invalid", sizeof(entry->value));
	}
	return config ? test_config_variables : NULL;
}

/** @brief Host-API test double for ast_config_destroy; observable effects are recorded in harness
 * state.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 */
void ast_config_destroy(struct ast_config *config)
{
	assert(config == test_config_load_result || config == test_processing_config_load_result);
	config_destroy_calls++;
}

/** @brief Host-API test double for ast_category_browse; observable effects are recorded in harness
 * state.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 * @param previous Previously returned category name, or NULL to start.
 * @return Scripted host result for the current test scenario.
 */
char *ast_category_browse(struct ast_config *config, const char *previous)
{
	(void)config;
	if (!previous)
		return test_config_category;
	return NULL;
}

/** @brief Host-API test double for ast_variable_retrieve; observable effects are recorded in
 * harness state.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 * @param category Asterisk category or category name, as declared.
 * @param variable Configuration variable to inspect or update.
 * @return Scripted host result for the current test scenario.
 */
const char *ast_variable_retrieve(struct ast_config *config, const char *category,
				  const char *variable)
{
	struct ast_variable *item;
	(void)config;
	(void)category;
	for (item = test_config_variables; item; item = item->next)
		if (!strcasecmp(item->name, variable))
			return item->value;
	return NULL;
}

/** @brief Host-API test double for ast_category_get; observable effects are recorded in harness
 * state.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 * @param category_name Configuration category name.
 * @param filter FFmpeg dynamics filter name.
 * @return Scripted host result for the current test scenario.
 */
struct ast_category *ast_category_get(const struct ast_config *config, const char *category_name,
				      const char *filter)
{
	(void)config;
	(void)category_name;
	(void)filter;
	return test_category_get_result;
}

/** @brief Host-API test double for ast_category_get_name; observable effects are recorded in
 * harness state.
 * @param category Asterisk category or category name, as declared.
 * @return Scripted host result for the current test scenario.
 */
const char *ast_category_get_name(const struct ast_category *category)
{
	(void)category;
	return "test";
}

/** @brief Host-API test double for ast_category_new; observable effects are recorded in harness
 * state.
 * @param name Option, metadata field, or channel name.
 * @param input_file Configuration source filename.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @return Scripted host result for the current test scenario.
 */
struct ast_category *ast_category_new(const char *name, const char *input_file, int line)
{
	(void)name;
	(void)input_file;
	(void)line;
	return (struct ast_category *)(uintptr_t)1;
}

/** @brief Host-API test double for ast_category_append; observable effects are recorded in harness
 * state.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 * @param category Asterisk category or category name, as declared.
 */
void ast_category_append(struct ast_config *config, struct ast_category *category)
{
	(void)config;
	(void)category;
}

/** @brief Host-API test double for ast_variable_update; observable effects are recorded in harness
 * state.
 * @param category Asterisk category or category name, as declared.
 * @param variable Configuration variable to inspect or update.
 * @param value Input value or writable result, as declared.
 * @param match Optional variable-value matching constraint.
 * @param object Host reference-counted object.
 * @return Scripted host result for the current test scenario.
 */
int ast_variable_update(struct ast_category *category, const char *variable, const char *value,
			const char *match, unsigned int object)
{
	(void)category;
	(void)match;
	(void)object;
	ast_copy_string(updated_variable_name, variable, sizeof(updated_variable_name));
	ast_copy_string(updated_variable_value, value, sizeof(updated_variable_value));
	return variable_update_result;
}

/** @brief Host-API test double for _ast_variable_new; observable effects are recorded in harness
 * state.
 * @param name Option, metadata field, or channel name.
 * @param value Input value or writable result, as declared.
 * @param filename Configuration or diagnostic source filename.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @return Result used by the test's assertions.
 */
struct ast_variable *_ast_variable_new(const char *name, const char *value, const char *filename,
				       const char *file, const char *function, int line)
{
	static struct ast_variable variable;
	(void)filename;
	(void)file;
	(void)function;
	(void)line;
	if (variable_new_failure)
		return NULL;
	variable.name = name;
	variable.value = value;
	variable.next = NULL;
	return &variable;
}

/** @brief Host-API test double for ast_variable_append; observable effects are recorded in harness
 * state.
 * @param category Asterisk category or category name, as declared.
 * @param variable Configuration variable to inspect or update.
 */
void ast_variable_append(struct ast_category *category, struct ast_variable *variable)
{
	(void)category;
	(void)variable;
	variable_append_calls++;
}

/** @brief Host-API test double for ast_config_text_file_save2; observable effects are recorded in
 * harness state.
 * @param filename Configuration or diagnostic source filename.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 * @param generator Name of the component saving configuration.
 * @param flags Host API option bit mask.
 * @return Scripted host result for the current test scenario.
 */
int ast_config_text_file_save2(const char *filename, const struct ast_config *config,
			       const char *generator, uint32_t flags)
{
	(void)filename;
	(void)config;
	(void)generator;
	(void)flags;
	return config_save_result;
}

/** @brief Host-API test double for ast_channel_tech_pvt; observable effects are recorded in harness
 * state.
 * @param channel Radio channel or channel index, as declared.
 * @return Scripted host result for the current test scenario.
 */
void *ast_channel_tech_pvt(const struct ast_channel *channel)
{
	(void)channel;
	return test_channel_private;
}

/** @brief Host-API test double for ast_setstate; observable effects are recorded in harness state.
 * @param channel Radio channel or channel index, as declared.
 * @param state Processor or stream state owned by the caller.
 * @return Scripted host result for the current test scenario.
 */
int ast_setstate(struct ast_channel *channel, enum ast_channel_state state)
{
	(void)channel;
	assert(state == AST_STATE_UP);
	setstate_calls++;
	return 0;
}

/** @brief Host-API test double for ast_moh_start; observable effects are recorded in harness state.
 * @param channel Radio channel or channel index, as declared.
 * @param music_class Requested music-on-hold class.
 * @param interpretation_class Fallback music-on-hold class.
 * @return Scripted host result for the current test scenario.
 */
int ast_moh_start(struct ast_channel *channel, const char *music_class,
		  const char *interpretation_class)
{
	(void)channel;
	(void)music_class;
	(void)interpretation_class;
	moh_start_calls++;
	return 0;
}

/** @brief Host-API test double for ast_moh_stop; observable effects are recorded in harness state.
 * @param channel Radio channel or channel index, as declared.
 */
void ast_moh_stop(struct ast_channel *channel)
{
	(void)channel;
	moh_stop_calls++;
}

/** @brief Host-API test double for ast_channel_internal_fd_set; observable effects are recorded in
 * harness state.
 * @param channel Radio channel or channel index, as declared.
 * @param which Channel descriptor slot.
 * @param value Input value or writable result, as declared.
 */
void ast_channel_internal_fd_set(struct ast_channel *channel, int which, int value)
{
	(void)channel;
	(void)which;
	(void)value;
}

/** @brief Host-API test double for __ast_verbose; observable effects are recorded in harness state.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param level Requested level or normalized tuning level, as declared.
 * @param format printf-style message format.
 * @param ... Values required by the wrapped variadic API.
 */
void __ast_verbose(const char *file, int line, const char *function, int level, const char *format,
		   ...)
{
	(void)file;
	(void)line;
	(void)function;
	(void)level;
	(void)format;
}

/** @brief Host-API test double for ast_cli; observable effects are recorded in harness state.
 * @param descriptor Test file or CLI descriptor.
 * @param format printf-style message format.
 * @param ... Values required by the wrapped variadic API.
 */
void ast_cli(int descriptor, const char *format, ...)
{
	(void)descriptor;
	(void)format;
}

/** @brief Host-API test double for ast_radio_ppwrite; observable effects are recorded in harness
 * state.
 * @param available Whether the fake hardware is present.
 * @param descriptor Test file or CLI descriptor.
 * @param base Parallel-port I/O base address.
 * @param port Parallel-port device name.
 * @param value Input value or writable result, as declared.
 */
void ast_radio_ppwrite(int available, unsigned int descriptor, unsigned int base, const char *port,
		       unsigned char value)
{
	(void)available;
	(void)descriptor;
	(void)base;
	(void)port;
	if (mock_parallel_ptt_mask && (value & mock_parallel_ptt_mask) == mock_parallel_ptt_mask) {
		parallel_saw_ptt_asserted = 1;
	}
	parallel_write_calls++;
}

/** @brief Host-API test double for ast_radio_load_parallel_port; observable effects are recorded in
 * harness state.
 * @param available Whether the fake hardware is present.
 * @param descriptor Test file or CLI descriptor.
 * @param base Parallel-port I/O base address.
 * @param port Parallel-port device name.
 * @param reload Nonzero selects reload handling for existing channels.
 * @return Scripted host result for the current test scenario.
 */
int ast_radio_load_parallel_port(int *available, int *descriptor, int *base, const char *port,
				 int reload)
{
	(void)port;
	(void)reload;
	parallel_load_calls++;
	*available = 1;
	*descriptor = 2;
	if (!*base)
		*base = PP_IOPORT;
	return 0;
}

/** @brief Host-API test double for ast_channel_name; observable effects are recorded in harness
 * state.
 * @param channel Radio channel or channel index, as declared.
 * @return Scripted host result for the current test scenario.
 */
const char *ast_channel_name(const struct ast_channel *channel)
{
	(void)channel;
	return "test";
}

/** @brief Host-API test double for ast_channel_appl; observable effects are recorded in harness
 * state.
 * @param channel Radio channel or channel index, as declared.
 * @return Scripted host result for the current test scenario.
 */
const char *ast_channel_appl(const struct ast_channel *channel)
{
	(void)channel;
	return NULL;
}

/** @brief Host-API test double for ast_channel_data; observable effects are recorded in harness
 * state.
 * @param channel Radio channel or channel index, as declared.
 * @return Scripted host result for the current test scenario.
 */
const char *ast_channel_data(const struct ast_channel *channel)
{
	(void)channel;
	return NULL;
}

/** @brief Host-API test double for ast_channel_get_by_name; observable effects are recorded in
 * harness state.
 * @param name Option, metadata field, or channel name.
 * @return Scripted host result for the current test scenario.
 */
struct ast_channel *ast_channel_get_by_name(const char *name)
{
	(void)name;
	return NULL;
}

/** @brief Host-API test double for ast_channel_iterator_all_new; observable effects are recorded in
 * harness state.
 * @return Scripted host result for the current test scenario.
 */
struct ast_channel_iterator *ast_channel_iterator_all_new(void)
{
	return NULL;
}

/** @brief Host-API test double for ast_channel_iterator_next; observable effects are recorded in
 * harness state.
 * @param iterator Harness channel iterator.
 * @return Scripted host result for the current test scenario.
 */
struct ast_channel *ast_channel_iterator_next(struct ast_channel_iterator *iterator)
{
	(void)iterator;
	return NULL;
}

/** @brief Host-API test double for ast_channel_iterator_destroy; observable effects are recorded in
 * harness state.
 * @param iterator Harness channel iterator.
 * @return Scripted host result for the current test scenario.
 */
struct ast_channel_iterator *ast_channel_iterator_destroy(struct ast_channel_iterator *iterator)
{
	return iterator;
}

/** @brief Host-API test double for __ao2_ref; observable effects are recorded in harness state.
 * @param object Host reference-counted object.
 * @param delta Requested reference-count change.
 * @param tag Host reference-tracking tag.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @return Scripted host result for the current test scenario.
 */
int __ao2_ref(void *object, int delta, const char *tag, const char *file, int line,
	      const char *function)
{
	(void)object;
	(void)delta;
	(void)tag;
	(void)file;
	(void)line;
	(void)function;
	return 0;
}

/** @brief Host-API test double for __ao2_lock; observable effects are recorded in harness state.
 * @param object Host reference-counted object.
 * @param request Host I/O or locking operation.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param name Option, metadata field, or channel name.
 * @return Scripted host result for the current test scenario.
 */
int __ao2_lock(void *object, enum ao2_lock_req request, const char *file, const char *function,
	       int line, const char *name)
{
	(void)object;
	(void)request;
	(void)file;
	(void)function;
	(void)line;
	(void)name;
	return 0;
}

/** @brief Host-API test double for __ao2_unlock; observable effects are recorded in harness state.
 * @param object Host reference-counted object.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param name Option, metadata field, or channel name.
 * @return Scripted host result for the current test scenario.
 */
int __ao2_unlock(void *object, const char *file, const char *function, int line, const char *name)
{
	(void)object;
	(void)file;
	(void)function;
	(void)line;
	(void)name;
	return 0;
}

/** @brief Host-API test double for __ast_datastore_alloc; observable effects are recorded in
 * harness state.
 * @param info Test module metadata.
 * @param uid Datastore identifier.
 * @param module Asterisk module reference.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @return Scripted host result for the current test scenario.
 */
struct ast_datastore *__ast_datastore_alloc(const struct ast_datastore_info *info, const char *uid,
					    struct ast_module *module, const char *file, int line,
					    const char *function)
{
	(void)info;
	(void)uid;
	(void)module;
	(void)file;
	(void)line;
	(void)function;
	return NULL;
}

/** @brief Host-API test double for ast_datastore_free; observable effects are recorded in harness
 * state.
 * @param datastore Channel-owned datastore.
 * @return Scripted host result for the current test scenario.
 */
int ast_datastore_free(struct ast_datastore *datastore)
{
	(void)datastore;
	return 0;
}

/** @brief Host-API test double for ast_channel_datastore_find; observable effects are recorded in
 * harness state.
 * @param channel Radio channel or channel index, as declared.
 * @param info Test module metadata.
 * @param uid Datastore identifier.
 * @return Scripted host result for the current test scenario.
 */
struct ast_datastore *ast_channel_datastore_find(struct ast_channel *channel,
						 const struct ast_datastore_info *info,
						 const char *uid)
{
	(void)channel;
	(void)info;
	(void)uid;
	return NULL;
}

/** @brief Host-API test double for ast_channel_datastore_add; observable effects are recorded in
 * harness state.
 * @param channel Radio channel or channel index, as declared.
 * @param datastore Channel-owned datastore.
 * @return Scripted host result for the current test scenario.
 */
int ast_channel_datastore_add(struct ast_channel *channel, struct ast_datastore *datastore)
{
	(void)channel;
	(void)datastore;
	return 0;
}

/** @brief Host-API test double for ast_channel_datastore_remove; observable effects are recorded in
 * harness state.
 * @param channel Radio channel or channel index, as declared.
 * @param datastore Channel-owned datastore.
 * @return Scripted host result for the current test scenario.
 */
int ast_channel_datastore_remove(struct ast_channel *channel, struct ast_datastore *datastore)
{
	(void)channel;
	(void)datastore;
	return 0;
}

/** @brief Host-API test double for ast_audiohook_init; observable effects are recorded in harness
 * state.
 * @param audiohook Attached link-processing hook.
 * @param type Requested Asterisk channel technology.
 * @param source Processing source or source text, as declared.
 * @param flags Host API option bit mask.
 * @return Scripted host result for the current test scenario.
 */
int ast_audiohook_init(struct ast_audiohook *audiohook, enum ast_audiohook_type type,
		       const char *source, enum ast_audiohook_init_flags flags)
{
	(void)audiohook;
	(void)type;
	(void)source;
	(void)flags;
	return 0;
}

/** @brief Host-API test double for ast_audiohook_attach; observable effects are recorded in harness
 * state.
 * @param channel Radio channel or channel index, as declared.
 * @param audiohook Attached link-processing hook.
 * @return Scripted host result for the current test scenario.
 */
int ast_audiohook_attach(struct ast_channel *channel, struct ast_audiohook *audiohook)
{
	(void)channel;
	(void)audiohook;
	return 0;
}

/** @brief Host-API test double for ast_audiohook_detach; observable effects are recorded in harness
 * state.
 * @param audiohook Attached link-processing hook.
 * @return Scripted host result for the current test scenario.
 */
int ast_audiohook_detach(struct ast_audiohook *audiohook)
{
	(void)audiohook;
	return 0;
}

/** @brief Host-API test double for ast_audiohook_destroy; observable effects are recorded in
 * harness state.
 * @param audiohook Attached link-processing hook.
 * @return Scripted host result for the current test scenario.
 */
int ast_audiohook_destroy(struct ast_audiohook *audiohook)
{
	(void)audiohook;
	return 0;
}

/** Native-rate format identity for the advanced interface fixture. */
static unsigned int native_format_rate = URP_RATE_NATIVE;

/** @brief Return the requested native format from the fixture cache.
 * @param rate Requested linear sample rate.
 * @return Native format identity, or the app_rpt format.
 */
struct ast_format *ast_format_cache_get_slin_by_rate(unsigned int rate)
{
	return rate == URP_RATE_NATIVE ? (struct ast_format *)&native_format_rate : ast_format_slin;
}

/** @brief Read the fixture format's sample rate.
 * @param format Native or app_rpt format identity.
 * @return PCM samples per second.
 */
unsigned int ast_format_get_sample_rate(const struct ast_format *format)
{
	return format == (struct ast_format *)&native_format_rate ? native_format_rate : 8000;
}

/** @brief Return the fixture's source format for link-hook attachment tests.
 * @param channel Asterisk channel supplied by the hook owner.
 * @return The linear fixture format.
 */
struct ast_format *ast_channel_rawreadformat(struct ast_channel *channel)
{
	(void)channel;
	return ast_format_slin;
}

/** @brief Host-API test double for ast_radio_time; observable effects are recorded in harness
 * state.
 * @param seconds Accumulates total processing time in seconds.
 */
void ast_radio_time(time_t *seconds)
{
	*seconds = 1234;
#ifdef URP_TEST_MODERN
	if (modern_stop_hid_on_radio_time && seconds != &modern_stop_hid_on_radio_time->lasthidtime)
		modern_stop_hid_on_radio_time->stophid = 1;
	modern_radio_time_calls++;
	if (modern_drop_hasusb_on_time &&
	    modern_radio_time_calls == modern_drop_hasusb_on_time_call)
		modern_drop_hasusb_on_time->hasusb = 0;
#endif
}

/** @brief Host-API test double for ast_radio_hid_device_mklist; observable effects are recorded in
 * harness state.
 * @return Scripted host result for the current test scenario.
 */
int ast_radio_hid_device_mklist(void)
{
	return hid_mklist_result;
}

/** @brief Host-API test double for ast_radio_usb_get_serial; observable effects are recorded in
 * harness state.
 * @param device Channel name or USB device identifier.
 * @param buffer Caller-owned buffer filled or consumed by the stub.
 * @param buffer_size Buffer size supplied by the test scenario.
 * @return Scripted host result for the current test scenario.
 */
int ast_radio_usb_get_serial(const char *device, char *buffer, size_t buffer_size)
{
	if (buffer_size) {
		if (mock_usb_serial_result > 0)
			ast_copy_string(buffer,
					mock_usb_serial_by_device && !strcmp(device, "usb-test")
						? "other-serial"
						: mock_usb_serial,
					buffer_size);
		else
			buffer[0] = '\0';
	}
	return mock_usb_serial_result;
}

#ifndef URP_TEST_MODERN
/** @brief Host-API test double for ast_radio_amixer_max; observable effects are recorded in harness
 * state.
 * @param device_number Simulated ALSA card number.
 * @param parameter ALSA mixer control name.
 * @return Scripted host result for the current test scenario.
 */
int ast_radio_amixer_max(int device_number, char *parameter)
{
	(void)device_number;
	if (mock_new_mixer_name && !strcmp(parameter, MIXER_PARAM_SPKR_PLAYBACK_VOL))
		return -1;
	return mock_amixer_max;
}

/** @brief Host-API test double for ast_radio_hid_device_init; observable effects are recorded in
 * harness state.
 * @param device Channel name or USB device identifier.
 * @return Scripted host result for the current test scenario.
 */
struct usb_device *ast_radio_hid_device_init(const char *device)
{
	(void)device;
	return mock_hid_device_available ? &mock_usb_device : NULL;
}

/** @brief Host-API test double for ast_radio_hid_set_outputs; observable effects are recorded in
 * harness state.
 * @param handle Simulated acquired USB handle.
 * @param outputs USB HID output report.
 */
void ast_radio_hid_set_outputs(struct usb_dev_handle *handle, unsigned char *outputs)
{
	(void)handle;
	(void)outputs;
}

/** @brief Host-API test double for ast_radio_hid_get_inputs; observable effects are recorded in
 * harness state.
 * @param handle Simulated acquired USB handle.
 * @param inputs USB HID input report.
 */
void ast_radio_hid_get_inputs(struct usb_dev_handle *handle, unsigned char *inputs)
{
	(void)handle;
	memcpy(inputs, mock_hid_inputs, sizeof(mock_hid_inputs));
	mock_hid_input_calls++;
	mock_hid_inputs[toggle_hid_input_index] ^= toggle_hid_inputs_mask;
	if (unkey_without_program_on_input &&
	    mock_hid_input_calls >= unkey_without_program_after_inputs) {
		/* An odd generation deliberately makes the lock-free reader reject this
		 * snapshot.  Physical unkey must still clear the parallel transmitter bit. */
		atomic_store_explicit(
			&unkey_without_program_on_input->plus_radio_program_generation, 1U,
			memory_order_release);
		unkey_without_program_on_input->lasttx = 1;
		usbradioplus_publish_hardware_ptt(unkey_without_program_on_input, 0);
		unkey_without_program_on_input = NULL;
	}
	if (key_without_program_on_input &&
	    mock_hid_input_calls >= key_without_program_after_inputs) {
		/* An odd generation deliberately makes the lock-free reader reject this
		 * snapshot.  Physical key-up must still apply the requested PTT state. */
		atomic_store_explicit(&key_without_program_on_input->plus_radio_program_generation,
				      1U, memory_order_release);
		key_without_program_on_input->lasttx = 0;
		usbradioplus_publish_hardware_ptt(key_without_program_on_input, 1);
		key_without_program_on_input = NULL;
	}
	if (program_and_key_on_input && mock_hid_input_calls >= program_and_key_after_inputs) {
		/* Publish after hidthread setup so the same service pass observes both
		 * the new coherent program request and its PTT transition. */
		usbradioplus_program_radio(program_and_key_on_input);
		usbradioplus_publish_hardware_ptt(program_and_key_on_input, 1);
		program_and_key_on_input = NULL;
	}
	if (key_after_program_on_input && mock_hid_input_calls >= key_after_program_after_inputs) {
		/* The first pass already consumed the program snapshot.  This second
		 * request must update only physical PTT. */
		usbradioplus_publish_hardware_ptt(key_after_program_on_input, 1);
		key_after_program_on_input = NULL;
	}
	if (drop_hid_on_input && mock_hid_input_calls >= stop_hid_after_inputs) {
		drop_hid_on_input->hasusb = 0;
		mock_hid_device_available = 0;
	}
	if (stop_hid_on_input && mock_hid_input_calls >= stop_hid_after_inputs)
		stop_hid_on_input->stophid = 1;
}

/** @brief Host-API test double for ast_radio_get_eeprom; observable effects are recorded in harness
 * state.
 * @param handle Simulated acquired USB handle.
 * @param buffer Caller-owned buffer filled or consumed by the stub.
 * @return Scripted host result for the current test scenario.
 */
unsigned short ast_radio_get_eeprom(struct usb_dev_handle *handle, unsigned short *buffer)
{
	(void)handle;
	buffer[EEPROM_USER_MAGIC_ADDR] = mock_eeprom_valid_magic ? EEPROM_MAGIC : 0;
	if (eeprom_write_after_read)
		eeprom_write_after_read->eepromctl = 2;
	return mock_eeprom_result;
}

/** @brief Host-API test double for ast_radio_put_eeprom; observable effects are recorded in harness
 * state.
 * @param handle Simulated acquired USB handle.
 * @param buffer Caller-owned buffer filled or consumed by the stub.
 */
void ast_radio_put_eeprom(struct usb_dev_handle *handle, unsigned short *buffer)
{
	(void)handle;
	(void)buffer;
}
#endif

/** @brief Host-API test double for ast_radio_ppread; observable effects are recorded in harness
 * state.
 * @param enabled Nonzero enables the operation.
 * @param descriptor Test file or CLI descriptor.
 * @param base Parallel-port I/O base address.
 * @param port Parallel-port device name.
 * @return Scripted host result for the current test scenario.
 */
unsigned char ast_radio_ppread(int enabled, unsigned int descriptor, unsigned int base,
			       const char *port)
{
	(void)enabled;
	(void)descriptor;
	(void)base;
	(void)port;
	{
		unsigned char result = mock_parallel_inputs;
		mock_parallel_inputs ^= toggle_parallel_inputs_mask;
		return result;
	}
}

/** @brief Host-API test double for ast_radio_tvnow; observable effects are recorded in harness
 * state.
 * @return Scripted host result for the current test scenario.
 */
struct timeval ast_radio_tvnow(void)
{
	struct timeval now = {.tv_sec = mock_tvnow_milliseconds / 1000,
			      .tv_usec = (mock_tvnow_milliseconds % 1000) * 1000};
	mock_tvnow_milliseconds += mock_tvnow_step;
	return now;
}

/** @brief Host-API test double for ast_queue_frame; observable effects are recorded in harness
 * state.
 * @param channel Radio channel or channel index, as declared.
 * @param frame Asterisk or FFmpeg audio frame, as declared.
 * @return Scripted host result for the current test scenario.
 */
int ast_queue_frame(struct ast_channel *channel, struct ast_frame *frame)
{
	(void)channel;
	(void)frame;
#ifdef URP_TEST_MODERN
	if (modern_drop_hasusb_on_queue) {
		modern_drop_hasusb_on_queue->hasusb = 0;
		modern_drop_hasusb_on_queue = NULL;
	}
#endif
	return 0;
}

/** @brief Host-API test double for ast_channel_tech_pvt_set; observable effects are recorded in
 * harness state.
 * @param channel Radio channel or channel index, as declared.
 * @param value Input value or writable result, as declared.
 */
void ast_channel_tech_pvt_set(struct ast_channel *channel, void *value)
{
	(void)channel;
	test_channel_private = value;
}

/** @brief Host-API test double for __ast_module_unref; observable effects are recorded in harness
 * state.
 * @param module Asterisk module reference.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 */
void __ast_module_unref(struct ast_module *module, const char *file, int line, const char *function)
{
	(void)module;
	(void)file;
	(void)line;
	(void)function;
}

/** @brief Host-API test double for __ast_module_ref; observable effects are recorded in harness
 * state.
 * @param module Asterisk module reference.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @return Scripted host result for the current test scenario.
 */
struct ast_module *__ast_module_ref(struct ast_module *module, const char *file, int line,
				    const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	return module;
}

/** @brief Host-API test double for __ast_channel_alloc; observable effects are recorded in harness
 * state.
 * @param need_queue Whether the allocated channel requires a frame queue.
 * @param state Processor or stream state owned by the caller.
 * @param caller_number Caller ID number.
 * @param caller_name Caller ID name.
 * @param account_code Asterisk accounting code.
 * @param extension Asterisk dialplan extension.
 * @param context Asterisk dialplan context or FFmpeg filter context.
 * @param assigned_ids Asterisk-assigned channel identifiers.
 * @param requestor Channel requesting the radio connection.
 * @param amaflag Asterisk call-accounting mode.
 * @param endpoint Asterisk endpoint associated with the channel.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param name_format printf-style channel-name format.
 * @param ... Values required by the wrapped variadic API.
 * @return Scripted host result for the current test scenario.
 */
struct ast_channel *__ast_channel_alloc(int need_queue, int state, const char *caller_number,
					const char *caller_name, const char *account_code,
					const char *extension, const char *context,
					const struct ast_assigned_ids *assigned_ids,
					const struct ast_channel *requestor, enum ama_flags amaflag,
					struct ast_endpoint *endpoint, const char *file, int line,
					const char *function, const char *name_format, ...)
{
	(void)need_queue;
	(void)state;
	(void)caller_number;
	(void)caller_name;
	(void)account_code;
	(void)extension;
	(void)context;
	(void)assigned_ids;
	(void)requestor;
	(void)amaflag;
	(void)endpoint;
	(void)file;
	(void)line;
	(void)function;
	(void)name_format;
	return fail_channel_alloc ? NULL : (struct ast_channel *)(uintptr_t)2;
}

/** @brief Host-API test double for ast_channel_tech_set; observable effects are recorded in harness
 * state.
 * @param channel Radio channel or channel index, as declared.
 * @param technology Asterisk channel technology callbacks.
 */
void ast_channel_tech_set(struct ast_channel *channel, const struct ast_channel_tech *technology)
{
	(void)channel;
	(void)technology;
}

/** @brief Host-API test double for ast_channel_nativeformats_set; observable effects are recorded
 * in harness state.
 * @param channel Radio channel or channel index, as declared.
 * @param formats Requested Asterisk audio formats.
 */
void ast_channel_nativeformats_set(struct ast_channel *channel, struct ast_format_cap *formats)
{
	(void)channel;
	(void)formats;
}

/** @brief Host-API test double for ast_channel_set_readformat; observable effects are recorded in
 * harness state.
 * @param channel Radio channel or channel index, as declared.
 * @param format printf-style message format.
 */
void ast_channel_set_readformat(struct ast_channel *channel, struct ast_format *format)
{
	(void)channel;
	(void)format;
}

/** @brief Host-API test double for ast_channel_set_writeformat; observable effects are recorded in
 * harness state.
 * @param channel Radio channel or channel index, as declared.
 * @param format printf-style message format.
 */
void ast_channel_set_writeformat(struct ast_channel *channel, struct ast_format *format)
{
	(void)channel;
	(void)format;
}

/** @brief Accept the advanced interface's native receive format.
 * @param channel Fixture channel.
 * @param format Requested PCM format.
 * @return Zero on success.
 */
int ast_set_read_format(struct ast_channel *channel, struct ast_format *format)
{
	ast_channel_set_readformat(channel, format);
	return set_read_result;
}

/** @brief Accept the advanced interface's native transmit format.
 * @param channel Fixture channel.
 * @param format Requested PCM format.
 * @return Zero on success.
 */
int ast_set_write_format(struct ast_channel *channel, struct ast_format *format)
{
	ast_channel_set_writeformat(channel, format);
	return set_write_result;
}

/** @brief Host-API test double for ast_jb_configure; observable effects are recorded in harness
 * state.
 * @param channel Radio channel or channel index, as declared.
 * @param configuration Configuration supplied by the test scenario.
 */
void ast_jb_configure(struct ast_channel *channel, const struct ast_jb_conf *configuration)
{
	(void)channel;
	(void)configuration;
}

/** @brief Host-API test double for ast_pbx_start; observable effects are recorded in harness state.
 * @param channel Radio channel or channel index, as declared.
 * @return Scripted host result for the current test scenario.
 */
enum ast_pbx_result ast_pbx_start(struct ast_channel *channel)
{
	(void)channel;
	return (enum ast_pbx_result)pbx_start_result;
}

/** @brief Host-API test double for ast_hangup; observable effects are recorded in harness state.
 * @param channel Radio channel or channel index, as declared.
 */
void ast_hangup(struct ast_channel *channel)
{
	(void)channel;
	hangup_calls++;
}

/** @brief Host-API test double for ast_format_cap_iscompatible; observable effects are recorded in
 * harness state.
 * @param first First value or format capability supplied to the stub.
 * @param second Second value or format capability supplied to the stub.
 * @return Scripted host result for the current test scenario.
 */
int ast_format_cap_iscompatible(const struct ast_format_cap *first,
				const struct ast_format_cap *second)
{
	(void)first;
	(void)second;
	return format_compatible;
}

/** @brief Host-API test double for ast_format_cap_get_names; observable effects are recorded in
 * harness state.
 * @param formats Requested Asterisk audio formats.
 * @param buffer Caller-owned buffer filled or consumed by the stub.
 * @return Scripted host result for the current test scenario.
 */
const char *ast_format_cap_get_names(const struct ast_format_cap *formats, struct ast_str **buffer)
{
	(void)formats;
	(void)buffer;
	return "test-format";
}

/** @brief Host-API test double for ast_channel_state; observable effects are recorded in harness
 * state.
 * @param channel Radio channel or channel index, as declared.
 * @return Scripted host result for the current test scenario.
 */
enum ast_channel_state ast_channel_state(const struct ast_channel *channel)
{
	(void)channel;
	return channel_state;
}

/** @brief Host-API test double for ast_dsp_process; observable effects are recorded in harness
 * state.
 * @param channel Radio channel or channel index, as declared.
 * @param dsp Dsp supplied by the test scenario.
 * @param frame Asterisk or FFmpeg audio frame, as declared.
 * @return Scripted host result for the current test scenario.
 */
struct ast_frame *ast_dsp_process(struct ast_channel *channel, struct ast_dsp *dsp,
				  struct ast_frame *frame)
{
	static struct ast_frame result;
	(void)channel;
	(void)dsp;
	if (dsp_result_type >= 0) {
		result = *frame;
		result.frametype = dsp_result_type;
		result.subclass.integer = dsp_result_digit;
		return &result;
	}
	return frame;
}

/** @brief Host-API test double for ast_frame_free; observable effects are recorded in harness
 * state.
 * @param frame Asterisk or FFmpeg audio frame, as declared.
 * @param cache Whether Asterisk may cache the released frame.
 */
void ast_frame_free(struct ast_frame *frame, int cache)
{
	(void)frame;
	(void)cache;
	frame_free_calls++;
}

/** @brief Host-API test double for ast_pthread_create_stack; observable effects are recorded in
 * harness state.
 * @param thread Worker thread identifier supplied by the harness.
 * @param attributes POSIX thread creation attributes.
 * @param start_routine Worker entry point supplied by the module.
 * @param data Input payload or owned state being released, as declared.
 * @param stack_size Requested worker stack size in bytes.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param caller Calling function name.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param start_function Worker entry-point name for diagnostics.
 * @return Scripted host result for the current test scenario.
 */
int ast_pthread_create_stack(pthread_t *thread, pthread_attr_t *attributes,
			     void *(*start_routine)(void *), void *data, size_t stack_size,
			     const char *file, const char *caller, int line,
			     const char *start_function)
{
	(void)attributes;
	(void)start_routine;
	(void)data;
	(void)stack_size;
	(void)file;
	(void)caller;
	(void)line;
	(void)start_function;
	pthread_create_calls++;
	if (pthread_create_calls == fail_pthread_create_call)
		return -1;
	*thread = (pthread_t)(uintptr_t)pthread_create_calls;
	return 0;
}

/** @brief Host-API test double for ast_background_stacksize; observable effects are recorded in
 * harness state.
 * @return Scripted host result for the current test scenario.
 */
int ast_background_stacksize(void)
{
	return 0;
}

/** @brief Host-API test double for __ast_pthread_mutex_init; observable effects are recorded in
 * harness state.
 * @param tracking Host mutex debug tracking flag.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param mutex_name Diagnostic mutex name.
 * @param mutex Mutex tracked by the harness.
 * @return Scripted host result for the current test scenario.
 */
int __ast_pthread_mutex_init(int tracking, const char *file, int line, const char *function,
			     const char *mutex_name, ast_mutex_t *mutex)
{
	(void)tracking;
	(void)file;
	(void)line;
	(void)function;
	(void)mutex_name;
	memset(mutex, 0, sizeof(*mutex));
	return 0;
}

/** @brief Host-API test double for ast_dsp_new; observable effects are recorded in harness state.
 * @return Scripted host result for the current test scenario.
 */
struct ast_dsp *ast_dsp_new(void)
{
	return mock_dsp_available ? (struct ast_dsp *)(uintptr_t)1 : NULL;
}

/** @brief Host-API test double for ast_dsp_set_features; observable effects are recorded in harness
 * state.
 * @param dsp Dsp supplied by the test scenario.
 * @param features Requested Asterisk DSP feature mask.
 */
void ast_dsp_set_features(struct ast_dsp *dsp, int features)
{
	(void)dsp;
	(void)features;
}

/** @brief Host-API test double for ast_dsp_set_digitmode; observable effects are recorded in
 * harness state.
 * @param dsp Dsp supplied by the test scenario.
 * @param mode Configured routing, detection, or hardware-open mode.
 * @return Scripted host result for the current test scenario.
 */
int ast_dsp_set_digitmode(struct ast_dsp *dsp, int mode)
{
	(void)dsp;
	(void)mode;
	return 0;
}

/** @brief Host-API test double for ast_radio_wait_or_poll; observable effects are recorded in
 * harness state.
 * @param descriptor Test file or CLI descriptor.
 * @param milliseconds Requested wait duration in milliseconds.
 * @param interactive Nonzero permits interactive cancellation.
 * @return Scripted host result for the current test scenario.
 */
int ast_radio_wait_or_poll(int descriptor, int milliseconds, int interactive)
{
	(void)descriptor;
	(void)milliseconds;
	(void)interactive;
	wait_or_poll_calls++;
	if (scripted_measure_stage && !(wait_or_poll_calls % 2) &&
	    scripted_measurement_index < scripted_measurement_count)
		scripted_measure_stage->apeak = scripted_measurements[scripted_measurement_index++];
	return wait_or_poll_fail_call == wait_or_poll_calls;
}

/** @brief Host-API test double for ast_radio_poll_input; observable effects are recorded in harness
 * state.
 * @param descriptor Test file or CLI descriptor.
 * @param milliseconds Requested wait duration in milliseconds.
 * @return Scripted host result for the current test scenario.
 */
int ast_radio_poll_input(int descriptor, int milliseconds)
{
	(void)descriptor;
	(void)milliseconds;
	if (poll_successes_before_exit > 0) {
		poll_successes_before_exit--;
		if (toggle_rxkey_radio)
			toggle_rxkey_radio->rxkeyed = !toggle_rxkey_radio->rxkeyed;
		return 0;
	}
	return 1;
}

/** @brief Host-API test double for ast_radio_print_audio_stats; observable effects are recorded in
 * harness state.
 * @param descriptor Test file or CLI descriptor.
 * @param statistics Audio measurement structure updated or displayed by the stub.
 * @param prefix Unique label prefix for the appended graph fragment.
 */
void ast_radio_print_audio_stats(int descriptor, struct audiostatistics *statistics,
				 const char *prefix)
{
	(void)descriptor;
	(void)statistics;
	(void)prefix;
	radio_print_audio_stats_calls++;
}

/** @brief Host-API test double for ast_radio_usb_get_usbdev; observable effects are recorded in
 * harness state.
 * @param device Channel name or USB device identifier.
 * @return Scripted host result for the current test scenario.
 */
int ast_radio_usb_get_usbdev(const char *device)
{
	(void)device;
	return mock_usb_device_number;
}

/** @brief Host-API test double for ast_radio_usb_list_check; observable effects are recorded in
 * harness state.
 * @param device Channel name or USB device identifier.
 * @return Scripted host result for the current test scenario.
 */
int ast_radio_usb_list_check(char *device)
{
	return installed_usb_device && !strcmp(device, installed_usb_device);
}

/** @brief Host-API test double for ast_radio_usb_get_devstr; observable effects are recorded in
 * harness state.
 * @param index Sample position within the trace block.
 * @return Scripted host result for the current test scenario.
 */
char *ast_radio_usb_get_devstr(int index)
{
	if (mock_no_usb_devices)
		return NULL;
	if (!index)
		return "usb-test";
	return index == 1 && mock_second_usb_device ? "usb-second" : NULL;
}

/** @brief Host-API test double for ast_debug_get_by_module; observable effects are recorded in
 * harness state.
 * @param module Asterisk module reference.
 * @return Scripted host result for the current test scenario.
 */
unsigned int ast_debug_get_by_module(const char *module)
{
	return !strcmp(module, AST_MODULE) ? module_debug_level : file_debug_level;
}

/** @brief Host-API test double for ast_log; observable effects are recorded in harness state.
 * @param level Requested level or normalized tuning level, as declared.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param format printf-style message format.
 * @param ... Values required by the wrapped variadic API.
 */
void ast_log(int level, const char *file, int line, const char *function, const char *format, ...)
{
	(void)level;
	(void)file;
	(void)line;
	(void)function;
	if (!strncmp(format, "URP_TXTRACE", 11)) {
		va_list args;
		va_start(args, format);
		vsnprintf(tx_trace_message, sizeof(tx_trace_message), format, args);
		va_end(args);
	}
}

/** @brief Host-API test double for ast_log_ap; observable effects are recorded in harness state.
 * @param level Requested level or normalized tuning level, as declared.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param format printf-style message format.
 * @param arguments Formatted-message values or filter options.
 */
void ast_log_ap(int level, const char *file, int line, const char *function, const char *format,
		va_list arguments)
{
	(void)level;
	(void)file;
	(void)line;
	(void)function;
	(void)format;
	(void)arguments;
}

/** @brief Host-API test double for __ast_pthread_mutex_lock; observable effects are recorded in
 * harness state.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param mutex_name Diagnostic mutex name.
 * @param mutex Mutex tracked by the harness.
 * @return Scripted host result for the current test scenario.
 */
int __ast_pthread_mutex_lock(const char *file, int line, const char *function,
			     const char *mutex_name, ast_mutex_t *mutex)
{
	(void)file;
	(void)line;
	(void)function;
	(void)mutex_name;
	return pthread_mutex_lock(&mutex->mutex);
}

/** @brief Host-API test double for __ast_pthread_mutex_unlock; observable effects are recorded in
 * harness state.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param mutex_name Diagnostic mutex name.
 * @param mutex Mutex tracked by the harness.
 * @return Scripted host result for the current test scenario.
 */
int __ast_pthread_mutex_unlock(const char *file, int line, const char *function,
			       const char *mutex_name, ast_mutex_t *mutex)
{
	(void)file;
	(void)line;
	(void)function;
	(void)mutex_name;
	return pthread_mutex_unlock(&mutex->mutex);
}

/** @brief Host-API test double for __ast_free; observable effects are recorded in harness state.
 * @param pointer Allocated buffer passed through the failure-injection shim.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 */
void __ast_free(void *pointer, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	free(pointer);
}

/** @brief Host-API test double for __ast_realloc; observable effects are recorded in harness state.
 * @param pointer Allocated buffer passed through the failure-injection shim.
 * @param size Destination capacity in bytes, including the terminator for text.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @return Scripted host result for the current test scenario.
 */
void *__ast_realloc(void *pointer, size_t size, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	realloc_calls++;
	return fail_realloc || fail_realloc_call == realloc_calls ? NULL : realloc(pointer, size);
}

/** @brief Host-API test double for __ast_calloc; observable effects are recorded in harness state.
 * @param count Number of elements available in the supplied block.
 * @param size Destination capacity in bytes, including the terminator for text.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @return Scripted host result for the current test scenario.
 */
void *__ast_calloc(size_t count, size_t size, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	ast_calloc_calls++;
	if (fail_radio_state_allocation && count * size == sizeof(urp_radio_state))
		return NULL;
	if (ast_calloc_calls == fail_ast_calloc_call)
		return NULL;
	return calloc(count, size);
}

#ifdef URP_TEST_MODERN
static struct ast_radio_device *modern_acquire_device;
static enum ast_radio_device_result modern_acquire_result = AST_RADIO_DEVICE_READY;
static long modern_mixer_max = 100;
static int modern_mixer_max_calls;
static int modern_mixer_max_failure_call;
static unsigned int modern_automatic_device_count;
static PaError modern_open_result;
static PaError modern_start_result;
/** Simulate a host wrapper that accepts an open while exposing no stream handle. */
static int modern_open_without_stream;
static PaError modern_read_result;
static PaError modern_write_result;
static int modern_stream_info_available;
static PaStreamInfo modern_stream_info;
static short modern_read_sample;
static const short *modern_last_write;
static long modern_write_available = AST_RADIO_PA_FRAMES_PER_BUFFER;
static struct chan_usbradio_pvt *modern_stop_audio_target;
static int modern_libusb_open_result;
static int modern_libusb_claim_result;
static int modern_libusb_claim_calls;
static int modern_libusb_claim_first_result;
static int modern_libusb_detach_result;
static struct chan_usbradio_pvt *modern_stop_hid_target;
static int modern_hid_input_calls;
static int modern_stop_hid_after_inputs = 1;
static int modern_toggle_hid_inputs;
static int modern_drop_hid_after_inputs;
static void (*modern_hid_input_hook)(struct chan_usbradio_pvt *, unsigned char *, int);
static int modern_device_swap_result;
static int modern_mixer_element_available;
static unsigned char modern_hid_inputs[4];
static unsigned short modern_eeprom_result = 1;
static int modern_eeprom_valid_magic;
static struct chan_usbradio_pvt *modern_eeprom_write_after_read;

int __wrap_libusb_open(libusb_device *device, libusb_device_handle **handle)
{
	(void)device;
	*handle = modern_libusb_open_result < 0 ? NULL : (libusb_device_handle *)(uintptr_t)1;
	return modern_libusb_open_result;
}

void __wrap_libusb_close(libusb_device_handle *handle)
{
	(void)handle;
}

int __wrap_libusb_claim_interface(libusb_device_handle *handle, int interface_number)
{
	(void)handle;
	(void)interface_number;
	modern_libusb_claim_calls++;
	if (modern_libusb_claim_calls == 1 && modern_libusb_claim_first_result)
		return modern_libusb_claim_first_result;
	return modern_libusb_claim_result;
}

int __wrap_libusb_detach_kernel_driver(libusb_device_handle *handle, int interface_number)
{
	(void)handle;
	(void)interface_number;
	return modern_libusb_detach_result;
}

enum ast_radio_device_result
ast_radio_device_acquire(const struct ast_radio_device_request *request,
			 struct ast_radio_device **device)
{
	(void)request;
	*device = modern_acquire_result == AST_RADIO_DEVICE_READY ? modern_acquire_device : NULL;
	return modern_acquire_result;
}

const char *ast_radio_device_result_str(enum ast_radio_device_result result)
{
	(void)result;
	return "mock device result";
}

void ast_radio_device_release(struct ast_radio_device *device)
{
	(void)device;
}

unsigned int ast_radio_device_automatic_count(void)
{
	return modern_automatic_device_count;
}

int ast_radio_device_swap(struct ast_radio_device **first, struct ast_radio_device **second)
{
	if (modern_device_swap_result)
		return modern_device_swap_result;
	struct ast_radio_device *temporary = *first;
	*first = *second;
	*second = temporary;
	return 0;
}

const struct ast_radio_mixer_element *
ast_radio_device_mixer_element(const struct ast_radio_device *device,
			       const struct ast_radio_mixer_path *path)
{
	(void)device;
	(void)path;
	return modern_mixer_element_available ? (const struct ast_radio_mixer_element *)(uintptr_t)1
					      : NULL;
}

long ast_radio_device_mixer_max(const struct ast_radio_device *device,
				const struct ast_radio_mixer_path *path, unsigned int capability)
{
	(void)device;
	(void)path;
	(void)capability;
	modern_mixer_max_calls++;
	return modern_mixer_max_calls == modern_mixer_max_failure_call ? 0 : modern_mixer_max;
}

PaError ast_radio_pa_open_device(struct ast_radio_pa_stream *stream,
				 const struct ast_radio_device *device)
{
	(void)device;
	stream->stream = modern_open_result == paNoError && !modern_open_without_stream
				 ? (PaStream *)(uintptr_t)1
				 : NULL;
	stream->output_channels = 2;
	return modern_open_result;
}

const PaStreamInfo *__wrap_Pa_GetStreamInfo(PaStream *stream)
{
	assert(stream == (PaStream *)(uintptr_t)1);
	return modern_stream_info_available ? &modern_stream_info : NULL;
}

PaError ast_radio_pa_start(struct ast_radio_pa_stream *stream)
{
	if (modern_start_result == paNoError)
		stream->active = 1;
	return modern_start_result;
}

void ast_radio_pa_stop(struct ast_radio_pa_stream *stream)
{
	stream->active = 0;
	stream->stream = NULL;
}

PaError ast_radio_pa_read(struct ast_radio_pa_stream *stream, short *buffer, unsigned long frames,
			  int timeout_ms, volatile sig_atomic_t *stop)
{
	(void)timeout_ms;
	(void)stop;
	if (modern_read_result == paNoError) {
		unsigned long samples = frames * stream->input_channels;
		for (unsigned long i = 0; i < samples; ++i)
			buffer[i] = modern_read_sample;
	}
	if (modern_stop_audio_target)
		modern_stop_audio_target->stopaudiothread = 1;
	return modern_read_result;
}

PaError ast_radio_pa_write(struct ast_radio_pa_stream *stream, const short *data,
			   unsigned long frames)
{
	(void)stream;
	record_mock_sound_write(data, frames * AST_RADIO_PA_OUTPUT_CHANNELS * sizeof(*data));
	modern_last_write = data;
	return modern_write_result;
}

long ast_radio_pa_write_available(struct ast_radio_pa_stream *stream)
{
	(void)stream;
	return modern_write_available;
}

int ast_radio_hid_set_outputs(struct libusb_device_handle *handle, unsigned char *outputs)
{
	(void)handle;
	(void)outputs;
	return 0;
}

int ast_radio_hid_get_inputs(struct libusb_device_handle *handle, unsigned char *inputs)
{
	(void)handle;
	memcpy(inputs, modern_hid_inputs, sizeof(modern_hid_inputs));
	modern_hid_input_calls++;
	if (modern_hid_input_hook)
		modern_hid_input_hook(modern_stop_hid_target, inputs, modern_hid_input_calls);
	if (modern_toggle_hid_inputs)
		for (size_t index = 0; index < ARRAY_LEN(modern_hid_inputs); ++index)
			modern_hid_inputs[index] ^= 0xff;
	if (modern_stop_hid_target && modern_hid_input_calls >= modern_stop_hid_after_inputs) {
		if (modern_drop_hid_after_inputs) {
			modern_stop_hid_target->hasusb = 0;
			modern_acquire_result = AST_RADIO_DEVICE_WAIT;
		} else
			modern_stop_hid_target->stophid = 1;
	}
	return 0;
}

unsigned short ast_radio_get_eeprom(struct libusb_device_handle *handle, unsigned short *buffer)
{
	(void)handle;
	buffer[EEPROM_USER_MAGIC_ADDR] = modern_eeprom_valid_magic ? EEPROM_MAGIC : 0;
	if (modern_eeprom_write_after_read)
		modern_eeprom_write_after_read->eepromctl = 2;
	return modern_eeprom_result;
}

void ast_radio_put_eeprom(struct libusb_device_handle *handle, unsigned short *buffer)
{
	(void)handle;
	(void)buffer;
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
/** @brief Host-API test double for ast_radio_setamixer; observable effects are recorded in harness
 * state.
 * @param device Channel name or USB device identifier.
 * @param parameter ALSA mixer control name.
 * @param first First value or format capability supplied to the stub.
 * @param second Second value or format capability supplied to the stub.
 * @return Scripted host result for the current test scenario.
 */
int ast_radio_setamixer(int device, char *parameter, int first, int second)
{
	(void)device;
	(void)parameter;
	(void)first;
	(void)second;
	return 0;
}

/** @brief Host-API test double for ast_radio_make_spkr_playback_value; observable effects are
 * recorded in harness state.
 * @param maximum Maximum supported mixer step.
 * @param requested Requested normalized mixer setting.
 * @param device_type Simulated USB interface type.
 * @return Scripted host result for the current test scenario.
 */
int ast_radio_make_spkr_playback_value(int maximum, int requested, int device_type)
{
	(void)maximum;
	(void)device_type;
	return requested;
}
#endif

#ifdef URP_TEST_MODERN
static void test_modern_device_policy_helpers(void)
{
	struct chan_usbradio_pvt radio = {.name = "modern-test"};
	struct ast_radio_mixer_path tx_paths[3] = {0};
	struct ast_radio_mixer_path rx_paths[2] = {0};
	struct ast_radio_mixer_path sidetone_paths[1] = {0};
	struct ast_radio_mixer_path boost_paths[1] = {0};
	struct ast_radio_device device = {
		.devstr = "usb-modern",
		.serial = "serial-modern",
		.alsa_card = 7,
		.mixer_tx_paths = tx_paths,
		.mixer_tx_path_count = ARRAY_LEN(tx_paths),
		.mixer_rx_paths = rx_paths,
		.mixer_rx_path_count = ARRAY_LEN(rx_paths),
		.mixer_sidetone_paths = sidetone_paths,
		.mixer_sidetone_path_count = ARRAY_LEN(sidetone_paths),
		.mixer_rx_boost_paths = boost_paths,
		.mixer_rx_boost_path_count = ARRAY_LEN(boost_paths),
	};

	modern_stream_info_available = 1;
	modern_stream_info.outputLatency = 0.041;
	atomic_init(&radio.plus_radio_tx_active, 0);
	urp_radio_state radio_state = {0};
	char devstr[64];
	char serial[64];
	int card;

	assert(usbradio_log_fault(&radio, 0, "first fault %d\n", 1));
	assert(radio.usb_faulted);
	assert(usbradio_log_fault(&radio, 1, "repeat fault\n"));

	usbradio_device_identity(&radio, devstr, sizeof(devstr), serial, sizeof(serial), &card);
	assert(!devstr[0] && !serial[0] && card == -1);
	usbradio_device_identity(&radio, NULL, 0, NULL, 0, NULL);
	usbradio_device_identity(&radio, devstr, 0, serial, 0, NULL);
	radio.radio_device = &device;
	usbradio_device_identity(&radio, devstr, sizeof(devstr), serial, sizeof(serial), &card);
	assert(!strcmp(devstr, "usb-modern"));
	assert(!strcmp(serial, "serial-modern"));
	assert(card == 7);
	usbradio_device_identity(&radio, devstr, 0, serial, sizeof(serial), &card);
	usbradio_device_identity(&radio, devstr, sizeof(devstr), serial, 0, &card);
	device.serial = NULL;
	usbradio_device_identity(&radio, NULL, 0, serial, sizeof(serial), NULL);
	assert(!serial[0]);
	device.serial = "serial-modern";
	usbradio_log_usb_recovered(&radio);
	assert(!radio.usb_faulted);
	usbradio_log_usb_recovered(&radio);
	radio.radio_device = NULL;
	radio.usb_faulted = 1;
	usbradio_log_usb_recovered(&radio);

	radio.radio = &radio_state;
	radio.pa.output_channels = 2;
	radio.txmixa = TX_OUT_VOICE;
	radio.txmixb = TX_OUT_OFF;
	usbradio_adjust_txmix_for_mono(&radio);
	assert(radio.txmixb == TX_OUT_OFF);
	for (int a = TX_OUT_OFF; a <= TX_OUT_AUX; ++a) {
		for (int b = TX_OUT_OFF; b <= TX_OUT_AUX; ++b) {
			radio.pa.output_channels = 1;
			radio.txmixa = a;
			radio.txmixb = b;
			radio_state.txMixA = a;
			radio_state.txMixB = b;
			usbradio_adjust_txmix_for_mono(&radio);
			assert(radio.txmixa == radio_state.txMixA);
			assert(radio.txmixb == radio_state.txMixB);
		}
	}
	radio.radio = NULL;
	radio.pa.output_channels = 1;
	radio.txmixa = TX_OUT_VOICE;
	radio.txmixb = TX_OUT_LSD;
	usbradio_adjust_txmix_for_mono(&radio);
	radio.txmixa = TX_OUT_OFF;
	radio.txmixb = TX_OUT_VOICE;
	usbradio_adjust_txmix_for_mono(&radio);
	radio.radio = &radio_state;

	radio.radio_device = &device;
	usbradio_release_device(&radio);
	assert(!radio.radio_device);
	usbradio_release_device(&radio);

	radio.swap_state = DEVICE_SWAP_IDLE;
	usbradio_swap_begin(&radio);
	assert(radio.swap_state == DEVICE_SWAP_QUIESCING && !radio.swap_audio_ready);
	usbradio_swap_audio_stopped(&radio);
	assert(radio.swap_audio_ready);
	radio.stophid = 1;
	assert(usbradio_swap_hid_wait(&radio));
	assert(radio.swap_state == DEVICE_SWAP_READY);
	assert(usbradio_swap_ready(&radio));
	usbradio_swap_finish(&radio);
	assert(!usbradio_swap_ready(&radio));
	assert(radio.swap_state == DEVICE_SWAP_IDLE && !radio.swap_audio_ready);
	usbradio_swap_audio_stopped(&radio);
	radio.swap_state = DEVICE_SWAP_QUIESCING;
	radio.swap_audio_ready = 0;
	assert(!usbradio_swap_hid_wait(&radio));
	radio.stophid = 0;
	radio.swap_state = DEVICE_SWAP_QUIESCING;
	radio.swap_audio_ready = 0;
	modern_swap_first = &radio;
	modern_swap_second = &radio;
	modern_swap_wait_step = 0;
	assert(usbradio_swap_hid_wait(&radio));
	assert(radio.swap_state == DEVICE_SWAP_IDLE);
	modern_swap_first = NULL;
	modern_swap_second = NULL;

	radio.radio_device = &device;
	{
		int rx_max, tx_max, sidetone_max;
		usbradio_mixer_limits(&radio, &rx_max, &tx_max, &sidetone_max);
		assert(rx_max == 100 && tx_max == 100 && sidetone_max == 100);
		usbradio_set_sidetone_switch(&radio, 1);
		usbradio_set_rx_mixer(&radio, 50);
		radio.duplex3 = 500;
		radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
		modern_mixer_element_available = 0;
		mixer_write(&radio);
		modern_mixer_element_available = 1;
		mixer_write(&radio);
		radio.duplex3mode = DUPLEX3_MODE_SOFTWARE;
		mixer_write(&radio);
		modern_mixer_element_available = 0;
		radio.radio_device = NULL;
		usbradio_mixer_limits(&radio, &rx_max, &tx_max, &sidetone_max);
		assert(!rx_max && !tx_max && !sidetone_max);
		usbradio_set_sidetone_switch(&radio, 0);
		usbradio_set_rx_mixer(&radio, 0);
	}

	radio.radio_device = NULL;
	radio.device_error = AST_RADIO_DEVICE_READY;
	modern_acquire_device = &device;
	modern_acquire_result = AST_RADIO_DEVICE_WAIT;
	assert(init_audio_device(&radio) == -1);
	assert(radio.device_error == AST_RADIO_DEVICE_WAIT && radio.usb_faulted);
	assert(init_audio_device(&radio) == -1);
	modern_acquire_result = AST_RADIO_DEVICE_READY;
	modern_mixer_max = 0;
	modern_mixer_max_calls = 0;
	assert(init_audio_device(&radio) == -1);
	assert(radio.device_error == AST_RADIO_DEVICE_ERROR);
	modern_mixer_max = 100;
	modern_mixer_max_calls = 0;
	modern_mixer_max_failure_call = 2;
	assert(init_audio_device(&radio) == -1);
	modern_mixer_max_failure_call = 0;
	radio.devstr[0] = '\0';
	radio.serial[0] = '\0';
	modern_mixer_max_calls = 0;
	assert(init_audio_device(&radio) == 0);
	assert(radio.radio_device == &device && radio.device_error == AST_RADIO_DEVICE_READY);
	usbradio_release_device(&radio);
	strcpy(radio.devstr, "configured-device");
	modern_mixer_max_calls = 0;
	assert(init_audio_device(&radio) == 0);
	usbradio_release_device(&radio);
	radio.devstr[0] = '\0';
	strcpy(radio.serial, "configured-serial");
	modern_mixer_max_calls = 0;
	assert(init_audio_device(&radio) == 0);

	radio.pa.active = 1;
	assert(usbradio_start_audio(&radio) == 0);
	radio.pa.active = 0;
	radio.radio_device = NULL;
	assert(usbradio_start_audio(&radio) == -1);
	radio.radio_device = &device;
	device.pa_input_channels = 1;
	modern_open_result = paUnanticipatedHostError;
	assert(usbradio_start_audio(&radio) == -1);
	modern_open_result = paNoError;
	modern_start_result = paUnanticipatedHostError;
	assert(usbradio_start_audio(&radio) == -1);
	assert(!radio.pa.active);
	modern_start_result = paNoError;
	assert(usbradio_start_audio(&radio) == 0);
	assert(radio.pa.active && radio.pa.input_channels == 1);
	assert(radio.plus_portaudio_playout_hold_callbacks == 4U);
	ast_radio_pa_stop(&radio.pa);

	/* The playout hold has a conservative one-block fallback whenever PortAudio
	 * cannot supply a usable output-latency estimate. */
	modern_stream_info_available = 0;
	assert(usbradio_start_audio(&radio) == 0);
	assert(radio.plus_portaudio_playout_hold_callbacks == 1U);
	ast_radio_pa_stop(&radio.pa);
	modern_stream_info_available = 1;
	modern_stream_info.outputLatency = -1.0;
	assert(usbradio_start_audio(&radio) == 0);
	assert(radio.plus_portaudio_playout_hold_callbacks == 1U);
	ast_radio_pa_stop(&radio.pa);
	modern_stream_info.outputLatency = NAN;
	assert(usbradio_start_audio(&radio) == 0);
	assert(radio.plus_portaudio_playout_hold_callbacks == 1U);
	ast_radio_pa_stop(&radio.pa);
	modern_stream_info.outputLatency =
		((double)UINT_MAX * (double)URP_NATIVE_SAMPLES / (double)URP_RATE_NATIVE) + 1.0;
	assert(usbradio_start_audio(&radio) == 0);
	assert(radio.plus_portaudio_playout_hold_callbacks == UINT_MAX);
	ast_radio_pa_stop(&radio.pa);
	modern_open_without_stream = 1;
	assert(usbradio_start_audio(&radio) == 0);
	assert(radio.plus_portaudio_playout_hold_callbacks == 1U);
	ast_radio_pa_stop(&radio.pa);
	modern_open_without_stream = 0;
	modern_stream_info.outputLatency = 0.041;

	{
		short output[AST_RADIO_PA_48K_STEREO_SAMPLES] = {1};
		short *stereo = (short *)(radio.usbradio_read_buf + AST_FRIENDLY_OFFSET);

		radio.radio_device = NULL;
		radio.pa.active = 0;
		assert(soundcard_writeframe(&radio, output) == 0);
		radio.radio_device = &device;
		modern_open_result = paNoError;
		modern_start_result = paNoError;
		assert(soundcard_writeframe(&radio, output) > 0);
		radio.pa.active = 1;
		radio.radio = &radio_state;
		radio_state.txPttIn = radio_state.txPttOut = 0;
		atomic_store_explicit(&radio.plus_radio_tx_active, 0, memory_order_release);
		modern_write_result = paNoError;
		assert(soundcard_writeframe(&radio, output) ==
		       AST_RADIO_PA_48K_STEREO_SAMPLES * (int)sizeof(short));
		assert(modern_last_write == silence_buf);
		radio_state.txPttIn = radio_state.txPttOut = 1;
		atomic_store_explicit(&radio.plus_radio_tx_active, 1, memory_order_release);
		modern_write_result = paNoError;
		assert(soundcard_writeframe(&radio, output) > 0);
		assert(modern_last_write == output);
		assert(radio.plus_tx_playout_hold.callbacks_remaining == 4U);
		modern_write_result = paOutputUnderflowed;
		assert(soundcard_writeframe(&radio, output) > 0);
		assert(modern_last_write == output);
		assert(radio.plus_tx_playout_hold.callbacks_remaining == 4U);
		radio.hasusb = 1;
		modern_write_result = paUnanticipatedHostError;
		assert(soundcard_writeframe(&radio, output) == 0);
		assert(!radio.hasusb && !radio.pa.active);

		radio.pa.input_channels = 1;
		modern_read_sample = 1234;
		modern_read_result = paNoError;
		assert(usbradio_read_pa_stereo(&radio) == paNoError);
		assert(stereo[0] == 1234 && stereo[1] == 1234);
		modern_read_result = paInputOverflowed;
		assert(usbradio_read_pa_stereo(&radio) == paInputOverflowed);
		radio.pa.input_channels = 2;
		modern_read_result = paNoError;
		assert(usbradio_read_pa_stereo(&radio) == paNoError);

		radio.pa.active = 1;
		radio.audio_thread_ready = 1;
		radio.swap_state = DEVICE_SWAP_QUIESCING;
		stream_cleanup(&radio);
		assert(!radio.pa.active && !radio.audio_thread_ready && radio.swap_audio_ready);
		assert(!radio.plus_portaudio_playout_hold_callbacks);
	}
}

static void test_modern_channel_callbacks(void)
{
	struct chan_usbradio_pvt radio = {.name = "modern-test"};
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	struct ast_module_info module_info = {0};
	struct ast_format_cap *formats = (struct ast_format_cap *)(uintptr_t)1;
	short samples[URP_LINK_SAMPLES] = {1};
	struct ast_frame frame = {
		.frametype = AST_FRAME_VOICE,
		.data.ptr = samples,
		.datalen = sizeof(samples),
	};
	urp_radio_state radio_state = {0};
	urp_radio_state radio_configuration = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	int cause = 0;

	assert(!rpcr_init(&radio.plus_program_ring, URP_PROGRAM_RING_SAMPLES, RPCR_SINC_BEST));
	test_channel_private = &radio;
	radio.radio = &radio_state;
	assert(usbradio_read(channel) == &ast_null_frame);
	assert(usbradio_write(channel, &frame) == 0);
	radio.hasusb = 1;
	assert(usbradio_write(channel, &frame) == 0);
	radio.audio_thread_ready = 1;
	radio.echoing = 1;
	assert(usbradio_write(channel, &frame) == 0);
	radio.plus_advanced = 1;
	assert(usbradio_write(channel, &frame) == 0);
	radio.plus_advanced = 0;
	radio.echoing = 0;
	assert(usbradio_write(channel, &frame) == 0);
	radio.txkeyed = 1;
	frame.frametype = AST_FRAME_NULL;
	assert(usbradio_write(channel, &frame) == 0);
	frame.frametype = AST_FRAME_VOICE;
	frame.data.ptr = NULL;
	assert(usbradio_write(channel, &frame) == 0);
	frame.data.ptr = samples;
	assert(usbradio_write(channel, &frame) == 0);
	assert(rpcr_available(&radio.plus_program_ring) > 0);
	rpcr_destroy(&radio.plus_program_ring);

	radio.hidthread = radio.audiothread = AST_PTHREADT_NULL;
	pthread_create_calls = 0;
	fail_pthread_create_call = 1;
	assert(usbradio_call(channel, "destination", 1000) == -1);
	pthread_create_calls = 0;
	fail_pthread_create_call = 2;
	assert(usbradio_call(channel, "destination", 1000) == -1);
	assert(radio.hidthread == AST_PTHREADT_NULL);
	pthread_create_calls = 0;
	fail_pthread_create_call = 0;
	assert(usbradio_call(channel, "destination", 1000) == 0);
	assert(pthread_create_calls == 2 && setstate_calls > 0);
	/* Existing workers are reused on a repeated call. */
	assert(usbradio_call(channel, "destination", 1000) == 0);
	assert(pthread_create_calls == 2);

	usbradioplus_test_set_module_info(&module_info);
	radio.owner = channel;
	radio.hookstate = 1;
	radio.pa.active = 1;
	assert(usbradio_hangup(channel) == 0);
	assert(!radio.owner && !radio.hookstate && !radio.pa.active);
	assert(radio.hidthread == AST_PTHREADT_NULL && radio.audiothread == AST_PTHREADT_NULL);
	radio.owner = channel;
	radio.hookstate = 0;
	test_channel_private = &radio;
	assert(usbradio_hangup(channel) == 0);

	radio.owner = NULL;
	fail_channel_alloc = 1;
	assert(!usbradio_new(&radio, "s", "default", AST_STATE_DOWN, NULL, NULL));
	fail_channel_alloc = 0;
	assert(usbradio_new(&radio, "s", "default", AST_STATE_DOWN, NULL, NULL));
	radio.owner = NULL;
	pbx_start_result = 0;
	assert(usbradio_new(&radio, "s", "default", AST_STATE_UP, NULL, NULL));
	radio.owner = NULL;
	pbx_start_result = 1;
	hangup_calls = 0;
	assert(!usbradio_new(&radio, "s", "default", AST_STATE_UP, NULL, NULL));
	assert(hangup_calls == 1);
	pbx_start_result = 0;

	usbradio_default.next = NULL;
	assert(!usbradio_request("RadioPlus", formats, NULL, NULL, "missing", &cause));
	usbradio_default.next = &radio;
	radio.next = NULL;
	format_compatible = 0;
	assert(!usbradio_request("RadioPlus", formats, NULL, NULL, "modern-test", &cause));
	format_compatible = 1;
	radio.owner = channel;
	assert(!usbradio_request("RadioPlus", formats, NULL, NULL, "modern-test", &cause));
	assert(cause == AST_CAUSE_BUSY);
	radio.owner = NULL;
	fail_channel_alloc = 1;
	assert(!usbradio_request("RadioPlus", formats, NULL, NULL, "modern-test", &cause));
	fail_channel_alloc = 0;
	radio.owner = NULL;
	radio.radio = urp_radio_create(&radio_configuration, URP_LINK_SAMPLES);
	assert(radio.radio);
	assert(usbradio_request("RadioPlus", formats, NULL, NULL, "modern-test", &cause));
	assert(!radio.remoted);

	usbradioplus_test_set_module_info(NULL);
	test_channel_private = NULL;
	usbradio_default.next = NULL;
	urp_radio_destroy(radio.radio);
}

static void test_modern_hid_worker_baseline(void)
{
	struct chan_usbradio_pvt radio = {0};
	struct chan_usbradio_pvt constructed = {0};
	struct chan_usbradio_pvt inactive = {.name = "inactive", .radioactive = 1};
	struct ast_radio_device device = {
		.devstr = "usb-modern",
		.serial = "serial-modern",
		.usb_device = (libusb_device *)(uintptr_t)1,
		.pa_input_channels = 2,
		.pa_output_channels = 2,
	};
	urp_radio_state configuration = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};

	settings_defaults(&settings);
	settings.profiles[0].enabled = 0;
	strcpy(settings.profiles[0].name, "modern-hid");
	strcpy(settings.profiles[0].channel, "RadioPlus/modern-hid");
	radio.name = "modern-hid";
	radio.pttkick[0] = radio.pttkick[1] = -1;
	radio.plus_app_rpt_rate = URP_RATE_LINK;
	radio.plus_app_rpt_samples = URP_LINK_SAMPLES;
	radio.plus_deemphasis_corner_hz = 300.0;
	radio.plus_preemphasis_corner_hz = 300.0;
	radio.wanteeprom = 1;
	radio.gpios[0] = "in";
	radio.gpios[1] = "out";
	radio.gpios[2] = "in";
	radio.valid_gpios = 1;
	radio.pps[2] = "ptt";
	radio.pps[3] = "output";
	radio.pps[10] = "in";
	radio.pps[11] = "out";
	radio.pps[12] = "cor";
	radio.pps[13] = "ctcss";
	radio.hid_gpio_pulsetimer[0] = 100;
	radio.radio = urp_radio_create(&configuration, URP_LINK_SAMPLES);
	assert(radio.radio);
	/* The HID worker consumes only the audio worker's atomic PTT request.  A
	 * direct signaling-state write must not be mistaken for a physical key
	 * request in this worker-only fixture. */
	usbradioplus_publish_hardware_ptt(&radio, 1);
	modern_acquire_device = &device;
	modern_acquire_result = AST_RADIO_DEVICE_READY;
	modern_mixer_max = 100;
	device.product_id = C108AH_PRODUCT_ID;
	modern_libusb_open_result = 0;
	modern_libusb_claim_result = 0;
	modern_libusb_detach_result = 0;
	modern_open_result = paNoError;
	modern_start_result = paNoError;
	modern_hid_input_calls = 0;
	memset(modern_hid_inputs, 0xff, sizeof(modern_hid_inputs));
	modern_eeprom_result = 0;
	modern_eeprom_valid_magic = 1;
	modern_eeprom_write_after_read = &radio;
	haspp = 2;
	mock_parallel_inputs = 0xff;
	modern_stop_hid_target = &radio;
	mock_poll_enabled = 1;
	mock_poll_result = 0;
	variable_browse_calls = 0;
	assert(hidthread(&radio) == NULL);
	settings_defaults(&settings);
	assert(radio.stophid && modern_hid_input_calls == 1);
	assert(!radio.radio_device);
	/* The audio worker posts clipping asynchronously; only the HID worker may
	 * turn that request into a timed physical GPIO pulse. */
	radio.stophid = 0;
	radio.clipledgpio = 1;
	radio.hid_gpio_pulsetimer[0] = 0;
	atomic_store_explicit(&radio.plus_clip_led_request, 1, memory_order_release);
	modern_hid_input_calls = 0;
	modern_stop_hid_target = &radio;
	assert(hidthread(&radio) == NULL);
	assert(radio.hid_gpio_pulsetimer[0] == CLIP_LED_HOLD_TIME_MS);
	assert(!atomic_load_explicit(&radio.plus_clip_led_request, memory_order_acquire));
	/* A clear request, an unavailable LED, and an active pulse all consume no
	 * additional GPIO time. */
	radio.stophid = 0;
	radio.clipledgpio = 1;
	radio.hid_gpio_pulsetimer[0] = 0;
	atomic_store_explicit(&radio.plus_clip_led_request, 0, memory_order_release);
	modern_hid_input_calls = 0;
	modern_stop_hid_target = &radio;
	assert(hidthread(&radio) == NULL);
	assert(radio.hid_gpio_pulsetimer[0] == 0);
	radio.stophid = 0;
	radio.clipledgpio = 0;
	atomic_store_explicit(&radio.plus_clip_led_request, 1, memory_order_release);
	modern_hid_input_calls = 0;
	modern_stop_hid_target = &radio;
	assert(hidthread(&radio) == NULL);
	assert(!atomic_load_explicit(&radio.plus_clip_led_request, memory_order_acquire));
	{
		const long saved_clip_time_step = mock_tvnow_step;

		mock_tvnow_step = 0;
		radio.stophid = 0;
		radio.clipledgpio = 1;
		radio.hid_gpio_pulsetimer[0] = 1;
		atomic_store_explicit(&radio.plus_clip_led_request, 1, memory_order_release);
		modern_hid_input_calls = 0;
		modern_stop_hid_target = &radio;
		assert(hidthread(&radio) == NULL);
		assert(radio.hid_gpio_pulsetimer[0] == 1);
		assert(!atomic_load_explicit(&radio.plus_clip_led_request, memory_order_acquire));
		mock_tvnow_step = saved_clip_time_step;
	}
	radio.clipledgpio = 0;
	radio.hid_gpio_pulsetimer[0] = 0;
	modern_stop_hid_target = NULL;
	mock_poll_enabled = 0;
	ast_radio_pa_stop(&radio.pa);
	urp_radio_destroy(radio.radio);
	if (radio.pttkick[0] >= 0)
		close(radio.pttkick[0]);
	if (radio.pttkick[1] >= 0)
		close(radio.pttkick[1]);

	constructed.name = "modern-constructed";
	constructed.pttkick[0] = constructed.pttkick[1] = -1;
	constructed.plus_app_rpt_rate = URP_RATE_LINK;
	constructed.plus_app_rpt_samples = URP_LINK_SAMPLES;
	constructed.plus_deemphasis_corner_hz = 300.0;
	constructed.plus_preemphasis_corner_hz = 300.0;
	constructed.txmixa = TX_OUT_OFF;
	constructed.txmixb = TX_OUT_OFF;
	constructed.txpreemphasis = 1;
	constructed.wanteeprom = 1;
	constructed.radioactive = 1;
	constructed.invertptt = 1;
	constructed.gpios[0] = "in";
	constructed.valid_gpios = 1;
	constructed.had_gpios_in = 1;
	constructed.last_gpios_in = 0;
	constructed.pps[10] = "in";
	constructed.had_pp_in = 1;
	constructed.last_pp_in = 0;
	strcpy(constructed.txctcssdefault, "100.0");
	strcpy(constructed.txctcssfreq, "100.0");
	device.product_id = C108_PRODUCT_ID + 1;
	modern_eeprom_valid_magic = 0;
	modern_eeprom_result = 1;
	modern_eeprom_write_after_read = NULL;
	mock_tvnow_step = 10;
	constructed.hid_gpio_pulsetimer[0] = 1;
	inactive.next = &constructed;
	usbradio_default.next = &inactive;
	modern_stop_hid_target = &constructed;
	assert(hidthread(&constructed) == NULL);
	assert(constructed.radio && constructed.devtype == C108_PRODUCT_ID);
	assert(!inactive.radioactive && constructed.radioactive);
	modern_stop_hid_target = NULL;
	modern_eeprom_result = 1;
	modern_eeprom_valid_magic = 0;
	modern_eeprom_write_after_read = NULL;
	memset(modern_hid_inputs, 0, sizeof(modern_hid_inputs));
	haspp = 0;
	mock_parallel_inputs = 0;
	mock_tvnow_step = 0;
	ast_radio_pa_stop(&constructed.pa);
	urp_radio_destroy(constructed.radio);
	if (constructed.pttkick[0] >= 0)
		close(constructed.pttkick[0]);
	if (constructed.pttkick[1] >= 0)
		close(constructed.pttkick[1]);
	usbradio_default.next = NULL;
}

/** @brief Verify a parallel-port PTT release is not gated by radio programming. */
static void test_modern_parallel_ptt_release(void)
{
	struct chan_usbradio_pvt radio = {0};
	unsigned char outputs[4] = {0};
	unsigned char saved[4] = {0};
	const int saved_haspp = haspp;
	const int8_t saved_pp_val = pp_val;
	const int saved_parallel_write_calls = parallel_write_calls;

	radio.pps[2] = "ptt";
	radio.hid_gpio_loc = 0;
	radio.hid_gpio_ctl_loc = 1;
	radio.hid_io_ptt = 1;
	radio.hid_gpio_val = 1;
	atomic_init(&radio.plus_hardware_ptt_applied, 1);
	haspp = 2;
	pp_val = (int8_t)0xff;
	parallel_write_calls = 0;
	/* A normal key-up must not run the unkey-only parallel clear path. */
	hidthread_apply_ptt(&radio, (struct libusb_device_handle *)(uintptr_t)1, outputs, saved, 1,
			    NULL, 0);
	assert(radio.lasttx);
	assert(atomic_load_explicit(&radio.plus_hardware_ptt_applied, memory_order_acquire));
	parallel_write_calls = 0;
	hidthread_apply_ptt(&radio, (struct libusb_device_handle *)(uintptr_t)1, outputs, saved, 0,
			    NULL, 0);
	assert(!radio.lasttx);
	assert(!atomic_load_explicit(&radio.plus_hardware_ptt_applied, memory_order_acquire));
	assert(parallel_write_calls > 0);
	haspp = saved_haspp;
	pp_val = saved_pp_val;
	parallel_write_calls = saved_parallel_write_calls;
}

static void run_modern_audio_iteration(struct chan_usbradio_pvt *radio)
{
	radio->stopaudiothread = 0;
	radio->hasusb = 1;
	radio->pa.active = 1;
	modern_stop_audio_target = radio;
	assert(usbradio_audio_thread(radio) == NULL);
	modern_stop_audio_target = NULL;
}

static void test_modern_audio_worker_baseline(void)
{
	struct chan_usbradio_pvt radio = {0};
	struct ast_radio_device device = {
		.devstr = "usb-modern",
		.pa_input_channels = 2,
		.pa_output_channels = 2,
	};
	urp_radio_state configuration = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};

	settings_defaults(&settings);
	settings.profiles[0].enabled = 0;
	strcpy(settings.profiles[0].name, "modern-audio");
	strcpy(settings.profiles[0].channel, "RadioPlus/modern-audio");
	radio.name = "modern-audio";
	radio.radio_device = &device;
	radio.radio = urp_radio_create(&configuration, URP_LINK_SAMPLES);
	assert(radio.radio);
	radio.hasusb = 1;
	radio.pa.active = 1;
	radio.pa.input_channels = 2;
	radio.pa.output_channels = 2;
	radio.plus_app_rpt_rate = URP_RATE_LINK;
	radio.plus_app_rpt_samples = URP_LINK_SAMPLES;
	radio.plus_deemphasis_corner_hz = 300.0;
	radio.plus_preemphasis_corner_hz = 300.0;
	urp_sample_queue_init(&radio.echo_queue, radio.echo_samples, URP_ECHO_QUEUE_SAMPLES);
	assert(usbradioplus_dsp_init(&radio) == 0);
	modern_read_result = paNoError;
	modern_read_sample = 0;
	modern_write_result = paNoError;
	modern_write_available = AST_RADIO_PA_FRAMES_PER_BUFFER;
	run_modern_audio_iteration(&radio);
	assert(radio.stopaudiothread && !radio.audio_thread_ready && !radio.pa.active);

	/* A parser reload must keep PortAudio paced with silence without consuming
	 * signaling state; its write path also has terminal and full-queue cases. */
	memset(radio.usbradio_read_buf_8k, 0x5a, sizeof(radio.usbradio_read_buf_8k));
	memset(radio.usbradio_write_buf, 0x5a, sizeof(radio.usbradio_write_buf));
	atomic_store_explicit(&radio.plus_radio_tx_active, 1, memory_order_release);
	atomic_store_explicit(&radio.plus_radio_access.reconfiguring, 1, memory_order_seq_cst);
	modern_write_result = paNoError;
	run_modern_audio_iteration(&radio);
	assert(!atomic_load_explicit(&radio.plus_radio_tx_active, memory_order_acquire));
	assert(!memcmp(radio.usbradio_read_buf_8k + AST_FRIENDLY_OFFSET, silence_buf,
		       radio.plus_app_rpt_samples * sizeof(short)));
	assert(!memcmp(radio.usbradio_write_buf, silence_buf, sizeof(radio.usbradio_write_buf)));
	modern_write_available = 0;
	run_modern_audio_iteration(&radio);
	modern_write_available = AST_RADIO_PA_FRAMES_PER_BUFFER;
	modern_write_result = paUnanticipatedHostError;
	run_modern_audio_iteration(&radio);
	assert(!radio.pa.active);
	modern_write_result = paNoError;
	atomic_store_explicit(&radio.plus_radio_access.reconfiguring, 0, memory_order_seq_cst);

	/* The worker can open and start an inactive stream, not only reuse one. */
	radio.stopaudiothread = 0;
	radio.hasusb = 1;
	radio.pa.active = 0;
	modern_stop_audio_target = &radio;
	assert(usbradio_audio_thread(&radio) == NULL);
	modern_stop_audio_target = NULL;

	modern_read_result = paTimedOut;
	run_modern_audio_iteration(&radio);
	modern_read_result = paInputOverflowed;
	run_modern_audio_iteration(&radio);
	modern_read_result = paUnanticipatedHostError;
	run_modern_audio_iteration(&radio);
	modern_read_result = paNoError;
	modern_write_available = -1;
	run_modern_audio_iteration(&radio);
	modern_write_available = AST_RADIO_PA_FRAMES_PER_BUFFER;
	modern_write_result = paUnanticipatedHostError;
	radio.txkeyed = 1;
	run_modern_audio_iteration(&radio);
	modern_write_result = paNoError;
	radio.txkeyed = 0;
	/* PortAudio's underflow recovery writes a priming frame but does not advance
	 * the transmitter's submitted-frame accounting. */
	modern_write_result = paOutputUnderflowed;
	run_modern_audio_iteration(&radio);
	modern_write_result = paNoError;
	/* Isolate the following receive checks from the preceding transmit-error
	 * scenario, including its deliberately conservative post-playout blanking. */
	radio.radio->txPttIn = 0;
	radio.radio->txPttOut = 0;
	radio.radio->txState = CHAN_TXSTATE_IDLE;
	radio.radio->txrxblankingtime = 0;
	radio.radio->txrxblankingtimer = 0;
	usbradioplus_tx_playout_hold_reset(&radio);
	radio.owner = (struct ast_channel *)(uintptr_t)1;
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_HID;
	radio.rxhidsq = 1;
	radio.rxhidctcss = 1;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CARRIER |
							     URP_HARDWARE_INPUT_HID_CTCSS);
	radio.radioduplex = 1;
	radio.rxctcssdecode = 1;
	strcpy(radio.rxctcssfreq, "100.0");
	radio.duplex3 = 999;
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
	run_modern_audio_iteration(&radio);
	assert(radio.rxkeyed && radio.lastrx);
	/* A post-transmit blanking interval overrides an otherwise valid carrier. */
	radio.radio->txrxblankingtimer = 2 * MS_PER_FRAME;
	run_modern_audio_iteration(&radio);
	assert(!radio.rx_cos_active);
	radio.radio->txrxblankingtimer = 0;

	/* Advanced transport delivers hardware-rate frames through this worker too. */
	usbradioplus_interface_mode(&radio, 1);
	run_modern_audio_iteration(&radio);
	assert(radio.read_f.subclass.format == ast_format_cache_get_slin_by_rate(48000));
	assert(radio.read_f.samples == URP_NATIVE_SAMPLES);
	mixer_write(&radio);
	radio.rxhidsq = 0;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CTCSS);
	run_modern_audio_iteration(&radio);
	assert(!radio.rxkeyed && !radio.lastrx);
	radio.rxhidsq = 1;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CARRIER |
							     URP_HARDWARE_INPUT_HID_CTCSS);
	run_modern_audio_iteration(&radio);
	assert(radio.rxkeyed && radio.lastrx);
	usbradioplus_interface_mode(&radio, 0);

	radio.rxhidsq = 0;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CTCSS);
	run_modern_audio_iteration(&radio);
	assert(!radio.rxkeyed && !radio.lastrx);

	/* A nonzero, current HID timestamp is healthy. */
	radio.lasthidtime = 1234;
	run_modern_audio_iteration(&radio);
	radio.lasthidtime = 0;

	radio.rxcdtype = CD_HID_INVERT;
	radio.rxhidsq = 0;
	radio.rxsdtype = SD_HID_INVERT;
	radio.rxhidctcss = 0;
	usbradioplus_publish_hardware_inputs(&radio, 0U);
	run_modern_audio_iteration(&radio);
	radio.radio->rxExtCarrierDetect = 1;
	run_modern_audio_iteration(&radio);
	radio.rxcdtype = CD_HID;
	radio.rxhidsq = 1;
	radio.rxsdtype = SD_PP;
	radio.rxppctcss = 1;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CARRIER |
							     URP_HARDWARE_INPUT_PARALLEL_CTCSS);
	run_modern_audio_iteration(&radio);
	radio.rxsdtype = SD_PP_INVERT;
	radio.rxppctcss = 0;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CARRIER);
	run_modern_audio_iteration(&radio);

	radio.txoffdelay = 1;
	radio.txkeyed = 1;
	run_modern_audio_iteration(&radio);
	radio.txkeyed = 0;
	radio.txoffcnt = MS_TO_FRAMES(TX_OFF_DELAY_MAX);
	radio.radio->txPttOut = 0;
	run_modern_audio_iteration(&radio);
	assert(radio.txoffcnt == MS_TO_FRAMES(TX_OFF_DELAY_MAX));
	radio.txoffdelay = 0;

	radio.echomode = 1;
	radio.echomax = 2;
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_HID;
	radio.rxhidsq = radio.rxhidctcss = 1;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CARRIER |
							     URP_HARDWARE_INPUT_HID_CTCSS);
	radio.rxkeyed = radio.lastrx = 0;
	run_modern_audio_iteration(&radio);
	/* A second keyed frame traverses an existing queued echo frame. */
	run_modern_audio_iteration(&radio);
	radio.rxhidsq = 0;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CTCSS);
	radio.rxkeyed = 0;
	run_modern_audio_iteration(&radio);

	radio.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	radio.rxhidsq = radio.rxhidctcss = 1;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CARRIER |
							     URP_HARDWARE_INPUT_HID_CTCSS);
	radio.rxkeyed = radio.lastrx = 0;
	run_modern_audio_iteration(&radio);
	radio.rxhidsq = 0;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CTCSS);
	run_modern_audio_iteration(&radio);
	radio.echomode = 0;
	run_modern_audio_iteration(&radio);

	radio.rxctcssoverride = 1;
	radio.rxhidsq = 1;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CARRIER |
							     URP_HARDWARE_INPUT_HID_CTCSS);
	radio.rxondelay = 2;
	radio.rxoncnt = 0;
	radio.rxkeyed = radio.lastrx = 0;
	run_modern_audio_iteration(&radio);
	assert(radio.rxoncnt == 1 && !radio.rxkeyed);
	run_modern_audio_iteration(&radio);
	radio.rxondelay = 0;
	radio.rxctcssoverride = 0;
	radio.rxhidsq = 1;
	radio.rxhidctcss = 0;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CARRIER);
	radio.rxsdtype = SD_HID;
	radio.radio->b.ctcssRxEnable = 1;
	radio.radio->rxCtcss->decode = CTCSS_NULL;
	run_modern_audio_iteration(&radio);
	radio.radio->b.ctcssRxEnable = 0;
	radio.rxcdtype = CD_IGNORE;
	radio.rxsdtype = SD_HID;
	run_modern_audio_iteration(&radio);
	radio.rxsdtype = SD_IGNORE;
	run_modern_audio_iteration(&radio);
	radio.rxsdtype = SD_HID;
	run_modern_audio_iteration(&radio);
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_HID;

	mock_audio_clipping = 1;
	radio.clipledgpio = 0;
	run_modern_audio_iteration(&radio);
	radio.clipledgpio = 1;
	radio.hid_gpio_pulsetimer[0] = 0;
	run_modern_audio_iteration(&radio);
	/* The audio callback only requests a clip indication.  The HID worker owns
	 * the GPIO pulse timer and consumes this request on its next iteration. */
	assert(atomic_load_explicit(&radio.plus_clip_led_request, memory_order_acquire));
	atomic_store_explicit(&radio.plus_clip_led_request, 0, memory_order_release);
	mock_audio_clipping = 0;
	radio.clipledgpio = 0;

	radio.radio->b.txCtcssReady = 1;
	radio.sendvoter = 1;
	radio.count_rssi_update = 1;
	radio.rxhidctcss = 1;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CARRIER |
							     URP_HARDWARE_INPUT_HID_CTCSS);
	radio.rxkeyed = 1;
	run_modern_audio_iteration(&radio);
	assert(!radio.radio->b.txCtcssReady && radio.count_rssi_update == 10);
	radio.sendvoter = 0;
	radio.sendvoter = 1;
	radio.count_rssi_update = 2;
	radio.rxkeyed = 1;
	radio.rxctcssoverride = 1;
	run_modern_audio_iteration(&radio);
	radio.rxctcssoverride = 0;
	radio.sendvoter = 0;
	radio.sendvoter = 1;
	radio.count_rssi_update = 0;
	run_modern_audio_iteration(&radio);
	radio.count_rssi_update = 1;
	radio.rxkeyed = 0;
	radio.rxhidsq = 0;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CTCSS);
	run_modern_audio_iteration(&radio);
	radio.rxhidsq = 1;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CARRIER |
							     URP_HARDWARE_INPUT_HID_CTCSS);
	radio.sendvoter = 0;

	radio.usedtmf = 1;
	radio.dsp = (struct ast_dsp *)(uintptr_t)1;
	dsp_result_type = AST_FRAME_DTMF_END;
	dsp_result_digit = 'm';
	run_modern_audio_iteration(&radio);
	dsp_result_digit = 'u';
	run_modern_audio_iteration(&radio);
	dsp_result_digit = '5';
	option_verbose = 1;
	run_modern_audio_iteration(&radio);
	option_verbose = 0;
	dsp_result_type = AST_FRAME_DTMF_END;
	dsp_result_digit = '7';
	run_modern_audio_iteration(&radio);
	dsp_result_type = AST_FRAME_DTMF_BEGIN;
	dsp_result_digit = '6';
	radio.toneflag = 0;
	run_modern_audio_iteration(&radio);
	radio.toneflag = 1;
	run_modern_audio_iteration(&radio);
	dsp_result_type = AST_FRAME_VOICE;
	run_modern_audio_iteration(&radio);
	dsp_result_type = -1;
	radio.usedtmf = 0;
	radio.dsp = NULL;
	/* DTMF enabled without an allocated detector must pass voice unchanged. */
	radio.usedtmf = 1;
	run_modern_audio_iteration(&radio);
	radio.usedtmf = 0;

	/* A temporarily full output buffer skips the write without faulting the
	 * stream. A clip indication already being held is not restarted. */
	modern_write_available = 0;
	run_modern_audio_iteration(&radio);
	modern_write_available = AST_RADIO_PA_FRAMES_PER_BUFFER;
	mock_audio_clipping = 1;
	radio.clipledgpio = 1;
	radio.hid_gpio_pulsetimer[0] = 1;
	run_modern_audio_iteration(&radio);
	mock_audio_clipping = 0;
	radio.clipledgpio = 0;

	/* Cover transmitter state that is already asserted and test-key-only PTT. */
	radio.txkeyed = 1;
	radio.radio->txPttIn = 1;
	run_modern_audio_iteration(&radio);
	radio.txkeyed = 0;
	radio.txtestkey = 1;
	radio.radio->txPttIn = 0;
	run_modern_audio_iteration(&radio);
	radio.txtestkey = 0;

	/* Half-duplex transmission suppresses an otherwise valid carrier. */
	radio.rxcdtype = CD_HID_INVERT;
	radio.rxhidsq = 0;
	radio.radio->rxExtCarrierDetect = 1;
	run_modern_audio_iteration(&radio);
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_HID;
	radio.rxhidsq = radio.rxhidctcss = 1;
	radio.radio->txPttIn = 0;
	radio.radio->txPttOut = 0;
	radio.radio->txState = CHAN_TXSTATE_IDLE;
	radio.txkeyed = 1;
	radio.radioduplex = 0;
	radio.rxkeyed = radio.lastrx = 0;
	run_modern_audio_iteration(&radio);
	assert(!radio.rx_cos_active);
	radio.txkeyed = 0;
	radio.radio->txPttOut = 0;
	radio.radioduplex = 1;

	/* An advanced transport remains eligible for COR while its local PTT is
	 * asserted; this takes the middle full-duplex admission arm. */
	radio.radio->txPttIn = radio.radio->txPttOut = 1;
	radio.radio->txState = CHAN_TXSTATE_ACTIVE;
	radio.txkeyed = 1;
	radio.plus_advanced = 1;
	radio.radioduplex = 0;
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_HID;
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CARRIER |
							     URP_HARDWARE_INPUT_HID_CTCSS);
	run_modern_audio_iteration(&radio);
	assert(radio.radio->txPttOut && radio.rx_cos_active);
	radio.txkeyed = 0;
	radio.plus_advanced = 0;
	radio.radioduplex = 1;
	radio.radio->txPttIn = radio.radio->txPttOut = 0;
	radio.radio->txState = CHAN_TXSTATE_IDLE;
	usbradioplus_tx_playout_hold_reset(&radio);

	/* Both delay predicates are evaluated while receive qualification waits. */
	radio.rxhidsq = radio.rxhidctcss = 1;
	radio.rxctcssoverride = 1;
	radio.rxkeyed = 0;
	radio.rxoncnt = 0;
	radio.rxondelay = 2;
	radio.txoffcnt = 0;
	radio.txoffdelay = 2;
	run_modern_audio_iteration(&radio);
	radio.rxondelay = 0;
	radio.txoffdelay = 0;
	radio.rxctcssoverride = 0;

	/* Delay counters exercise their unsaturated and already-keyed paths. */
	radio.txoffdelay = 10;
	radio.txoffcnt = 0;
	radio.rxkeyed = 1;
	run_modern_audio_iteration(&radio);
	radio.txoffdelay = 0;
	radio.rxkeyed = 0;

	/* A full legacy echo queue and an allocation failure both drop the current
	 * frame safely. */
	radio.echomode = 1;
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
	radio.rxkeyed = radio.lastrx = 1;
	radio.echoing = 1;
	run_modern_audio_iteration(&radio);
	radio.echoing = 0;
	radio.echomax = 0;
	run_modern_audio_iteration(&radio);
	radio.echomax = 2;
	ast_calloc_calls = 0;
	fail_ast_calloc_call = 1;
	run_modern_audio_iteration(&radio);
	fail_ast_calloc_call = 0;
	radio.echomode = 0;

	/* Signaling transitions remain safe without an attached channel, and with
	 * local-repeat sidetone disabled. */
	radio.owner = NULL;
	radio.plus_advanced = 1;
	radio.duplex3 = 999;
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_HID;
	radio.rxctcssoverride = 0;
	radio.rxondelay = radio.txoffdelay = 0;
	radio.radio->txPttIn = radio.radio->txPttOut = 0;
	radio.radio->txState = CHAN_TXSTATE_IDLE;
	radio.radio->txrxblankingtimer = 0;
	usbradioplus_tx_playout_hold_reset(&radio);
	usbradioplus_publish_hardware_inputs(&radio, 0U);
	radio.rxkeyed = 1;
	radio.lastrx = 1;
	run_modern_audio_iteration(&radio);
	assert(!radio.rxkeyed && !radio.lastrx);
	/* A hardware key transition follows the same owner-free advanced path. */
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CARRIER |
							     URP_HARDWARE_INPUT_HID_CTCSS);
	radio.rxkeyed = 0;
	radio.lastrx = 0;
	run_modern_audio_iteration(&radio);
	assert(radio.rxkeyed && radio.lastrx);
	radio.plus_advanced = 0;
	radio.duplex3 = 0;
	usbradioplus_publish_hardware_inputs(&radio, 0U);
	radio.rxkeyed = radio.lastrx = 1;
	run_modern_audio_iteration(&radio);
	assert(!radio.rxkeyed && !radio.lastrx);
	usbradioplus_publish_hardware_inputs(&radio, URP_HARDWARE_INPUT_HID_CARRIER |
							     URP_HARDWARE_INPUT_HID_CTCSS);
	radio.rxkeyed = radio.lastrx = 0;
	run_modern_audio_iteration(&radio);
	assert(radio.rxkeyed && radio.lastrx);
	radio.owner = (struct ast_channel *)(uintptr_t)1;
	channel_state = AST_STATE_DOWN;
	run_modern_audio_iteration(&radio);
	channel_state = AST_STATE_UP;
	radio.rxctcssdecode = 0;
	radio.rxkeyed = 0;
	radio.lastrx = 0;
	radio.rxhidsq = radio.rxhidctcss = 1;
	run_modern_audio_iteration(&radio);

	radio.lasthidtime = 1;
	radio.stopaudiothread = 0;
	radio.hasusb = 1;
	radio.pa.active = 1;
	modern_stop_audio_on_usleep = &radio;
	assert(usbradio_audio_thread(&radio) == NULL);
	modern_stop_audio_on_usleep = NULL;
	radio.lasthidtime = 0;

	radio.stopaudiothread = 0;
	radio.hasusb = 1;
	radio.pa.active = 1;
	radio.rxkeyed = 1;
	radio.duplex3 = 1;
	modern_radio_time_calls = 0;
	modern_drop_hasusb_on_time = &radio;
	modern_drop_hasusb_on_time_call = 3;
	modern_stop_audio_on_usleep = &radio;
	assert(usbradio_audio_thread(&radio) == NULL);
	modern_stop_audio_on_usleep = NULL;
	modern_drop_hasusb_on_time = NULL;
	radio.duplex3 = 0;

	/* USB loss with no keyed receiver skips signaling teardown. */
	radio.stopaudiothread = 0;
	radio.hasusb = 1;
	radio.pa.active = 1;
	radio.rxkeyed = 0;
	modern_radio_time_calls = 0;
	modern_drop_hasusb_on_time = &radio;
	modern_drop_hasusb_on_time_call = 3;
	modern_stop_audio_on_usleep = &radio;
	assert(usbradio_audio_thread(&radio) == NULL);
	modern_stop_audio_on_usleep = NULL;
	modern_drop_hasusb_on_time = NULL;

	/* Losing USB after the final queued frame reaches the loop condition with
	 * stop still clear, rather than taking the immediate mid-frame break. */
	radio.stopaudiothread = 0;
	radio.hasusb = 1;
	radio.pa.active = 1;
	radio.owner = (struct ast_channel *)(uintptr_t)1;
	radio.rxkeyed = radio.lastrx = 1;
	modern_drop_hasusb_on_queue = &radio;
	modern_stop_audio_on_usleep = &radio;
	assert(usbradio_audio_thread(&radio) == NULL);
	modern_stop_audio_on_usleep = NULL;
	modern_drop_hasusb_on_queue = NULL;

	/* A keyed receiver can disappear without an owner or hardware sidetone. */
	radio.stopaudiothread = 0;
	radio.hasusb = 1;
	radio.pa.active = 1;
	radio.owner = NULL;
	radio.rxkeyed = 1;
	radio.duplex3 = 0;
	modern_radio_time_calls = 0;
	modern_drop_hasusb_on_time = &radio;
	modern_drop_hasusb_on_time_call = 3;
	modern_stop_audio_on_usleep = &radio;
	assert(usbradio_audio_thread(&radio) == NULL);
	modern_stop_audio_on_usleep = NULL;
	modern_drop_hasusb_on_time = NULL;
	radio.owner = NULL;

	radio.stopaudiothread = 0;
	radio.hasusb = 0;
	modern_stop_audio_on_usleep = &radio;
	assert(usbradio_audio_thread(&radio) == NULL);
	modern_stop_audio_on_usleep = NULL;

	radio.stopaudiothread = 0;
	radio.hasusb = 1;
	radio.pa.active = 0;
	modern_open_result = paUnanticipatedHostError;
	modern_stop_audio_on_usleep = &radio;
	assert(usbradio_audio_thread(&radio) == NULL);
	modern_stop_audio_on_usleep = NULL;
	modern_open_result = paNoError;
	usbradioplus_dsp_destroy(&radio);
	urp_radio_destroy(radio.radio);
}

static void run_modern_hid_retry(struct chan_usbradio_pvt *radio, int stop_after)
{
	radio->stophid = 0;
	stop_hid_usleep_count = 0;
	stop_hid_after_usleeps = stop_after;
	stop_hid_radio_on_usleep = radio;
	assert(hidthread(radio) == NULL);
	assert(radio->stophid);
	stop_hid_radio_on_usleep = NULL;
	stop_hid_after_usleeps = 0;
}

static void configure_modern_hid_status_iteration(struct chan_usbradio_pvt *radio,
						  unsigned char *inputs, int call)
{
	unsigned int pp10 = 1U << ppinshift[10];
	unsigned int pp11 = 1U << ppinshift[11];

	assert(radio && call > 0);
	radio->devtype = C108AH_PRODUCT_ID;
	radio->gpios[0] = "in";
	radio->gpios[1] = "in";
	radio->valid_gpios = 3;
	radio->had_gpios_in = 1;
	radio->last_gpios_in = 1;
	inputs[radio->hid_io_cor_loc] &= (unsigned char)~0x10U;
	inputs[radio->hid_gpio_loc] = 3;

	radio->pps[2] = NULL;
	radio->pps[10] = "in";
	radio->pps[11] = "in";
	radio->pps[14] = "in";
	radio->pps[12] = "cor";
	radio->had_pp_in = 1;
	radio->last_pp_in = (int)pp10;
	mock_parallel_inputs = (unsigned char)(0x80U | pp10 | pp11);
	radio->rxppsq = (int)(1U << ppinshift[12]);
	radio->lasttx = 0;
	usbradioplus_publish_hardware_ptt(radio, 1);
}

/** @brief Script two idle HID polls around an unavailable program snapshot. */
static void configure_modern_hid_idle_program_iteration(struct chan_usbradio_pvt *radio,
							unsigned char *inputs, int call)
{
	assert(radio && call > 0);
	assert(radio->hid_io_cor && radio->hid_io_ctcss);
	inputs[radio->hid_io_cor_loc] |= (unsigned char)radio->hid_io_cor;
	inputs[radio->hid_io_ctcss_loc] |= (unsigned char)radio->hid_io_ctcss;
	radio->pps[12] = NULL;
	radio->pps[13] = "ctcss";
	mock_parallel_inputs = 0;
	if (call == 1) {
		/* Hold the sequence odd so the worker has no coherent program request. */
		atomic_store_explicit(&radio->plus_radio_program_generation, 1U,
				      memory_order_release);
		radio->lasttx = 0;
		usbradioplus_publish_hardware_ptt(radio, 1);
	}
}

/** @brief Request PTT only after a valid program snapshot becomes current. */
static void request_modern_hid_stable_program_ptt(struct chan_usbradio_pvt *radio,
						  unsigned char *inputs, int call)
{
	(void)inputs;
	assert(radio && call > 0);
	if (call == 3)
		usbradioplus_publish_hardware_ptt(radio, 1);
}

static void test_modern_hid_worker_retries(void)
{
	struct chan_usbradio_pvt radio = {0};
	struct chan_usbradio_pvt constructed = {0};
	struct ast_radio_device device = {
		.devstr = "usb-retry",
		.usb_device = (libusb_device *)(uintptr_t)1,
		.product_id = C119_PRODUCT_ID,
		.pa_input_channels = 2,
		.pa_output_channels = 2,
	};
	urp_radio_state configuration = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	struct ast_variable explicit_rxvoice = {.name = "rxvoiceadj", .value = "0.75"};

	radio.name = "modern-retry";
	radio.pttkick[0] = radio.pttkick[1] = -1;
	radio.plus_app_rpt_rate = URP_RATE_LINK;
	radio.plus_app_rpt_samples = URP_LINK_SAMPLES;
	radio.plus_deemphasis_corner_hz = 300.0;
	radio.plus_preemphasis_corner_hz = 300.0;
	radio.radio = urp_radio_create(&configuration, URP_LINK_SAMPLES);
	assert(radio.radio);
	modern_acquire_device = &device;
	modern_mixer_max = 100;
	modern_open_result = paNoError;
	modern_start_result = paNoError;

	constructed.name = "modern-create-failure";
	constructed.pttkick[0] = constructed.pttkick[1] = -1;
	constructed.plus_app_rpt_rate = URP_RATE_LINK;
	constructed.plus_app_rpt_samples = URP_LINK_SAMPLES;
	constructed.plus_deemphasis_corner_hz = 300.0;
	constructed.plus_preemphasis_corner_hz = 300.0;
	fail_radio_state_allocation = 1;
	run_modern_hid_retry(&constructed, 1);
	fail_radio_state_allocation = 0;

	for (int mode = 0; mode < 3; ++mode) {
		struct ast_variable ignored_option = {.name = "unsupported", .value = "4999"};
		constructed.plus_advanced = mode == 1;
		constructed.radioduplex = mode == 2;
		constructed.name = "modern-tone-route";
		constructed.stophid = 0;
		constructed.txmixa = TX_OUT_VOICE;
		constructed.txmixb = TX_OUT_LSD;
		strcpy(constructed.txctcssfreq, "100.0");
		strcpy(constructed.txctcssdefault, "100.0");
		test_config_category = constructed.name;
		test_config_variables = &ignored_option;
		test_config_load_result = (struct ast_config *)(uintptr_t)1;
		modern_stop_hid_target = &constructed;
		assert(hidthread(&constructed) == NULL);
		modern_stop_hid_target = NULL;
		test_config_category = NULL;
		test_config_variables = NULL;
		test_config_load_result = NULL;
		assert(constructed.radio);
		assert(constructed.radio->radioDuplex == (mode != 0));
		urp_radio_destroy(constructed.radio);
		constructed.radio = NULL;
		if (constructed.pttkick[0] >= 0)
			close(constructed.pttkick[0]);
		if (constructed.pttkick[1] >= 0)
			close(constructed.pttkick[1]);
		constructed.pttkick[0] = constructed.pttkick[1] = -1;
	}

	radio.lastaudiotime = 1;
	radio.stophid = 0;
	modern_stop_hid_on_radio_time = &radio;
	assert(hidthread(&radio) == NULL);
	modern_stop_hid_on_radio_time = NULL;
	radio.lastaudiotime = 0;
	radio.stophid = 0;
	radio.wanteeprom = 1;
	modern_eeprom_result = 0;
	modern_eeprom_valid_magic = 0;
	modern_stop_hid_target = &radio;
	assert(hidthread(&radio) == NULL);
	modern_stop_hid_target = NULL;
	modern_eeprom_result = 1;
	radio.wanteeprom = 0;

	radio.stophid = 0;
	radio.gpios[0] = "in";
	radio.valid_gpios = 1;
	modern_hid_input_calls = 0;
	modern_stop_hid_after_inputs = 2;
	modern_toggle_hid_inputs = 1;
	mock_poll_enabled = 1;
	mock_poll_result = 0;
	modern_stop_hid_target = &radio;
	assert(hidthread(&radio) == NULL);
	modern_stop_hid_target = NULL;
	modern_stop_hid_after_inputs = 1;
	modern_toggle_hid_inputs = 0;

	/* Configure status state after attachment so radio_config() cannot replace
	 * the transition setup used by this worker-level test. */
	radio.stophid = 0;
	radio.lastaudiotime = 1234;
	modern_hid_input_calls = 0;
	modern_stop_hid_target = &radio;
	modern_hid_input_hook = configure_modern_hid_status_iteration;
	haspp = 2;
	assert(hidthread(&radio) == NULL);
	modern_hid_input_hook = NULL;
	modern_stop_hid_target = NULL;
	radio.lastaudiotime = 0;
	mock_poll_enabled = 0;
	memset(modern_hid_inputs, 0, sizeof(modern_hid_inputs));

	/* Idle COR/CTCSS, a missing parallel CTCSS input, and a torn programming
	 * publication must leave the second hardware pass as a no-op. */
	radio.stophid = 0;
	radio.lastaudiotime = 0;
	modern_hid_input_calls = 0;
	modern_stop_hid_after_inputs = 2;
	modern_stop_hid_target = &radio;
	modern_hid_input_hook = configure_modern_hid_idle_program_iteration;
	radio.hdwtype = 0;
	assert(hidhdwconfig(&radio) == 0);
	haspp = 2;
	mock_poll_enabled = 1;
	mock_poll_result = 0;
	memset(modern_hid_inputs, 0, sizeof(modern_hid_inputs));
	assert(hidthread(&radio) == NULL);
	assert(modern_hid_input_calls == 2);
	assert(!(atomic_load_explicit(&radio.plus_hardware_inputs, memory_order_acquire) &
		 URP_HARDWARE_INPUT_HID_CARRIER));
	assert(!(atomic_load_explicit(&radio.plus_hardware_inputs, memory_order_acquire) &
		 URP_HARDWARE_INPUT_HID_CTCSS));
	modern_hid_input_hook = NULL;
	modern_stop_hid_target = NULL;
	modern_stop_hid_after_inputs = 1;
	mock_poll_enabled = 0;
	mock_parallel_inputs = 0;
	haspp = 0;
	atomic_store_explicit(&radio.plus_radio_program_generation, 0U, memory_order_release);
	usbradioplus_publish_hardware_ptt(&radio, 0);

	/* A stable program snapshot must distinguish an unchanged PTT request from
	 * a later PTT transition, which reapplies the current radio request. */
	radio.stophid = 0;
	modern_hid_input_calls = 0;
	modern_stop_hid_after_inputs = 3;
	modern_stop_hid_target = &radio;
	modern_hid_input_hook = request_modern_hid_stable_program_ptt;
	mock_poll_enabled = 1;
	mock_poll_result = 0;
	radio.rxfreq = 146520000U;
	radio.txfreq = 146940000U;
	usbradioplus_program_radio(&radio);
	usbradioplus_publish_hardware_ptt(&radio, 0);
	assert(hidthread(&radio) == NULL);
	assert(modern_hid_input_calls == 3);
	assert(atomic_load_explicit(&radio.plus_hardware_ptt_request, memory_order_acquire));
	modern_hid_input_hook = NULL;
	modern_stop_hid_target = NULL;
	modern_stop_hid_after_inputs = 1;
	mock_poll_enabled = 0;
	usbradioplus_publish_hardware_ptt(&radio, 0);

	radio.swap_state = DEVICE_SWAP_QUIESCING;
	radio.swap_audio_ready = 0;
	modern_swap_first = &radio;
	modern_swap_second = &radio;
	modern_swap_wait_step = 0;
	run_modern_hid_retry(&radio, 2);
	modern_swap_first = NULL;
	modern_swap_second = NULL;

	modern_acquire_result = AST_RADIO_DEVICE_WAIT;
	run_modern_hid_retry(&radio, 2);
	modern_acquire_result = AST_RADIO_DEVICE_READY;
	modern_libusb_open_result = -1;
	run_modern_hid_retry(&radio, 2);
	modern_libusb_open_result = 0;
	modern_libusb_claim_result = -1;
	modern_libusb_detach_result = -1;
	run_modern_hid_retry(&radio, 2);
	modern_libusb_detach_result = 0;
	modern_libusb_claim_calls = 0;
	modern_libusb_claim_first_result = -1;
	modern_libusb_claim_result = 0;
	radio.stophid = 0;
	modern_hid_input_calls = 0;
	modern_stop_hid_target = &radio;
	assert(hidthread(&radio) == NULL);
	modern_stop_hid_target = NULL;
	modern_libusb_claim_first_result = 0;
	modern_libusb_claim_result = -1;
	run_modern_hid_retry(&radio, 1);
	modern_libusb_claim_result = 0;
	assert(pipe(radio.pttkick) == 0);
	mock_pipe_failure = 1;
	run_modern_hid_retry(&radio, 1);
	assert(radio.pttkick[0] == -1 && radio.pttkick[1] == -1);
	mock_pipe_failure = 0;
	ast_radio_pa_stop(&radio.pa);
	modern_open_result = paUnanticipatedHostError;
	run_modern_hid_retry(&radio, 1);
	modern_open_result = paNoError;

	mock_poll_enabled = 1;
	mock_poll_result = -1;
	run_modern_hid_retry(&radio, 1);
	mock_poll_result = 1;
	mock_poll_revents = POLLIN;
	mock_oss_io = 1;
	mock_read_result = -1;
	mock_read_errno = EIO;
	radio.stophid = 0;
	modern_stop_hid_target = &radio;
	assert(hidthread(&radio) == NULL);
	modern_stop_hid_target = NULL;
	/* A closed wake pipe is benign and must not be reported as an I/O error. */
	mock_read_result = 0;
	radio.stophid = 0;
	modern_stop_hid_target = &radio;
	assert(hidthread(&radio) == NULL);
	modern_stop_hid_target = NULL;
	mock_read_errno = EAGAIN;
	mock_oss_io = 0;
	mock_poll_revents = 0;
	mock_poll_result = 0;
	mock_poll_enabled = 0;

	/* Exercise healthy watchdog and successful kick reads, then EEPROM loading
	 * when an explicit receive-level setting must win over the stored value. */
	radio.stophid = 0;
	radio.lastaudiotime = 1234;
	radio.wanteeprom = 1;
	radio.eepromctl = 1;
	test_config_load_result = (struct ast_config *)(uintptr_t)1;
	test_config_category = radio.name;
	test_config_variables = &explicit_rxvoice;
	modern_eeprom_result = 0;
	modern_eeprom_valid_magic = 1;
	modern_eeprom_write_after_read = &radio;
	modern_hid_input_calls = 0;
	modern_stop_hid_after_inputs = 2;
	modern_stop_hid_target = &radio;
	mock_poll_enabled = 1;
	mock_poll_result = 1;
	mock_poll_revents = POLLIN;
	mock_oss_io = 1;
	mock_read_result = 1;
	assert(hidthread(&radio) == NULL);
	modern_stop_hid_target = NULL;
	modern_stop_hid_after_inputs = 1;
	test_config_load_result = NULL;
	test_config_category = NULL;
	test_config_variables = NULL;
	modern_eeprom_write_after_read = NULL;
	mock_oss_io = 0;
	mock_poll_revents = 0;
	mock_poll_result = 0;
	mock_poll_enabled = 0;
	radio.lastaudiotime = 0;
	radio.wanteeprom = 0;
	modern_eeprom_result = 1;
	modern_eeprom_valid_magic = 0;

	/* Dropping only the acquired interface exercises the inner-loop USB-loss
	 * condition; the outer retry sleep then requests orderly worker shutdown. */
	radio.stophid = 0;
	modern_hid_input_calls = 0;
	modern_drop_hid_after_inputs = 1;
	modern_stop_hid_target = &radio;
	stop_hid_radio_on_usleep = &radio;
	assert(hidthread(&radio) == NULL);
	stop_hid_radio_on_usleep = NULL;
	modern_stop_hid_target = NULL;
	modern_drop_hid_after_inputs = 0;
	modern_acquire_result = AST_RADIO_DEVICE_READY;

	/* Two service intervals cover stable and changing HID/parallel inputs,
	 * CM108AH hook deassertion, an expired pulse, and no parallel PTT route. */
	radio.stophid = 0;
	radio.devtype = C108AH_PRODUCT_ID;
	radio.gpios[0] = "in";
	radio.gpios[1] = "in";
	radio.valid_gpios = 3;
	radio.had_gpios_in = 1;
	radio.last_gpios_in = 0;
	radio.pps[2] = NULL;
	radio.pps[10] = "in";
	radio.pps[11] = "cor";
	radio.had_pp_in = 1;
	radio.last_pp_in = 0;
	radio.rxppsq = 0;
	radio.hid_gpio_pulsetimer[0] = 1;
	radio.hid_gpio_pulsemask = 1;
	memset(modern_hid_inputs, 0, sizeof(modern_hid_inputs));
	modern_hid_inputs[radio.hid_io_cor_loc] = 0;
	modern_hid_input_calls = 0;
	modern_stop_hid_after_inputs = 2;
	modern_toggle_hid_inputs = 1;
	mock_parallel_inputs = 0x80;
	toggle_parallel_inputs_mask = 0x40;
	mock_tvnow_step = 10;
	modern_stop_hid_target = &radio;
	mock_poll_enabled = 1;
	mock_poll_result = 0;
	assert(hidthread(&radio) == NULL);
	modern_stop_hid_target = NULL;
	modern_stop_hid_after_inputs = 1;
	modern_toggle_hid_inputs = 0;
	mock_poll_enabled = 0;
	mock_tvnow_step = 0;
	toggle_parallel_inputs_mask = 0;
	haspp = 0;

	if (radio.pttkick[0] >= 0)
		close(radio.pttkick[0]);
	if (radio.pttkick[1] >= 0)
		close(radio.pttkick[1]);
	urp_radio_destroy(radio.radio);
}

static void test_modern_module_lifecycle_baseline(void)
{
	struct chan_usbradio_pvt no_radio = {0};
	struct ast_variable active = {.name = "channel_enabled", .value = "yes"};

	haspp = hasout = 0;
	usbradio_start_parallel_pulser();
	haspp = hasout = 1;
	usbradio_start_parallel_pulser();
	haspp = hasout = 0;

	fail_format_cap_alloc = 1;
	assert(load_module() == AST_MODULE_LOAD_DECLINE);
	fail_format_cap_alloc = 0;
	test_config_load_result = CONFIG_STATUS_FILEMISSING;
	assert(load_module() == AST_MODULE_LOAD_FAILURE);
	separate_processing_config_result = 1;
	test_processing_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(load_module() == AST_MODULE_LOAD_FAILURE);
	test_config_load_result = (struct ast_config *)(uintptr_t)1;
	test_processing_config_load_result = test_config_load_result;
	test_config_category = "general";
	test_config_variables = &active;
	assert(load_module() == AST_MODULE_LOAD_FAILURE);
	test_config_category = "modern-test";
	separate_processing_config_result = 0;
	test_config_load_calls = 0;
	test_config_load_second_result = CONFIG_STATUS_FILEINVALID;
	assert(load_module() == AST_MODULE_LOAD_DECLINE);
	test_config_load_second_result = NULL;
	separate_processing_config_result = 1;
	ast_strdup_calls = 0;
	fail_ast_strdup_call = 1;
	assert(load_module() == AST_MODULE_LOAD_DECLINE);
	fail_ast_strdup_call = 0;
	channel_register_result = 1;
	assert(load_module() == AST_MODULE_LOAD_FAILURE);
	channel_register_result = 0;
	cli_register_result = 1;
	assert(load_module() == AST_MODULE_LOAD_FAILURE);
	cli_register_result = 0;
	advanced_register_result = -1;
	assert(load_module() == AST_MODULE_LOAD_FAILURE);
	advanced_register_result = 0;
	assert(load_module() == AST_MODULE_LOAD_SUCCESS);
	{
		struct chan_usbradio_pvt *radio = usbradio_default.next;
		assert(radio);
		radio->owner = (struct ast_channel *)(uintptr_t)1;
		radio->audiothread = (pthread_t)1;
		radio->hidthread = (pthread_t)2;
		radio->dsp = (struct ast_dsp *)(uintptr_t)1;
		radio->gpios[0] = ast_strdup("in");
		radio->pps[2] = ast_strdup("ptt");
		assert(radio->gpios[0] && radio->pps[2]);
		no_radio.pttkick[0] = no_radio.pttkick[1] = -1;
		no_radio.hidthread = no_radio.audiothread = AST_PTHREADT_NULL;
		radio->next = &no_radio;
	}
	assert(unload_module() == 0);
	haspp = 0;
	hasout = 0;
	usbradio_default.next = NULL;
	test_config_category = NULL;
	test_config_variables = NULL;
	test_config_load_result = NULL;
	separate_processing_config_result = 0;
}
#endif

#ifdef URP_TEST_MODERN
int ast_radio_check_audio(short *samples, struct audiostatistics *statistics, short count,
			  short mono)
#else
/** @brief Host-API test double for ast_radio_check_audio; observable effects are recorded in
 * harness state.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param statistics Audio measurement structure updated or displayed by the stub.
 * @param count Number of elements available in the supplied block.
 * @return Scripted host result for the current test scenario.
 */
int ast_radio_check_audio(short *samples, struct audiostatistics *statistics, short count)
#endif
{
	(void)samples;
	(void)statistics;
	(void)count;
#ifdef URP_TEST_MODERN
	(void)mono;
#endif
	return mock_audio_clipping;
}

static void add_processing_override(const char *section, const char *name, const char *value);

/** @brief Verify clean-slate CTCSS and DCS defaults are immediately usable. */
static void test_clean_slate_signaling_defaults(void)
{
	struct chan_usbradio_pvt radio = {0};

	assert(fabs(usbradio_default.plus_deemphasis_corner_hz - 300.0) < 0.001);
	assert(fabs(usbradio_default.plus_preemphasis_corner_hz - 300.0) < 0.001);
	assert(fabs(usbradio_default.rxctcssadj - 1.0F) < 0.001F);
	assert(fabs(urp_pcm_peak_dbfs((unsigned int)lround(usbradio_default.ctcss_level)) + 24.0) <
	       0.1);
	assert(fabs(urp_pcm_peak_dbfs((unsigned int)usbradio_default.dcs_level) + 24.0) < 0.1);
	assert(fabs(usbradio_default.ctcss_phase_shift_degrees - 120.0) < 0.001);
	assert(usbradio_default.ctcss_tail_duration_ms == 180);
	assert(fabs(usbradio_default.ctcss_tail_frequency_hz - 55.0) < 0.001);

	/* Omitted clean-slate settings must resolve exactly like the shipped sample. */
	settings_defaults(&settings);
	assert(!apply_processing_config_overrides(&radio, "usb"));
	assert(!strcmp(radio.receive_signaling_method, "carrier"));
	assert(!strcmp(radio.transmit_signaling_method, "carrier"));
	assert(radio.rxdemod == RX_AUDIO_FLAT && radio.rxcdtype == CD_XPMR_NOISE);
	assert(!radio.rxcpusaver && !radio.txcpusaver && radio.rxsquelchadj == 500);
	assert(radio.voxhangtime == 2000 && radio.rxsqhyst == 3000);
	assert(!radio.rxsquelchdelay && !radio.rxondelay && radio.txpreemphasis);
	assert(radio.txsettletime == 500 && !radio.txrxblankingtime && !radio.txoffdelay);
	assert(radio.rxsdtype == SD_IGNORE && !strcmp(radio.rxctcssfreqs, "100.0") &&
	       !strcmp(radio.txctcssfreqs, "100.0"));
	assert(!strcmp(radio.dcs_receive_code, "023N") && !strcmp(radio.dcs_transmit_code, "023N"));
	assert(fabs(radio.rxctcssadj - 1.0) < 0.001);
	assert(fabs(urp_pcm_peak_dbfs((unsigned int)lround(radio.ctcss_level)) + 24.0) < 0.1);
	assert(radio.txtoctype == TOC_PHASE && radio.ctcss_tail_duration_ms == 180);
	assert(radio.dcs_turnoff_enabled && radio.dcs_turnoff_duration_ms == 180);
	assert(fabs(urp_pcm_peak_dbfs((unsigned int)radio.dcs_level) + 24.0) < 0.1);

	/* CTCSS uses the same direct PCM-peak conversion as DCS; it does not pass
	 * through the retired normalized tone calibration. */
	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("ctcss", "transmit_peak_dbfs", "-12.0");
	assert(!apply_processing_config_overrides(&radio, "usb"));
	assert(fabs(radio.ctcss_level - 32767.0 * pow(10.0, -12.0 / 20.0)) < 0.001);

	/* The omitted protocol defaults must also satisfy a later method selection. */
	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("receive", "signaling_method", "ctcss");
	add_processing_override("transmit", "signaling_method", "ctcss");
	assert(!apply_processing_config_overrides(&radio, "usb"));
	assert(radio.rxsdtype == SD_XPMR && !strcmp(radio.rxctcssfreqs, "100.0"));
	assert(!strcmp(radio.txctcssfreqs, "100.0"));
	assert(!strcmp(radio.txctcssdefault, "100.0"));

	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("receive", "signaling_method", "dcs");
	add_processing_override("transmit", "signaling_method", "dcs");
	assert(!apply_processing_config_overrides(&radio, "usb"));
	assert(!strcmp(radio.dcs_receive_code, "023N"));
	assert(!strcmp(radio.dcs_transmit_code, "023N"));
}

/** @brief Verify option decoders. */
static void test_option_decoders(void)
{
	struct chan_usbradio_pvt radio = {0};

	store_rxdemod(&radio, "no");
	assert(radio.rxdemod == RX_AUDIO_NONE);
	store_rxdemod(&radio, "speaker");
	assert(radio.rxdemod == RX_AUDIO_SPEAKER);
	store_rxdemod(&radio, "flat");
	assert(radio.rxdemod == RX_AUDIO_FLAT);

	store_rxsdtype(&radio, "no");
	assert(radio.rxsdtype == SD_IGNORE);
	store_rxsdtype(&radio, "usb");
	assert(radio.rxsdtype == SD_HID);
	store_rxsdtype(&radio, "usbinvert");
	assert(radio.rxsdtype == SD_HID_INVERT);
	store_rxsdtype(&radio, "dsp");
	assert(radio.rxsdtype == SD_XPMR);
	store_rxsdtype(&radio, "pp");
	assert(radio.rxsdtype == SD_PP);
	store_rxsdtype(&radio, "ppinvert");
	assert(radio.rxsdtype == SD_PP_INVERT);

	radio.rxdemod = RX_AUDIO_FLAT;
	store_rxdemod(&radio, "invalid");
	assert(radio.rxdemod == RX_AUDIO_FLAT);
	radio.rxsdtype = SD_HID;
	store_rxsdtype(&radio, "invalid");
	assert(radio.rxsdtype == SD_HID);
	store_txtoctype(&radio, "no");
	assert(radio.txtoctype == TOC_NONE);
	store_txtoctype(&radio, "ctcss_phase_shift");
	assert(radio.txtoctype == TOC_PHASE);
	store_txtoctype(&radio, "ctcss_tone_remove");
	assert(radio.txtoctype == TOC_NOTONE);
	store_txtoctype(&radio, "ctcss_tail_tone");
	assert(radio.txtoctype == 3);
	store_txtoctype(&radio, "invalid");
	assert(radio.txtoctype == 3);
}

/** @brief Verify channel callbacks. */
static void test_channel_callbacks(void)
{
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state radio_state = {0};
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	char option;

	radio.name = "test";
	radio.radio = &radio_state;
	radio.pttkick[1] = -1;
	strcpy(radio.txctcssdefault, "100.0");
	test_channel_private = &radio;
	assert(usbradio_digit_begin(channel, '1') == 0);
	assert(usbradio_digit_end(channel, '2', 100) == 0);
	setstate_calls = 0;
	assert(usbradio_answer(channel) == 0 && setstate_calls == 1);
	assert(usbradio_fixup(NULL, channel) == 0 && radio.owner == channel);

	assert(usbradio_indicate(channel, AST_CONTROL_BUSY, NULL, 0) == 0);
	assert(usbradio_indicate(channel, AST_CONTROL_CONGESTION, NULL, 0) == 0);
	assert(usbradio_indicate(channel, AST_CONTROL_RINGING, NULL, 0) == 0);
	assert(usbradio_indicate(channel, AST_CONTROL_VIDUPDATE, NULL, 0) == 0);
	moh_start_calls = moh_stop_calls = 0;
	assert(usbradio_indicate(channel, AST_CONTROL_HOLD, "default", 7) == 0);
	assert(moh_start_calls == 1);
	assert(usbradio_indicate(channel, AST_CONTROL_UNHOLD, NULL, 0) == 0);
	assert(usbradio_indicate(channel, AST_CONTROL_PROCEEDING, NULL, 0) == 0);
	assert(usbradio_indicate(channel, AST_CONTROL_PROGRESS, NULL, 0) == 0);
	assert(moh_stop_calls == 3);
	option_debug = 0;
	tx_trace_message[0] = '\0';
	assert(usbradio_indicate(channel, AST_CONTROL_RADIO_KEY, "0", 0) == 0);
	assert(!tx_trace_message[0]);
	option_debug = 5;
	assert(usbradio_indicate(channel, AST_CONTROL_RADIO_KEY, "0", 0) == 0);
	assert(strstr(tx_trace_message, "event=request key=1"));
	assert(radio.txkeyed && !radio.forcetxcode);
	assert(usbradio_indicate(channel, AST_CONTROL_RADIO_KEY, "0", 1) == 0);
	assert(usbradio_indicate(channel, AST_CONTROL_RADIO_UNKEY, NULL, 0) == 0);
	assert(strstr(tx_trace_message, "event=request key=0"));
	option_debug = 0;
	tx_trace_message[0] = '\0';
	radio.forcetxcode = 1;
	assert(usbradio_indicate(channel, AST_CONTROL_RADIO_UNKEY, NULL, 0) == 0);
	assert(!tx_trace_message[0]);
	assert(!radio.txkeyed && !radio.forcetxcode);
	assert(radio_state.pTxCodeDefault == radio.txctcssdefault);
	assert(usbradio_indicate(channel, -1234, NULL, 0) == -1);

	assert(usbradio_setoption(channel, AST_OPTION_TONE_VERIFY, NULL, 1) == -1);
	assert(usbradio_setoption(channel, AST_OPTION_TONE_VERIFY, &option, 0) == -1);
	option = 1;
	assert(usbradio_setoption(channel, AST_OPTION_TONE_VERIFY, &option, 1) == 0);
	assert(radio.usedtmf);
	option = 2;
	assert(usbradio_setoption(channel, AST_OPTION_TONE_VERIFY, &option, 1) == 0);
	assert(radio.usedtmf);
	option = 3;
	assert(usbradio_setoption(channel, AST_OPTION_TONE_VERIFY, &option, 1) == 0);
	assert(!radio.usedtmf);
	option = 99;
	assert(usbradio_setoption(channel, AST_OPTION_TONE_VERIFY, &option, 1) == 0);
	assert(radio.usedtmf);
	assert(usbradio_setoption(channel, -1, &option, 1) == 0);
	assert(errno == 0);
}

/** @brief Verify text controls. */
static void test_text_controls(void)
{
	struct chan_usbradio_pvt radio = {0};
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	urp_radio_state template = {0};

	test_channel_private = NULL;
	assert(usbradio_text(channel, "") == -1);
	radio.name = "test";
	radio.pttkick[1] = -1;
	radio.valid_gpios = (1 << 0) | (1 << 2);
	template.pRxCodeSrc = "100.0";
	template.pTxCodeSrc = "100.0";
	template.pTxCodeDefault = "100.0";
	radio.radio = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(radio.radio);
	test_channel_private = &radio;
	assert(usbradio_text(channel, "") == 0);
	assert(usbradio_text(channel, "SETCHAN 7") == 0);
	assert(usbradio_text(channel, "RXCTCSS 0") == 0 && radio.rxctcssoverride);
	assert(usbradio_text(channel, "RXCTCSS 1") == 0 && !radio.rxctcssoverride);
	assert(usbradio_text(channel, "TXCTCSS") == 0);
	assert(usbradio_text(channel, "TXCTCSS 2") == 0);
	assert(usbradio_text(channel, "TXCTCSS 0") == 0 && radio.radio->b.txCtcssOff);
	assert(usbradio_text(channel, "TXCTCSS 1") == 0 && !radio.radio->b.txCtcssOff);
	{
		urp_radio_state *saved_radio = radio.radio;
		radio.radio = NULL;
		assert(usbradio_text(channel, "TXCTCSS 1") == 0);
		radio.radio = saved_radio;
	}
	assert(usbradio_text(channel, "GPIO") == 0);
	assert(usbradio_text(channel, "GPIO 0 1") == 0);
	assert(usbradio_text(channel, "GPIO 9 1") == 0);
	assert(usbradio_text(channel, "GPIO 2 1") == 0);
	assert(usbradio_text(channel, "GPIO 1 3") == 0);
	assert(radio.hid_gpio_pulsetimer[0] == 2);
	assert(usbradio_text(channel, "GPIO 1 0") == 0);
	assert(usbradio_text(channel, "GPIO 1 1") == 0);
	assert(radio.hid_gpio_val & 1);
	haspp = 2;
	assert(usbradio_text(channel, "PP") == 0);
	assert(usbradio_text(channel, "PP 1 1") == 0);
	assert(usbradio_text(channel, "PP 10 1") == 0);
	assert(usbradio_text(channel, "PP 2 3") == 0);
	assert(pp_pulsetimer[2] == 2);
	assert(usbradio_text(channel, "PP 2 0") == 0);
	assert(usbradio_text(channel, "PP 2 1") == 0);
	assert(pp_val & 1);
	haspp = 0;
	assert(usbradio_text(channel, "UNKNOWN") == 0);
	assert(usbradio_text(channel, "UNKNOWN 1 2 3 4 H") == 0);
	settings_defaults(&settings);
	assert(usbradio_text(channel, "SETFREQ 146.520 146.940 100.0 123.0 H") == 0);
	assert(radio.remoted && radio.set_rxctcssfreqs[0] && radio.set_txctcssfreqs[0]);
	assert(radio.set_txpower);
	assert(!urp_radio_destroy(radio.radio));
	test_channel_private = NULL;
}

/** @brief Verify console keying. */
static void test_console_keying(void)
{
	struct chan_usbradio_pvt radio = {0};
	const char *arguments[] = {"radio", "key"};

	radio.name = "test";
	radio.pttkick[1] = -1;
	usbradio_default.next = &radio;
	usbradio_active = radio.name;
	assert(find_desc("test") == &radio);
	assert(find_desc("missing") == NULL);
	assert(find_desc(NULL) == NULL);
	assert(console_key(0, 1, arguments) == RESULT_SHOWUSAGE);
	assert(console_key(0, 2, arguments) == RESULT_SUCCESS);
	assert(radio.txtestkey);
	assert(console_unkey(0, 1, arguments) == RESULT_SHOWUSAGE);
	assert(console_unkey(0, 2, arguments) == RESULT_SUCCESS);
	assert(!radio.txtestkey);
	usbradio_default.next = NULL;
	usbradio_active = NULL;
}

/** @brief Verify channel selection helpers. */
static void test_channel_selection_helpers(void)
{
	struct chan_usbradio_pvt first = {.name = "first"};
	struct chan_usbradio_pvt second = {.name = "second"};
#ifdef URP_TEST_MODERN
	struct ast_radio_device first_device = {.devstr = "lease-first", .serial = "serial-first"};
	struct ast_radio_device second_device = {.devstr = "lease-second",
						 .serial = "serial-second"};
#endif
	const char *show_active[] = {"radio", "active"};
	const char *invalid[] = {"radio", "active", "first", "extra"};
	const char *missing[] = {"radio", "active", "missing"};
	const char *select_second[] = {"radio", "active", "second"};
	const char *show_devices[] = {"radio", "active", "show"};

	strcpy(first.devstr, "usb-first");
	strcpy(second.devstr, "usb-second");
	first.next = &second;
#ifdef URP_TEST_MODERN
	first.radio_device = &first_device;
#endif
	usbradio_default.next = &first;
	usbradio_active = first.name;
	assert(radio_active(1, 2, show_active) == RESULT_SUCCESS);
	assert(radio_active(1, 4, invalid) == RESULT_SHOWUSAGE);
	assert(radio_active(1, 3, show_devices) == RESULT_SUCCESS);
	assert(radio_active(1, 3, missing) == RESULT_SUCCESS);
	assert(radio_active(1, 3, select_second) == RESULT_SUCCESS);
	assert(usbradio_active == second.name && second.radioactive && !first.radioactive);
	usbradio_active = "missing";
	assert(usb_device_swap(1, "second") == -1);
	usbradio_active = first.name;
	assert(usb_device_swap(1, NULL) == -1);
	assert(usb_device_swap(1, "missing") == -1);
	assert(usb_device_swap(1, "first") == -1);
#ifndef URP_TEST_MODERN
	first.devicenum = 1;
	second.devicenum = 2;
	first.hasusb = first.usbass = second.hasusb = second.usbass = 1;
	assert(usb_device_swap(1, "second") == 0);
	assert(first.devicenum == 2 && second.devicenum == 1);
	assert(!first.hasusb && !first.usbass && !second.hasusb && !second.usbass);
	installed_usb_device = NULL;
	assert(find_installed_usb_match() == NULL);
	installed_usb_device = "usb-second";
	assert(find_installed_usb_match() == first.devstr);
	installed_usb_device = NULL;
#else
	{
		first.hasusb = 1;
		second.hasusb = 0;
		assert(usb_device_swap(1, "second") == -1);
		first.hasusb = 0;
		second.hasusb = 1;
		assert(usb_device_swap(1, "second") == -1);

		first.hasusb = second.hasusb = 1;
		first.radio_device = &first_device;
		second.radio_device = &second_device;
		modern_swap_first = NULL;
		modern_swap_second = NULL;
		assert(usb_device_swap(1, "second") == -1);

		first.hasusb = second.hasusb = 1;
		modern_swap_first = &first;
		modern_swap_second = &second;
		modern_swap_wait_step = 0;
		modern_swap_staggered = 1;
		modern_device_swap_result = 0;
		assert(usb_device_swap(1, "second") == 0);
		modern_swap_staggered = 0;
		assert(first.radio_device == &second_device);
		assert(second.radio_device == &first_device);
		assert(!strcmp(first.devstr, "lease-second"));
		assert(!strcmp(second.devstr, "lease-first"));

		first.hasusb = second.hasusb = 1;
		first.radio_device->serial = NULL;
		second.radio_device->serial = NULL;
		modern_swap_wait_step = 0;
		assert(usb_device_swap(1, "second") == 0);

		first.hasusb = second.hasusb = 1;
		modern_swap_wait_step = 0;
		modern_device_swap_result = -1;
		assert(usb_device_swap(1, "second") == -1);
		modern_device_swap_result = 0;
		modern_swap_first = NULL;
		modern_swap_second = NULL;
	}
#endif
	usbradio_default.next = NULL;
	usbradio_active = NULL;
}

/** @brief Verify cli handlers. */
static void test_cli_handlers(void)
{
	struct ast_cli_entry entry = {0};
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state template = {0};
	const char *key_args[] = {"radioplus", "key"};
	const char *tune_args[] = {"radioplus", "tune", "rxsquelch"};
	const char *stats_args[] = {"radioplus", "native", "stats", "reset"};
	struct ast_cli_args args2 = {.fd = 1, .argc = 2, .argv = key_args};
	struct ast_cli_args args3 = {.fd = 1, .argc = 3, .argv = tune_args};
	struct ast_cli_args stats3 = {.fd = 1, .argc = 3, .argv = stats_args};
	struct ast_cli_args stats4 = {.fd = 1, .argc = 4, .argv = stats_args};
	struct ast_cli_args *args = &args2;

	assert(res2cli(RESULT_SUCCESS) == CLI_SUCCESS);
	assert(res2cli(RESULT_SHOWUSAGE) == CLI_SHOWUSAGE);
	assert(res2cli(-999) == CLI_FAILURE);
	radio.name = "test";
	radio.pttkick[1] = -1;
	radio.plus_app_rpt_rate = URP_RATE_LINK;
	radio.plus_app_rpt_samples = URP_LINK_SAMPLES;
	radio.plus_deemphasis_corner_hz = 300.0;
	radio.plus_preemphasis_corner_hz = 300.0;
	settings_defaults(&settings);
	ast_copy_string(settings.profiles[0].name, radio.name, sizeof(settings.profiles[0].name));
	ast_copy_string(settings.profiles[0].channel, "RadioPlus/test",
			sizeof(settings.profiles[0].channel));
	template.pRxCodeSrc = "100.0";
	template.pTxCodeSrc = "100.0";
	template.pTxCodeDefault = "100.0";
	radio.radio = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(radio.radio);
	assert(!usbradioplus_dsp_init(&radio));
	usbradio_default.next = &radio;
	usbradio_active = radio.name;

#define EXERCISE_HANDLER(handler)                                                                  \
	do {                                                                                       \
		assert((handler)(&entry, CLI_INIT, args) == NULL);                                 \
		assert((handler)(&entry, CLI_GENERATE, args) == NULL);                             \
	} while (0)
	EXERCISE_HANDLER(handle_console_key);
	assert(handle_console_key(&entry, 0, args) == CLI_SUCCESS);
	EXERCISE_HANDLER(handle_console_unkey);
	assert(handle_console_unkey(&entry, 0, args) == CLI_SUCCESS);
	args = &args3;
	EXERCISE_HANDLER(handle_radio_tune);
	assert(handle_radio_tune(&entry, 0, args) == CLI_SUCCESS);
	args = &args2;
	EXERCISE_HANDLER(handle_radio_active);
	assert(handle_radio_active(&entry, 0, args) == CLI_SUCCESS);
	EXERCISE_HANDLER(handle_show_settings);
	assert(handle_show_settings(&entry, 0, args) == CLI_SUCCESS);
	usbradio_active = "missing";
	assert(handle_show_settings(&entry, 0, args) == CLI_SUCCESS);
	usbradio_active = radio.name;
	EXERCISE_HANDLER(handle_set_dsp_debug);
	assert(handle_set_dsp_debug(&entry, 0, args) == CLI_SUCCESS);
	EXERCISE_HANDLER(handle_radioplus_native_stats);
	assert(handle_radioplus_native_stats(&entry, 0, &args2) == CLI_SHOWUSAGE);
	radio.plus_app_rpt_samples = 0;
	assert(handle_radioplus_native_stats(&entry, 0, &stats3) == CLI_SUCCESS);
	radio.plus_app_rpt_samples = URP_LINK_SAMPLES;
	assert(handle_radioplus_native_stats(&entry, 0, &stats3) == CLI_SUCCESS);
	assert(handle_radioplus_native_stats(&entry, 0, &stats4) == CLI_SUCCESS);
	stats_args[3] = "invalid";
	assert(handle_radioplus_native_stats(&entry, 0, &stats4) == CLI_SHOWUSAGE);
	/* The statistics command distinguishes an unavailable graph from an
	 * unavailable renderer. Both are possible during a control-plane reload and
	 * must report a normal CLI failure rather than dereferencing partial state. */
	{
		struct usbradioplus_native_graph_set *graphs =
			usbradioplus_native_graphs_acquire(&radio);
		int local_configured;
		int final_configured;
		struct usbradioplus_native_renderer *renderer;

		assert(graphs);
		local_configured = graphs->local_dynamics.configured;
		graphs->local_dynamics.configured = 0;
		assert(handle_radioplus_native_stats(&entry, 0, &stats3) == CLI_FAILURE);
		graphs->local_dynamics.configured = local_configured;
		usbradioplus_native_graphs_release(&radio);

		graphs = usbradioplus_native_graphs_acquire(&radio);
		assert(graphs);
		final_configured = graphs->final.configured;
		graphs->final.configured = 0;
		assert(handle_radioplus_native_stats(&entry, 0, &stats3) == CLI_FAILURE);
		graphs->final.configured = final_configured;
		usbradioplus_native_graphs_release(&radio);

		/* A reload can briefly unpublish the active graph generation. */
		graphs = atomic_exchange_explicit(&radio.plus_native_graphs.active, NULL,
						  memory_order_seq_cst);
		assert(graphs);
		assert(handle_radioplus_native_stats(&entry, 0, &stats3) == CLI_FAILURE);
		atomic_store_explicit(&radio.plus_native_graphs.active, graphs,
				      memory_order_seq_cst);

		renderer = radio.plus_native_renderer;
		assert(renderer);
		radio.plus_native_renderer = NULL;
		assert(handle_radioplus_native_stats(&entry, 0, &stats3) == CLI_FAILURE);
		radio.plus_native_renderer = renderer;
	}
	usbradio_active = "missing";
	assert(handle_radioplus_native_stats(&entry, 0, &stats3) == CLI_FAILURE);
#undef EXERCISE_HANDLER
	usbradioplus_dsp_destroy(&radio);
	assert(!urp_radio_destroy(radio.radio));
	usbradio_default.next = NULL;
	usbradio_active = NULL;
}

/** @brief Verify tune flash sequences. */
static void test_tune_flash_sequences(void)
{
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state radio_state = {0};

	radio.name = "test";
	radio.radio = &radio_state;
	wait_or_poll_calls = wait_or_poll_fail_call = 0;
	tune_flash(1, &radio, 1);
	assert(wait_or_poll_calls == 5);
	assert(!radio.txtestkey && !radio.plus_test_tone_enabled && !radio_state.txPttIn);

	wait_or_poll_calls = 0;
	wait_or_poll_fail_call = 1;
	tune_flash(1, &radio, 1);
	assert(wait_or_poll_calls == 1);
	wait_or_poll_calls = 0;
	wait_or_poll_fail_call = 2;
	tune_flash(1, &radio, 1);
	assert(wait_or_poll_calls == 2);

	usleep_calls = 0;
	wait_or_poll_fail_call = 0;
	tune_flash(0, &radio, 0);
	assert(usleep_calls == 5);
}

/** @brief Run a synthetic calibration CLI command against a selected test channel.
 * @param radio Named radio profile or signaling state, as declared.
 * @param argument_count Number of synthetic CLI arguments.
 * @param command Synthetic tuning command name.
 * @param value Input value or writable result, as declared.
 * @return Result used by the test's assertions.
 */
static int call_radio_tune(struct chan_usbradio_pvt *radio, int argument_count, const char *command,
			   const char *value)
{
	const char *arguments[] = {"radio", "tune", command, value};
	usbradio_default.next = radio;
	usbradio_active = radio->name;
	return radio_tune(1, argument_count, arguments);
}

/** @brief Exercise radio trace decisions at injected module/file debug levels.
 * @param radio Named radio profile or signaling state, as declared.
 * @param module_level Injected module debug verbosity.
 * @param file_level Injected source-file debug verbosity.
 */
static void exercise_radio_debug_paths(struct chan_usbradio_pvt *radio, unsigned int module_level,
				       unsigned int file_level)
{
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	module_debug_level = module_level;
	file_debug_level = file_level;
	assert(usbradio_indicate(channel, AST_CONTROL_RADIO_KEY, "100.0", 5) == 0);
	assert(usbradio_indicate(channel, AST_CONTROL_RADIO_UNKEY, NULL, 0) == 0);
	radio->forcetxcode = 1;
	assert(radio_config(radio) == 0);
}

/** @brief Verify radio tune dispatch. */
static void test_radio_tune_dispatch(void)
{
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state radio_state = {0};
	int16_t squelch_adjust = 0;

	radio.name = "test";
	strcpy(radio.devstr, "usb-test");
	radio.radio = &radio_state;
	radio.pttkick[1] = -1;
	radio_state.prxSquelchAdjust = &squelch_adjust;
	assert(call_radio_tune(&radio, 2, "rxsquelch", NULL) == RESULT_SHOWUSAGE);
	assert(call_radio_tune(&radio, 5, "rxsquelch", NULL) == RESULT_SHOWUSAGE);
	assert(call_radio_tune(&radio, 3, "menu-support", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 4, "menu-support", "0") == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 3, "swap", NULL) == RESULT_SHOWUSAGE);
	assert(call_radio_tune(&radio, 4, "swap", "missing") == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 3, "rxnoise", NULL) == RESULT_SUCCESS);

	radio.hasusb = 1;
	radio.rxsquelchadj = 500;
	radio_state.rxRssi = 1000;
	assert(call_radio_tune(&radio, 3, "rxsquelch", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 4, "rxsquelch", "-1") == RESULT_SHOWUSAGE);
	assert(call_radio_tune(&radio, 4, "rxsquelch", "1000") == RESULT_SHOWUSAGE);
	assert(call_radio_tune(&radio, 4, "rxsquelch", "600") == RESULT_SUCCESS);
	assert(radio.rxsquelchadj == 600);
	assert(squelch_adjust == ((999 - 600) * 32767) / AUDIO_ADJUSTMENT);

	radio.txmixa = TX_OUT_VOICE;
	radio.txmixb = TX_OUT_OFF;
	radio.txmixaset = 500;
	assert(call_radio_tune(&radio, 3, "txvoice", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 4, "txvoice", "-1") == RESULT_SHOWUSAGE);
	assert(call_radio_tune(&radio, 4, "txvoice", "1000") == RESULT_SHOWUSAGE);
	assert(call_radio_tune(&radio, 4, "txvoice", "700") == RESULT_SUCCESS);
	assert(radio.txmixaset == 700);
	radio.txmixa = TX_OUT_OFF;
	radio.txmixb = TX_OUT_VOICE;
	assert(call_radio_tune(&radio, 3, "txvoice", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 4, "txvoice", "704") == RESULT_SUCCESS);
	assert(radio.txmixbset == 704);
	radio.txmixb = TX_OUT_OFF;
	assert(call_radio_tune(&radio, 3, "txvoice", NULL) == RESULT_SUCCESS);
	radio.txmixa = TX_OUT_COMPOSITE;
	assert(call_radio_tune(&radio, 3, "txvoice", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 4, "txvoice", "706") == RESULT_SUCCESS);
	radio.txmixa = TX_OUT_OFF;
	radio.txmixb = TX_OUT_COMPOSITE;
	assert(call_radio_tune(&radio, 3, "txvoice", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 3, "txall", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 4, "txall", "701") == RESULT_SUCCESS);
	assert(radio.txmixbset == 701);
	radio.txmixb = TX_OUT_VOICE;
	assert(call_radio_tune(&radio, 3, "txall", NULL) == RESULT_SUCCESS);
	radio.txmixa = TX_OUT_VOICE;
	radio.txmixb = TX_OUT_OFF;
	assert(call_radio_tune(&radio, 3, "txall", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 4, "txall", "705") == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 4, "txall", "-1") == RESULT_SHOWUSAGE);
	assert(call_radio_tune(&radio, 4, "txall", "1000") == RESULT_SHOWUSAGE);
	radio.txmixa = TX_OUT_COMPOSITE;
	assert(call_radio_tune(&radio, 3, "txall", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 4, "txall", "706") == RESULT_SUCCESS);
	radio.txmixa = radio.txmixb = TX_OUT_OFF;
	assert(call_radio_tune(&radio, 3, "txall", NULL) == RESULT_SUCCESS);

	radio.txmixa = TX_OUT_AUX;
	radio.txmixb = TX_OUT_OFF;
	assert(call_radio_tune(&radio, 3, "auxvoice", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 4, "auxvoice", "702") == RESULT_SUCCESS);
	assert(radio.txmixaset == 702);
	radio.txmixa = TX_OUT_OFF;
	radio.txmixb = TX_OUT_AUX;
	assert(call_radio_tune(&radio, 3, "auxvoice", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 4, "auxvoice", "703") == RESULT_SUCCESS);
	assert(radio.txmixbset == 703);
	assert(call_radio_tune(&radio, 4, "auxvoice", "1000") == RESULT_SHOWUSAGE);
	assert(call_radio_tune(&radio, 4, "auxvoice", "-1") == RESULT_SHOWUSAGE);
	radio.txmixb = TX_OUT_OFF;
	assert(call_radio_tune(&radio, 3, "auxvoice", NULL) == RESULT_SUCCESS);

	radio.txctcssadj = 200;
	assert(call_radio_tune(&radio, 3, "txtone", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 4, "txtone", "250") == RESULT_SUCCESS);
	assert(radio.txctcssadj == 250);
	assert(call_radio_tune(&radio, 4, "txtone", "1000") == RESULT_SHOWUSAGE);
	assert(call_radio_tune(&radio, 4, "txtone", "-1") == RESULT_SHOWUSAGE);
	assert(call_radio_tune(&radio, 3, "flash", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 3, "load", NULL) == RESULT_SUCCESS);
	assert(radio.eepromctl == 1);
	clear_eeprom_on_usleep = 1;
	assert(call_radio_tune(&radio, 3, "load", NULL) == RESULT_SUCCESS);
	clear_eeprom_on_usleep = 0;
	assert(call_radio_tune(&radio, 3, "save", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 3, "unknown", NULL) == RESULT_SHOWUSAGE);
	assert(!radio_state.b.tuning);
	const char *debug_arguments[] = {"radio", "set", "debug", "50"};
	assert(radio_set_dsp_debug(1, 3, debug_arguments) == RESULT_SUCCESS);
	assert(radio_set_dsp_debug(1, 4, debug_arguments) == RESULT_SUCCESS);
	assert(radio_state.tracelevel == 50);
	debug_arguments[3] = "-1";
	assert(radio_set_dsp_debug(1, 4, debug_arguments) == RESULT_SUCCESS);
	assert(radio_state.tracelevel == 50);
	debug_arguments[3] = "101";
	assert(radio_set_dsp_debug(1, 4, debug_arguments) == RESULT_SUCCESS);
	assert(radio_state.tracelevel == 50);
	usbradio_default.next = NULL;
	usbradio_active = NULL;
}

/** @brief Verify menu adjustment helpers. */
static void test_menu_adjustment_helpers(void)
{
	struct chan_usbradio_pvt radio = {0};
#ifdef URP_TEST_MODERN
	struct ast_radio_device radio_device = {
		.devstr = "usb-test", .serial = "serial-test", .alsa_card = 7};
#endif
	urp_radio_state radio_state = {0};
	int16_t squelch_adjust = 0;
	int32_t ctcss_adjust = 0;
	int32_t voice_adjust = 0;
	int parsed_level = -1;

	assert(parse_tune_level("0", &parsed_level) == 0 && parsed_level == 0);
	assert(parse_tune_level("999", &parsed_level) == 0 && parsed_level == 999);
	assert(parse_tune_level(NULL, &parsed_level) == -1);
	assert(parse_tune_level("", &parsed_level) == -1);
	assert(parse_tune_level("1", NULL) == -1);
	assert(parse_tune_level("1x", &parsed_level) == -1);
	assert(parse_tune_level("1000", &parsed_level) == -1);

	radio.name = "test";
	radio.radio = &radio_state;
#ifdef URP_TEST_MODERN
	radio.radio_device = &radio_device;
#else
	radio.micmax = 100;
#endif
	radio_state.prxSquelchAdjust = &squelch_adjust;
	radio_state.prxCtcssAdjust = &ctcss_adjust;
	radio_state.ptxCtcssAdjust = &ctcss_adjust;
	radio_state.prxVoiceAdjust = &voice_adjust;
	radio.rxdemod = RX_AUDIO_FLAT;
	_menu_rxvoice(1, &radio, "");
	_menu_rxvoice(1, &radio, "bad");
	_menu_rxvoice(1, &radio, "500");
	radio.rxdemod = RX_AUDIO_SPEAKER;
#ifdef URP_TEST_MODERN
	modern_mixer_max = 0;
	_menu_rxvoice(1, &radio, "500");
	tune_rxinput(1, &radio, 0, 1);
	modern_mixer_max = 100;
#endif
	_menu_rxvoice(1, &radio, "");
	_menu_rxvoice(1, &radio, "500");
	radio.rxsquelchadj = 500;
	_menu_rxsquelch(1, &radio, "");
	_menu_rxsquelch(1, &radio, "bad");
	_menu_rxsquelch(1, &radio, "1000");
	_menu_rxsquelch(1, &radio, "600");
	assert(radio.rxsquelchadj == 600);

	radio.txmixa = radio.txmixb = TX_OUT_OFF;
	_menu_txvoice(1, &radio, "");
	radio.txmixa = TX_OUT_VOICE;
	_menu_txvoice(1, &radio, "");
	_menu_txvoice(1, &radio, "bad");
	_menu_txvoice(1, &radio, "1000");
	_menu_txvoice(1, &radio, "321");
	_menu_txvoice(1, &radio, "K");
	_menu_txvoice(1, &radio, "KC");
	_menu_txvoice(1, &radio, "K322");
	_menu_txvoice(1, &radio, "KC323");
	assert(radio.txmixaset == 323);
	radio.txmixa = TX_OUT_OFF;
	radio.txmixb = TX_OUT_COMPOSITE;
	_menu_txvoice(1, &radio, "");
	_menu_txvoice(1, &radio, "324");
	assert(radio.txmixbset == 324);
	radio.txmixb = TX_OUT_VOICE;
	_menu_txvoice(1, &radio, "");
	radio.txmixa = TX_OUT_COMPOSITE;
	radio.txmixb = TX_OUT_OFF;
	_menu_txvoice(1, &radio, "");
	_menu_txvoice(1, &radio, "325");
	assert(radio.txmixaset == 325);

	radio.txmixa = radio.txmixb = TX_OUT_OFF;
	_menu_auxvoice(1, &radio, "");
	radio.txmixa = TX_OUT_AUX;
	_menu_auxvoice(1, &radio, "");
	_menu_auxvoice(1, &radio, "bad");
	_menu_auxvoice(1, &radio, "1000");
	_menu_auxvoice(1, &radio, "401");
	assert(radio.txmixaset == 401);
	radio.txmixa = TX_OUT_OFF;
	radio.txmixb = TX_OUT_AUX;
	_menu_auxvoice(1, &radio, "");
	_menu_auxvoice(1, &radio, "402");
	assert(radio.txmixbset == 402);

	radio.txmixa = radio.txmixb = TX_OUT_OFF;
	_menu_txtone(1, &radio, "");
	_menu_txtone(1, &radio, "bad");
	_menu_txtone(1, &radio, "1000");
	_menu_txtone(1, &radio, "200");
	assert(fabs(radio_state.txCtcssPeak - 32767.0 * 200.0 / 999.0) < 0.001);
	radio.txmixa = TX_OUT_LSD;
	_menu_txtone(1, &radio, "201");
	assert(fabs(radio_state.txCtcssPeak - 32767.0 * 201.0 / 999.0) < 0.001);
	radio.txmixa = TX_OUT_OFF;
	radio.txmixb = TX_OUT_LSD;
	_menu_txtone(1, &radio, "202");
	assert(fabs(radio_state.txCtcssPeak - 32767.0 * 202.0 / 999.0) < 0.001);
	_menu_txtone(1, &radio, "K");
	_menu_txtone(1, &radio, "K203");
	assert(radio.txctcssadj == 203);

	static const int output_modes[] = {TX_OUT_COMPOSITE, TX_OUT_VOICE, TX_OUT_LSD, TX_OUT_AUX};
	strcpy(radio.serial, "serial-test");
	radio.rxdemod = RX_AUDIO_FLAT;
	for (size_t mode = 0; mode < ARRAY_LEN(output_modes); ++mode) {
		radio.txmixa = output_modes[mode];
		radio.txmixb = output_modes[mode];
		_menu_print(1, &radio);
	}

	wait_or_poll_calls = 0;
	wait_or_poll_fail_call = 1;
	tune_txoutput(&radio, 0, 1, 1);
	assert(wait_or_poll_calls == 1);
	usleep_calls = 0;
	tune_txoutput(&radio, 0, 0, 0);
	assert(usleep_calls == 1);
	wait_or_poll_fail_call = 0;
}

/** @brief Verify menu support dispatch. */
static void test_menu_support_dispatch(void)
{
	struct chan_usbradio_pvt radio = {0};
	struct chan_usbradio_pvt first = {0};
	struct chan_usbradio_pvt second = {0};
	struct chan_usbradio_pvt third = {0};
	struct chan_usbradio_pvt unnamed = {0};
	urp_radio_state radio_state = {0};
	urp_radio_stage measure = {0};
	urp_radio_stage receive = {0};
	urp_radio_stage receive_output = {0};
	int16_t squelch_adjust = 0;
	int16_t ctcss_measure = 0;
	int16_t receive_sink = 0;
	int32_t ctcss_adjust = 0;
	int32_t voice_adjust = 0;
	static const char *const usb_commands[] = {"a",	   "b", "c500", "d", "e600", "f321", "g401",
						   "h200", "i", "l",	"v", "Y",    "Z",    "A"};

	radio.name = "selected";
	strcpy(radio.devstr, "usb-selected");
	radio.radio = &radio_state;
	radio_state.prxSquelchAdjust = &squelch_adjust;
	radio_state.prxCtcssAdjust = &ctcss_adjust;
	radio_state.ptxCtcssAdjust = &ctcss_adjust;
	radio_state.prxVoiceAdjust = &voice_adjust;
	radio_state.prxCtcssMeasure = &ctcss_measure;
	radio_state.spsMeasure = &measure;
	radio_state.spsRx = &receive;
	radio_state.spsRxOut = &receive_output;
	measure.source = &receive_sink;
	receive_output.sink = &receive_sink;
	radio.rxdemod = RX_AUDIO_FLAT;
	radio.txmixa = TX_OUT_COMPOSITE;
	radio.txmixb = TX_OUT_LSD;
	radio.rxcdtype = CD_IGNORE;
	radio.rxsdtype = SD_IGNORE;
	first.name = "selected";
	first.next = &second;
	second.name = "other";
	second.next = &third;
	third.name = "third";
	third.next = &unnamed;
	usbradio_default.next = &first;

	tune_menusupport(1, &radio, "0");
	tune_menusupport(1, &radio, "0+9");
	tune_menusupport(1, &radio, "1");
	tune_menusupport(1, &radio, "2");
	tune_menusupport(1, &radio, "3");
	for (size_t index = 0; index < ARRAY_LEN(usb_commands); ++index)
		tune_menusupport(1, &radio, usb_commands[index]);

	radio.hasusb = 1;
	wait_or_poll_fail_call = 1;
	poll_successes_before_exit = 1;
	for (size_t index = 0; index < ARRAY_LEN(usb_commands); ++index) {
		wait_or_poll_calls = 0;
		tune_menusupport(1, &radio, usb_commands[index]);
	}
	wait_or_poll_fail_call = 0;
	poll_successes_before_exit = 1;
	radio.txkeyed = 1;
	tune_menusupport(1, &radio, "y");
	tune_menusupport(1, &radio, "z");
	tune_menusupport(1, &radio, "A");
	radio.txkeyed = 0;
	tune_menusupport(1, &radio, "Z");

	tune_menusupport(1, &radio, "j");
	radio.echomode = 0;
	tune_menusupport(1, &radio, "k");
	radio.echomode = 1;
	tune_menusupport(1, &radio, "k");
	tune_menusupport(1, &radio, "k1");
	tune_menusupport(1, &radio, "k0");
	radio.duplex = 3;
	radio.duplex3 = 500;
	radio.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	fail_realloc = 1;
	tune_menusupport(1, &radio, "k1");
	fail_realloc = 0;
	tune_menusupport(1, &radio, "k1");
	tune_menusupport(1, &radio, "k");

	static const char *const settings[] = {
		"D",  "Dbad", "D-1", "D1000", "D500", "M",	"M2", "M0", "M1",     "o",
		"o0", "p",    "p0",  "q",     "q1",   "q99999", "r",  "r1", "r99999", "s",
		"s0", "s1",   "u",   "u1",    "w",    "w1",	"x",  "x1"};
	for (size_t index = 0; index < ARRAY_LEN(settings); ++index)
		tune_menusupport(1, &radio, settings[index]);
	tune_menusupport(1, &radio, "M");
	tune_menusupport(1, &radio, "s");
	radio.txmixa = TX_OUT_LSD;
	tune_menusupport(1, &radio, "0");
	radio.txmixa = TX_OUT_OFF;
	radio.txmixb = TX_OUT_COMPOSITE;
	tune_menusupport(1, &radio, "0");
	radio.txmixb = TX_OUT_OFF;
	tune_menusupport(1, &radio, "0");
	radio.txtestkey = 1;
	tune_menusupport(1, &radio, "Z");
	radio.txtestkey = 0;
	poll_successes_before_exit = 1;
	tune_menusupport(1, &radio, "z");
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_HID;
	poll_successes_before_exit = 1;
	tune_menusupport(1, &radio, "A");
	radio.rx_cos_active = radio.rx_ctcss_active = radio.rxkeyed = radio.txtestkey = 1;
	tune_menusupport(1, &radio, "A");
	tune_menusupport(1, &radio, "?");
	ast_free(radio.plus_parrot);
	usbradio_default.next = NULL;
	tune_menusupport(1, &radio, "1");
	tune_menusupport(1, &radio, "3");
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
	tune_menusupport(1, &radio, "M");
}

/** @brief Verify tuning displays. */
static void test_tuning_displays(void)
{
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state radio_state = {0};
	urp_radio_stage measure = {0};
	urp_radio_stage receive_output = {0};
	urp_radio_stage receive_input = {0};
	int16_t source = 0;
	int16_t sink = 0;
	int32_t voice_adjust = 0;

	radio.name = "test";
	radio.radio = &radio_state;
	tune_rxdisplay(1, &radio);
	radio_state.spsMeasure = &measure;
	tune_rxdisplay(1, &radio);
	measure.source = &source;
	tune_rxdisplay(1, &radio);
	radio_state.prxVoiceAdjust = &voice_adjust;
	radio_state.spsRxOut = &receive_output;
	receive_output.sink = &sink;
	radio_state.spsRx = &receive_input;
	receive_input.source = &source;
	radio.rxkeyed = 0;
	poll_successes_before_exit = 1;
	tune_rxdisplay(1, &radio);
	assert(!measure.enabled);
	radio.rxkeyed = 1;
	measure.apeak = 8192;
	poll_successes_before_exit = 1;
	tune_rxdisplay(1, &radio);
	assert(!measure.enabled);
	toggle_rxkey_radio = &radio;
	poll_successes_before_exit = 1;
	tune_rxdisplay(1, &radio);
	toggle_rxkey_radio = NULL;

	option_verbose = 3;
	radio.rxcdtype = CD_IGNORE;
	radio.rxsdtype = SD_IGNORE;
	radio.rxkeyed = radio.txkeyed = radio.txtestkey = 0;
	poll_successes_before_exit = 1;
	tune_rxtx_status(1, &radio);
	assert(option_verbose == 3);
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_HID;
	radio.rx_cos_active = radio.rx_ctcss_active = radio.rxkeyed = radio.txtestkey = 1;
	poll_successes_before_exit = 1;
	tune_rxtx_status(1, &radio);
	radio.rx_cos_active = radio.rx_ctcss_active = radio.txtestkey = 0;
	radio.txkeyed = 1;
	poll_successes_before_exit = 1;
	tune_rxtx_status(1, &radio);
}

/** @brief Install scripted detector readings for a calibration test.
 * @param stage Stage supplied by the test scenario.
 * @param value Input value or writable result, as declared.
 * @param count Number of elements available in the supplied block.
 */
static void set_measurements(urp_radio_stage *stage, int value, size_t count)
{
	assert(count <= ARRAY_LEN(scripted_measurements));
	for (size_t i = 0; i < count; ++i)
		scripted_measurements[i] = value;
	scripted_measure_stage = stage;
	scripted_measurement_count = count;
	scripted_measurement_index = 0;
	wait_or_poll_calls = 0;
	wait_or_poll_fail_call = 0;
}

/** @brief Verify receive calibration helpers. */
static void test_receive_calibration_helpers(void)
{
	struct chan_usbradio_pvt radio = {0};
#ifdef URP_TEST_MODERN
	struct ast_radio_device radio_device = {.devstr = "usb-assigned",
						.serial = "serial-assigned"};
#endif
	urp_radio_state radio_state = {0};
	urp_radio_stage measure = {0};
	urp_radio_stage receive_output = {0};
	urp_radio_stage receive_input = {0};
	int16_t source = 0;
	int16_t sink = 0;
	int16_t ctcss_measure = 0;
	int32_t voice_adjust = 0;
	int32_t ctcss_adjust = 0;
	int16_t squelch_adjust = 0;

	radio.name = "test";
	radio.radio = &radio_state;
#ifdef URP_TEST_MODERN
	radio.radio_device = &radio_device;
#endif
	tune_rxvoice(1, &radio, 1);
	radio_state.spsMeasure = &measure;
	tune_rxvoice(1, &radio, 1);
	measure.source = &source;
	radio_state.spsRxOut = &receive_output;
	receive_output.sink = &sink;
	radio_state.spsRx = &receive_input;
	receive_input.source = &source;
	tune_rxvoice(1, &radio, 1);
	radio_state.prxVoiceAdjust = &voice_adjust;
	radio_state.prxSquelchAdjust = &squelch_adjust;

	set_measurements(&measure, 7200, 6);
	wait_or_poll_fail_call = 1;
	tune_rxvoice(1, &radio, 1);
	assert(!radio_state.b.tuning);
	set_measurements(&measure, 7200, 6);
	wait_or_poll_fail_call = 2;
	tune_rxvoice(1, &radio, 1);
	assert(!radio_state.b.tuning);
	set_measurements(&measure, 7200, 6);
	tune_rxvoice(1, &radio, 1);
	assert(!radio_state.b.tuning && !measure.enabled);
	set_measurements(&measure, 1, 12);
	tune_rxvoice(1, &radio, 1);
	set_measurements(&measure, 32767, 12);
	tune_rxvoice(1, &radio, 1);
	set_measurements(&measure, 6840, 12);
	tune_rxvoice(1, &radio, 1);
	set_measurements(&measure, 7560, 12);
	tune_rxvoice(1, &radio, 1);

	radio_state.prxCtcssMeasure = &ctcss_measure;
	radio_state.prxCtcssAdjust = &ctcss_adjust;
	set_measurements(&measure, 2400, 6);
	wait_or_poll_fail_call = 1;
	tune_rxctcss(1, &radio, 1);
	assert(!radio_state.b.tuning);
	set_measurements(&measure, 2400, 6);
	wait_or_poll_fail_call = 2;
	tune_rxctcss(1, &radio, 1);
	assert(!radio_state.b.tuning);
	radio.rxcdtype = CD_IGNORE;
	set_measurements(&measure, 2400, 6);
	tune_rxctcss(1, &radio, 1);
	assert(fabs(radio.rxctcssadj - 1.0F) < 0.001F);
	set_measurements(&measure, 1, 12);
	tune_rxctcss(1, &radio, 1);
	set_measurements(&measure, 32767, 12);
	tune_rxctcss(1, &radio, 1);
	set_measurements(&measure, 2300, 12);
	tune_rxctcss(1, &radio, 1);
	set_measurements(&measure, 2500, 12);
	tune_rxctcss(1, &radio, 1);

	radio.rxcdtype = CD_XPMR_NOISE;
	radio.rxsquelchadj = 900;
	radio_state.rxRssi = 16384;
	set_measurements(&measure, 2400, 6);
	tune_rxctcss(1, &radio, 1);
	radio.rxsquelchadj = 100;
	set_measurements(&measure, 2400, 6);
	tune_rxctcss(1, &radio, 1);
	set_measurements(&measure, 2400, 6);
	wait_or_poll_fail_call = 13;
	tune_rxctcss(1, &radio, 1);

	radio.hasusb = 1;
	radio.rxdemod = RX_AUDIO_FLAT;
	wait_or_poll_calls = 0;
	wait_or_poll_fail_call = 1;
	assert(call_radio_tune(&radio, 3, "rxnoise", NULL) == RESULT_SUCCESS);
	wait_or_poll_calls = 0;
	assert(call_radio_tune(&radio, 3, "rxvoice", NULL) == RESULT_SUCCESS);
	wait_or_poll_calls = 0;
	assert(call_radio_tune(&radio, 3, "rxtone", NULL) == RESULT_SUCCESS);

#ifndef URP_TEST_MODERN
	radio.micmax = 100;
#endif
	radio.rxdemod = RX_AUDIO_FLAT;
	radio.rxcdtype = CD_XPMR_NOISE;
	radio_state.rxRssi = 12000;
	set_measurements(&measure, 27000, 20);
	tune_rxinput(1, &radio, 1, 1);
	assert(!radio_state.b.tuning);
	assert(radio.rxsquelchadj <= 999);
	set_measurements(&measure, 27000, 20);
	wait_or_poll_fail_call = 15;
	tune_rxinput(1, &radio, 1, 1);
	assert(!radio_state.b.tuning);
	set_measurements(&measure, 27000, 20);
	wait_or_poll_fail_call = 16;
	tune_rxinput(1, &radio, 1, 1);
	assert(!radio_state.b.tuning);
	set_measurements(&measure, 27000, 20);
	wait_or_poll_fail_call = 2;
	tune_rxinput(1, &radio, 1, 1);
	assert(!radio_state.b.tuning);

	radio.rxdemod = RX_AUDIO_SPEAKER;
	radio.rxcdtype = CD_XPMR_NOISE;
	radio_state.rxRssi = 12000;
	set_measurements(&measure, 23000, 20);
	tune_rxinput(1, &radio, 1, 1);
	radio.rxcdtype = CD_HID;
	set_measurements(&measure, 23000, 20);
	tune_rxinput(1, &radio, 1, 1);
	radio.rxcdtype = CD_XPMR_NOISE;
	set_measurements(&measure, 0, 28);
	tune_rxinput(1, &radio, 1, 1);
	set_measurements(&measure, 32767, 28);
	tune_rxinput(1, &radio, 1, 1);

	radio.rxdemod = RX_AUDIO_FLAT;
	radio_state.rxRssi = 1000;
	set_measurements(&measure, 27000, 20);
	tune_rxinput(1, &radio, 1, 1);
	assert(radio.rxsquelchadj == 999);
	radio_state.rxRssi = 12000;
	radio.rxsquelchadj = 0;
	set_measurements(&measure, 27000, 20);
	tune_rxinput(1, &radio, 0, 1);
	radio.rxsquelchadj = 999;
	radio.rxaudiostats.pwrbuf[AUDIO_STATS_LEN - 1] = 100;
	set_measurements(&measure, 27000, 20);
	tune_rxinput(1, &radio, 0, 1);

	scripted_measure_stage = NULL;
	wait_or_poll_fail_call = 0;
	usbradio_default.next = NULL;
	usbradio_active = NULL;
}

/** @brief Verify config update and radio programming. */
static void test_config_update_and_radio_programming(void)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	struct ast_category *category = (struct ast_category *)(uintptr_t)1;
	struct ast_variable variable = {.name = "gain", .value = "1"};
	struct ast_variable unrelated = {.name = "other", .value = "0", .next = &variable};
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state template = {0};
#ifdef URP_TEST_MODERN
	struct ast_radio_device radio_device = {
		.devstr = "usb-dump", .serial = "serial-dump", .alsa_card = 9};
#endif

	test_config_variables = &variable;
	assert(usbradioplus_config_variable_update(config, "test.conf", category, "gain", "1") ==
	       0);
	test_config_variables = &unrelated;
	assert(usbradioplus_config_variable_update(config, "test.conf", category, "gain", "1") ==
	       0);
	test_config_variables = &variable;
	variable_update_result = 0;
	assert(usbradioplus_config_variable_update(config, "test.conf", category, "gain", "2") ==
	       0);
	variable_update_result = -1;
	variable_append_calls = 0;
	assert(usbradioplus_config_variable_update(config, "test.conf", category, "gain", "3") ==
	       0);
	assert(variable_append_calls == 1);
	variable.inherited = 1;
	assert(usbradioplus_config_variable_update(config, "test.conf", category, "gain", "4") ==
	       0);
	variable.inherited = 0;
	test_config_variables = NULL;
	assert(usbradioplus_config_variable_update(config, "test.conf", category, "missing", "1") ==
	       0);
	variable_new_failure = 1;
	assert(usbradioplus_config_variable_update(config, "test.conf", category, "missing", "1") ==
	       -1);
	variable_new_failure = 0;
	variable_update_result = 0;

	radio.name = "test";
	atomic_init(&radio.plus_radio_program_generation, 0U);
	atomic_init(&radio.plus_radio_program_rx_frequency, 0U);
	atomic_init(&radio.plus_radio_program_tx_frequency, 0U);
	atomic_init(&radio.plus_radio_program_high_power, 0);
	assert(radio_config(&radio) == 1);
	template.pRxCodeSrc = "100.0";
	template.pTxCodeSrc = "100.0";
	template.pTxCodeDefault = "100.0";
	radio.radio = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(radio.radio);
#ifdef URP_TEST_MODERN
	radio.radio_device = &radio_device;
#endif
	strcpy(radio.rxctcssfreqs, "100.0");
	strcpy(radio.txctcssfreqs, "100.0");
	strcpy(radio.txctcssdefault, "100.0");
	settings_defaults(&settings);
	radio.name = "usb";
	test_channel_private = &radio;
	option_debug = 0;
	ast_set_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	exercise_radio_debug_paths(&radio, 10, 0);
	exercise_radio_debug_paths(&radio, 0, 10);
	exercise_radio_debug_paths(&radio, 0, 0);
	ast_clear_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	option_debug = 10;
	assert(usbradio_indicate((struct ast_channel *)(uintptr_t)1, AST_CONTROL_RADIO_KEY, "123.0",
				 5) == 0);
	assert(radio.forcetxcode && !strcmp(radio.set_txctcssfreq, "123.0"));
	assert(usbradio_indicate((struct ast_channel *)(uintptr_t)1, AST_CONTROL_RADIO_UNKEY, NULL,
				 0) == 0);
	option_debug = 0;
	haspp = 2;
	assert(radio_config(&radio) == 0);
	assert(!strcmp(radio.radio->pRxCodeSrc, "100.0"));
	assert(!strcmp(radio.radio->pTxCodeSrc, "100.0"));

	strcpy(radio.rxctcssfreqs, "67.0");
	strcpy(radio.txctcssfreqs, "88.5");
	assert(radio_config(&radio) == 0);
	assert(!strcmp(radio.radio->pRxCodeSrc, "67.0"));
	assert(!strcmp(radio.radio->pTxCodeSrc, "88.5"));

	radio.remoted = 1;
	strcpy(radio.set_rxctcssfreqs, "71.9");
	strcpy(radio.set_txctcssfreqs, "74.4");
	strcpy(radio.set_txctcssdefault, "77.0");
	assert(radio_config(&radio) == 0);
	assert(radio.radio->pRxCodeSrc == radio.set_rxctcssfreqs);
	radio.forcetxcode = 1;
	strcpy(radio.set_txctcssfreq, "79.7");
	assert(radio_config(&radio) == 0);
	assert(radio.radio->pTxCodeDefault == radio.set_txctcssfreq);

	struct usbradioplus_radio_program_request program_request;

	parallel_write_calls = 0;
	haspp = 2;
	pp_val = 0;
	radio.rxfreq = 146520000;
	radio.txfreq = 146520000;
	radio.remoted = 0;
	usbradioplus_program_radio(&radio);
	/* Programming is published by the control plane and consumed by the HID
	 * worker. It must not write the parallel bus synchronously. */
	assert(parallel_write_calls == 0);
	assert(usbradioplus_read_radio_program_request(&radio, &program_request));
	assert(program_request.rx_frequency == 146520000U);
	assert(program_request.tx_frequency == 146520000U);
	assert(!program_request.high_power);
	assert(!(program_request.generation & 1U));
	radio.remoted = 1;
	radio.set_rxfreq = 147000000;
	radio.set_txfreq = 147600000;
	radio.set_txpower = 1;
	usbradioplus_program_radio(&radio);
	assert(parallel_write_calls == 0);
	assert(usbradioplus_read_radio_program_request(&radio, &program_request));
	assert(program_request.rx_frequency == 147000000U);
	assert(program_request.tx_frequency == 147600000U);
	assert(program_request.high_power);
	usbradioplus_program_radio(NULL);
	parallel_write_calls = 0;
	usbradioplus_set_channel(7);
	assert(parallel_write_calls > 0);
	usbradioplus_parallel_program_write(NULL, 0x55);
	assert(pp_val == 0x55);
	haspp = 0;
	usbradioplus_program_radio(&radio);
	usbradioplus_set_channel(1);

	radio.remoted = 0;
	radio.plus_hardware_applied = 0;
	radio.rxmixerset = 100;
	radio.txmixaset = 200;
	radio.txmixbset = 300;
	radio.txmixa = TX_OUT_VOICE;
	radio.txmixb = TX_OUT_LSD;
	settings_defaults(&settings);
	refresh_processing_hardware(&radio);
	assert(radio.plus_hardware_applied);
	assert(radio.plus_applied_rxmixer == 500);
	refresh_processing_hardware(&radio);
	settings.profiles[0].hardware.output_a_assignment = TX_OUT_VOICE;
	settings.profiles[0].hardware.output_b_assignment = TX_OUT_VOICE;
	radio.plus_hardware_applied = 0;
	refresh_processing_hardware(&radio);
	assert(radio.radio->txOutputGainA == radio.radio->txOutputGainB);
	settings.profiles[0].hardware.input_gain_configured = 1;
	settings.profiles[0].hardware.input_gain_db = 0.0;
	settings.profiles[0].hardware.output_a_gain_configured = 1;
	settings.profiles[0].hardware.output_a_gain_db = 0.0;
	settings.profiles[0].hardware.output_b_gain_configured = 1;
	settings.profiles[0].hardware.output_b_gain_db = 0.0;
	settings.profiles[0].hardware.output_a_assignment_configured = 1;
	settings.profiles[0].hardware.output_a_assignment = TX_OUT_COMPOSITE;
	settings.profiles[0].hardware.output_b_assignment_configured = 1;
	settings.profiles[0].hardware.output_b_assignment = TX_OUT_AUX;
	settings.profiles[0].hardware.output_b_gain_db = 6.0;
	strcpy(radio.rxctcssfreqs, "67.0");
	strcpy(radio.txctcssfreqs, "71.9");
	radio.plus_hardware_applied = 0;
	refresh_processing_hardware(&radio);
	assert(radio.plus_applied_txmixa != radio.plus_applied_txmixb);
	assert(radio.radio->txOutputGainA != radio.radio->txOutputGainB);
	assert(!strcmp(radio.plus_applied_rxctcssfreqs, "67.0"));
	assert(!strcmp(radio.plus_applied_txctcssfreqs, "71.9"));
	radio.remoted = 1;
	strcpy(radio.rxctcssfreqs, "74.4");
	refresh_processing_hardware(&radio);
	assert(!strcmp(radio.plus_applied_rxctcssfreqs, "67.0"));
	settings_defaults(&settings);
	radio.txmixa = radio.txmixb = TX_OUT_VOICE;
	settings.profiles[0].hardware.output_a_assignment = TX_OUT_VOICE;
	settings.profiles[0].hardware.output_b_assignment = TX_OUT_VOICE;
	mult_set(&radio);
	assert(radio.radio->txOutputGainA == radio.radio->txOutputGainB);
	radio.txmixb = TX_OUT_LSD;
	settings.profiles[0].hardware.output_b_assignment = TX_OUT_LSD;
	settings.profiles[0].hardware.output_b_gain_configured = 1;
	settings.profiles[0].hardware.output_b_gain_db = 6.0;
	mult_set(&radio);
	assert(radio.radio->txOutputGainA != radio.radio->txOutputGainB);
	settings_defaults(&settings);
	radio.remoted = 0;
	strcpy(radio.rxctcssfreqs, "100.0");
	strcpy(radio.txctcssfreqs, "123.0");
	strcpy(radio.plus_applied_rxctcssfreqs, "100.0");
	strcpy(radio.plus_applied_txctcssfreqs, "100.0");
	/* Exercise the transmit-only CTCSS update; the receive strings agree. */
	refresh_processing_hardware(&radio);
	assert(!strcmp(radio.plus_applied_txctcssfreqs, "123.0"));
	strcpy(radio.rxctcssfreqs, "103.5");
	strcpy(radio.txctcssfreqs, "123.0");
	strcpy(radio.plus_applied_rxctcssfreqs, "103.5");
	strcpy(radio.plus_applied_txctcssfreqs, "100.0");
	refresh_processing_hardware(&radio);
	refresh_processing_hardware(&radio);
	refresh_processing_hardware(&radio);

#define SET_APPLIED(rx, a, b, route_a, route_b)                                                    \
	do {                                                                                       \
		radio.plus_hardware_applied = 1;                                                   \
		radio.plus_applied_rxmixer = (rx);                                                 \
		radio.plus_applied_txmixaset = (a);                                                \
		radio.plus_applied_txmixbset = (b);                                                \
		radio.plus_applied_txmixa = (route_a);                                             \
		radio.plus_applied_txmixb = (route_b);                                             \
	} while (0)
	int effective_rx = effective_rxmixerset(&radio);
	int effective_a = effective_txmixaset(&radio);
	int effective_b = effective_txmixbset(&radio);
	int effective_route_a = effective_txmixa(&radio);
	int effective_route_b = effective_txmixb(&radio);
	SET_APPLIED(effective_rx + 1, effective_a, effective_b, effective_route_a,
		    effective_route_b);
	refresh_processing_hardware(&radio);
	SET_APPLIED(effective_rx, effective_a + 1, effective_b, effective_route_a,
		    effective_route_b);
	refresh_processing_hardware(&radio);
	SET_APPLIED(effective_rx, effective_a, effective_b + 1, effective_route_a,
		    effective_route_b);
	refresh_processing_hardware(&radio);
	SET_APPLIED(effective_rx, effective_a, effective_b, effective_route_a + 1,
		    effective_route_b);
	refresh_processing_hardware(&radio);
	SET_APPLIED(effective_rx, effective_a, effective_b, effective_route_a,
		    effective_route_b + 1);
	refresh_processing_hardware(&radio);
#undef SET_APPLIED
	/* The CTCSS cache must distinguish unchanged local values from a remote
	 * channel, where radio configuration owns the active tone strings. */
	radio.remoted = 0;
	strcpy(radio.rxctcssfreqs, "100.0");
	strcpy(radio.txctcssfreqs, "100.0");
	strcpy(radio.plus_applied_rxctcssfreqs, "100.0");
	strcpy(radio.plus_applied_txctcssfreqs, "100.0");
	refresh_processing_hardware(&radio);
	strcpy(radio.rxctcssfreqs, "123.0");
	strcpy(radio.txctcssfreqs, "100.0");
	strcpy(radio.plus_applied_rxctcssfreqs, "100.0");
	strcpy(radio.plus_applied_txctcssfreqs, "100.0");
	refresh_processing_hardware(&radio);
	radio.remoted = 1;
	strcpy(radio.rxctcssfreqs, "123.0");
	strcpy(radio.txctcssfreqs, "123.0");
	strcpy(radio.plus_applied_rxctcssfreqs, "100.0");
	strcpy(radio.plus_applied_txctcssfreqs, "100.0");
	refresh_processing_hardware(&radio);
	radio.remoted = 0;
	radio.rxcdtype = CD_XPMR_VOX;
	radio.voxhangtime = 250;
	radio.numrxctcssfreqs = 1;
	radio.rxctcss[0] = "100.0";
	radio.txctcss[0] = "100.0";
	assert(call_radio_tune(&radio, 3, "dump", NULL) == RESULT_SUCCESS);
	radio.rxcdtype = CD_IGNORE;
	radio.numrxctcssfreqs = 0;
	radio.radio->numrxcodes = 0;
	radio.radio->numtxcodes = 0;
#ifdef URP_TEST_MODERN
	radio.radio_device = NULL;
#endif
	assert(call_radio_tune(&radio, 3, "dump", NULL) == RESULT_SUCCESS);
	assert(!urp_radio_destroy(radio.radio));
	test_config_variables = NULL;
}

/** @brief Verify the lock-free audio/physical-hardware handoff snapshots. */
static void test_hardware_handoff_snapshots(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio = {0};
	struct usbradioplus_radio_program_request request;
	const unsigned int inputs = URP_HARDWARE_INPUT_HID_CARRIER | URP_HARDWARE_INPUT_HID_CTCSS |
				    URP_HARDWARE_INPUT_PARALLEL_CARRIER |
				    URP_HARDWARE_INPUT_PARALLEL_CTCSS;

	atomic_init(&channel.plus_hardware_ptt_request, 0);
	atomic_init(&channel.plus_hardware_ptt_applied, 0);
	atomic_init(&channel.plus_hardware_inputs, 0U);
	atomic_init(&channel.plus_clip_led_request, 0);
	atomic_init(&channel.plus_radio_program_generation, 0U);
	atomic_init(&channel.plus_radio_program_rx_frequency, 0U);
	atomic_init(&channel.plus_radio_program_tx_frequency, 0U);
	atomic_init(&channel.plus_radio_program_high_power, 0);

	/* Null or incomplete fixture calls are inert, as they are during teardown. */
	usbradioplus_audio_load_hardware_state(NULL);
	usbradioplus_publish_hardware_ptt(NULL, 1);
	usbradioplus_request_clip_led(NULL);
	usbradioplus_publish_hardware_inputs(NULL, inputs);
	assert(!usbradioplus_read_radio_program_request(NULL, &request));
	assert(!usbradioplus_read_radio_program_request(&channel, NULL));

	channel.radio = &radio;
	atomic_store_explicit(&channel.plus_hardware_ptt_applied, 1, memory_order_release);
	usbradioplus_publish_hardware_inputs(&channel, inputs);
	usbradioplus_audio_load_hardware_state(&channel);
	assert(channel.rxhidsq && channel.rxhidctcss && channel.rxppsq && channel.rxppctcss);
	assert(channel.radio->txPttHid);
	/* Each audio-side import is a complete atomic snapshot, rather than an
	 * OR-only update that could retain a departed carrier or decoder state. */
	atomic_store_explicit(&channel.plus_hardware_ptt_applied, 0, memory_order_release);
	usbradioplus_publish_hardware_inputs(&channel, 0U);
	usbradioplus_audio_load_hardware_state(&channel);
	assert(!channel.rxhidsq && !channel.rxhidctcss && !channel.rxppsq && !channel.rxppctcss);
	assert(!channel.radio->txPttHid);
	usbradioplus_publish_hardware_ptt(&channel, 1);
	assert(atomic_load_explicit(&channel.plus_hardware_ptt_request, memory_order_acquire));
	usbradioplus_publish_hardware_ptt(&channel, 0);
	assert(!atomic_load_explicit(&channel.plus_hardware_ptt_request, memory_order_acquire));
	channel.clipledgpio = 1;
	usbradioplus_request_clip_led(&channel);
	assert(atomic_load_explicit(&channel.plus_clip_led_request, memory_order_acquire));
	atomic_store_explicit(&channel.plus_clip_led_request, 0, memory_order_release);
	channel.clipledgpio = 0;
	usbradioplus_request_clip_led(&channel);
	assert(!atomic_load_explicit(&channel.plus_clip_led_request, memory_order_acquire));

	channel.rxfreq = 146520000U;
	channel.txfreq = 147000000U;
	usbradioplus_program_radio(&channel);
	assert(usbradioplus_read_radio_program_request(&channel, &request));
	assert(request.rx_frequency == (uint32_t)channel.rxfreq &&
	       request.tx_frequency == (uint32_t)channel.txfreq);
	assert(!request.high_power && !(request.generation & 1U));
	/* An odd publication generation is never exposed as a torn request. */
	atomic_store_explicit(&channel.plus_radio_program_generation, request.generation | 1U,
			      memory_order_release);
	assert(!usbradioplus_read_radio_program_request(&channel, &request));
	atomic_store_explicit(&channel.plus_radio_program_generation, request.generation + 2U,
			      memory_order_release);
	/* Input import remains valid during radio-state teardown. */
	usbradioplus_publish_hardware_inputs(&channel, inputs);
	channel.radio = NULL;
	usbradioplus_audio_load_hardware_state(&channel);
	assert(channel.rxhidsq && channel.rxhidctcss && channel.rxppsq && channel.rxppctcss);
}

/** @brief Append a synthetic processing configuration value to the harness.
 * @param section Flat or resolved configuration section name.
 * @param name Option, metadata field, or channel name.
 * @param value Input value or writable result, as declared.
 */
static void add_processing_override(const char *section, const char *name, const char *value)
{
	struct section_override *entry =
		&settings.profiles[0].overrides[settings.profiles[0].override_count++];
	assert(settings.profiles[0].override_count <= MAX_SECTION_OVERRIDES);
	ast_copy_string(entry->section, section, sizeof(entry->section));
	ast_copy_string(entry->name, name, sizeof(entry->name));
	ast_copy_string(entry->value, value, sizeof(entry->value));
}

/** @brief Add every non-audio modern setting with one valid representative value.
 *
 * The channel loader applies these values after the processing parser has
 * resolved a profile.  Keeping this exhaustive fixture independent of the
 * smaller behavior tests makes new configuration fields visibly require a
 * parser, a loader mapping, and a coverage case.
 */
static void add_complete_processing_override_fixture(void)
{
	static const struct {
		const char *section;
		const char *name;
		const char *value;
	} values[] = {
		{"asterisk", "asterisk_jitter_buffer_enabled", "yes"},
		{"asterisk", "asterisk_jitter_buffer_max_size_ms", "100"},
		{"asterisk", "asterisk_jitter_buffer_resync_threshold_ms", "100"},
		{"asterisk", "asterisk_jitter_buffer_implementation", "adaptive"},
		{"asterisk", "asterisk_jitter_buffer_logging_enabled", "yes"},
		{"asterisk", "asterisk_jitter_buffer_force_enabled", "yes"},
		{"asterisk", "asterisk_jitter_buffer_target_extra_ms", "100"},
		{"asterisk", "asterisk_jitter_buffer_video_sync_enabled", "yes"},
		{"hardware", "hardware_device_identifier", "usb-complete"},
		{"hardware", "hardware_serial", "serial-complete"},
		{"hardware", "hardware_interface_type", "1"},
		{"hardware", "hardware_eeprom_enabled", "yes"},
		{"hardware", "hardware_audio_fragment_count", "4"},
		{"hardware", "hardware_audio_queue_size", "8"},
		{"hardware", "hardware_ptt_inverted", "yes"},
		{"hardware", "hardware_repeater_number", "1"},
		{"hardware", "hardware_area", "2"},
		{"hardware", "hardware_user_key", "complete-key"},
		{"hardware", "hardware_idle_interval", "3"},
		{"hardware", "hardware_turnoff_count", "4"},
		{"hardware", "hardware_voter_reporting", "1"},
		{"hardware", "hardware_clip_led_gpio", "8"},
		{"hardware", "hardware_gpio_1_mode", "in"},
		{"hardware", "hardware_parallel_port_device", "/dev/parport0"},
		{"hardware", "hardware_parallel_port_base_address", "0x378"},
		{"hardware", "hardware_parallel_pin_2_assignment", "out0"},
		{"hardware", "hardware_deemphasis_corner_hz", "250"},
		{"hardware", "hardware_preemphasis_corner_hz", "500"},
		{"receive", "cpu_saver_enabled", "yes"},
		{"receive", "audio_source", "flat"},
		{"receive", "signaling_method", "ctcss"},
		{"receive", "vox_hang_ms", "100"},
		{"receive", "vox_threshold", "20"},
		{"receive", "noise_squelch_hysteresis", "2"},
		{"receive", "noise_filter_type", "1"},
		{"receive", "squelch_delay_ms", "10"},
		{"receive", "on_delay_frames", "10"},
		{"receive", "polarity_inverted", "yes"},
		{"receive", "squelch_level", "450"},
		{"receive", "frequency_hz", "146520000"},
		{"receive", "lsd_polarity_inverted", "yes"},
		{"receive", "cos_assignment", "dsp"},
		{"transmit", "cpu_saver_enabled", "yes"},
		{"transmit", "signaling_method", "ctcss"},
		{"transmit", "preemphasis_enabled", "yes"},
		{"transmit", "settle_ms", "100"},
		{"transmit", "rx_blanking_ms", "10"},
		{"transmit", "off_delay_frames", "10"},
		{"transmit", "polarity_inverted", "yes"},
		{"transmit", "frequency_hz", "146520000"},
		{"transmit", "lsd_polarity_inverted", "yes"},
		{"ctcss", "receive_frequencies", "100.0"},
		{"ctcss", "transmit_frequencies", "100.0"},
		{"ctcss", "receive_source", "dsp"},
		{"ctcss", "receive_decoder_gain_db", "6"},
		{"ctcss", "receive_override_enabled", "yes"},
		{"ctcss", "receive_relax", "1"},
		{"ctcss", "transmit_default_hz", "100.0"},
		{"ctcss", "transmit_peak_dbfs", "-12"},
		{"ctcss", "turnoff_mode", "ctcss_phase_shift"},
		{"ctcss", "phase_shift_degrees", "180"},
		{"ctcss", "tail_duration_ms", "100"},
		{"ctcss", "tail_frequency_hz", "55"},
		{"dcs", "receive_code", "023N"},
		{"dcs", "transmit_code", "431I"},
		{"dcs", "turnoff_code_enabled", "yes"},
		{"dcs", "turnoff_duration_ms", "180"},
		{"dcs", "peak_dbfs", "-18"},
		{"duplex", "duplex_radio_mode", "1"},
		{"duplex", "duplex_local_repeat_level", "999"},
		{"duplex", "duplex_local_repeat_mode", "software"},
		{"diagnostics", "diagnostics_trace_type", "1"},
		{"diagnostics", "diagnostics_trace_level", "2"},
		{"diagnostics", "diagnostics_fever", "3"},
		{"general", "channel_enabled", "yes"},
	};
	size_t index;

	for (index = 0; index < ARRAY_LEN(values); ++index)
		add_processing_override(values[index].section, values[index].name,
					values[index].value);
}

/** @brief Release channel-owned GPIO and parallel-pin strings from a fixture.
 * @param radio Channel fixture whose optional assignment strings are released.
 */
static void clear_processing_assignment_fixture(struct chan_usbradio_pvt *radio)
{
	/* One assignment per generated option family covers its true and false
	 * branches without making this configuration fixture needlessly repetitive. */
	ast_free(radio->gpios[0]);
	radio->gpios[0] = NULL;
	ast_free(radio->pps[2]);
	radio->pps[2] = NULL;
}

/** @brief Release every optional assignment string created by an override fixture.
 * @param radio Fixture whose dynamically copied GPIO and parallel assignments are released.
 */
static void clear_all_processing_assignments(struct chan_usbradio_pvt *radio)
{
	size_t index;

	for (index = 0; index < GPIO_PINCOUNT; ++index) {
		ast_free(radio->gpios[index]);
		radio->gpios[index] = NULL;
	}
	for (index = 0; index < ARRAY_LEN(radio->pps); ++index) {
		ast_free(radio->pps[index]);
		radio->pps[index] = NULL;
	}
}

/** @brief Verify processing config overrides. */
static void test_processing_config_overrides(void)
{
	struct chan_usbradio_pvt radio = {0};
	size_t index;

	settings_defaults(&settings);
	add_processing_override("hardware", "hardware_device_identifier", "usb-test");
	add_processing_override("hardware", "hardware_serial", "serial-test");
	add_processing_override("hardware", "hardware_interface_type", "1");
	add_processing_override("hardware", "hardware_eeprom_enabled", "yes");
	add_processing_override("hardware", "hardware_audio_fragment_count", "1");
	add_processing_override("hardware", "hardware_audio_queue_size", "1");
	add_processing_override("hardware", "hardware_ptt_inverted", "yes");
	add_processing_override("hardware", "hardware_user_key", "user-key");
	add_processing_override("hardware", "hardware_gpio_1_mode", "in");
	add_processing_override("hardware", "hardware_parallel_pin_15_assignment", "out");
	add_processing_override("hardware", "hardware_deemphasis_corner_hz", "250.0");
	add_processing_override("hardware", "hardware_preemphasis_corner_hz", "500.0");
	for (index = 0; index < ARRAY_LEN(asterisk_override_options); ++index) {
		const char *value = "100";
		if (index == 0 || index == 4 || index == 5 || index == 7)
			value = "yes";
		else if (index == 3)
			value = "adaptive";
		add_processing_override("asterisk", asterisk_override_options[index], value);
	}
	add_processing_override("duplex", duplex_override_options[0], "3");
	add_processing_override("duplex", duplex_override_options[1], "999");
	add_processing_override("duplex", duplex_override_options[2], "software");
	add_processing_override("receive", "audio_source", "flat");
	add_processing_override("receive", "cos_assignment", "dsp");
	add_processing_override("receive", "signaling_method", "ctcss");
	add_processing_override("transmit", "signaling_method", "ctcss");
	add_processing_override("transmit", "preemphasis_enabled", "yes");
	add_processing_override("ctcss", "receive_source", "dsp");
	add_processing_override("ctcss", "receive_frequencies", "100.0");
	add_processing_override("ctcss", "transmit_frequencies", "100.0");
	add_processing_override("ctcss", "transmit_default_hz", "100.0");
	for (index = 0; index < ARRAY_LEN(diagnostics_override_options); ++index)
		add_processing_override("diagnostics", diagnostics_override_options[index], "1");
	add_processing_override("general", "channel_enabled", "yes");
	assert(apply_processing_config_overrides(&radio, "usb") == 0);
	assert(!strcmp(radio.devstr, "usb-test"));
	assert(!strcmp(radio.serial, "serial-test"));
	assert(radio.rxdemod == RX_AUDIO_FLAT);
	assert(radio.rxsdtype == SD_XPMR);
	assert(radio.duplex3mode == DUPLEX3_MODE_SOFTWARE);
	assert(radio.duplex3 == 999);
	assert(radio.radioactive);
	assert(radio.txpreemphasis);
	assert(fabs(radio.plus_deemphasis_corner_hz - 250.0) < 0.001);
	assert(fabs(radio.plus_preemphasis_corner_hz - 500.0) < 0.001);
	assert(radio.gpios[0] && !strcmp(radio.gpios[0], "in"));
	assert(radio.pps[15] && !strcmp(radio.pps[15], "out"));
	for (index = 0; index < GPIO_PINCOUNT; ++index) {
		ast_free(radio.gpios[index]);
		radio.gpios[index] = NULL;
	}
	for (index = 0; index < ARRAY_LEN(radio.pps); ++index) {
		ast_free(radio.pps[index]);
		radio.pps[index] = NULL;
	}

	settings_defaults(&settings);
	add_processing_override("receive", "on_delay_frames", "999999");
	add_processing_override("transmit", "off_delay_frames", "999999");
	add_processing_override("duplex", duplex_override_options[2], "hardware");
	assert(apply_processing_config_overrides(&radio, "usb") == 0);
	assert(radio.rxondelay == MS_TO_FRAMES(RX_ON_DELAY_MAX));
	assert(radio.txoffdelay == MS_TO_FRAMES(TX_OFF_DELAY_MAX));
	assert(radio.duplex3mode == DUPLEX3_MODE_HARDWARE);

	settings_defaults(&settings);
	add_processing_override("receive", "cpu_saver_enabled", "no");
	add_processing_override("transmit", "cpu_saver_enabled", "no");
	add_processing_override("transmit", "preemphasis_enabled", "no");
	add_processing_override("general", "channel_enabled", "no");
	assert(apply_processing_config_overrides(&radio, "usb") == 0);
	assert(!radio.rxcpusaver && !radio.txcpusaver && !radio.radioactive);
	assert(!radio.txpreemphasis);

	settings_defaults(&settings);
	add_processing_override("hardware", "hardware_interface_type", "bad");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	settings_defaults(&settings);
	add_processing_override("hardware", "hardware_eeprom_enabled", "bad");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	settings_defaults(&settings);
	add_processing_override("ctcss", "receive_decoder_gain_db", "nan");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	settings_defaults(&settings);
	add_processing_override("duplex", duplex_override_options[2], "invalid");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	settings_defaults(&settings);
	add_processing_override("hardware", "hardware_deemphasis_corner_hz", "invalid");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	settings_defaults(&settings);
	add_processing_override("hardware", "hardware_preemphasis_corner_hz", "invalid");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	settings_defaults(&settings);
	add_processing_override("asterisk", asterisk_override_options[0], "yes");
	jitter_config_result = -1;
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	jitter_config_result = 0;
	settings_defaults(&settings);
	add_processing_override("hardware", "hardware_gpio_1_mode", "in");
	ast_strdup_calls = 0;
	fail_ast_strdup_call = 1;
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	settings_defaults(&settings);
	add_processing_override("hardware", "hardware_parallel_pin_2_assignment", "out");
	ast_strdup_calls = 0;
	fail_ast_strdup_call = 1;
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	fail_ast_strdup_call = 0;

	/* Exercise the complete clean-slate CTCSS/DCS mapping once with valid
	 * values, including protocol selection clearing inactive state. */
	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("receive", "cos_assignment", "vox");
	add_processing_override("receive", "signaling_method", "dcs");
	add_processing_override("transmit", "signaling_method", "dcs");
	add_processing_override("ctcss", "receive_decoder_gain_db", "6");
	add_processing_override("ctcss", "transmit_peak_dbfs", "-12");
	add_processing_override("ctcss", "turnoff_mode", "ctcss_tail_tone");
	add_processing_override("ctcss", "phase_shift_degrees", "180");
	add_processing_override("ctcss", "tail_frequency_hz", "55");
	add_processing_override("dcs", "receive_code", "023N");
	add_processing_override("dcs", "transmit_code", "431I");
	add_processing_override("dcs", "peak_dbfs", "-18");
	assert(apply_processing_config_overrides(&radio, "usb") == 0);
	assert(radio.rxcdtype == CD_XPMR_VOX);
	/* Direction selection leaves inactive CTCSS data intact for a later live
	 * signaling change; radio_config() chooses which direction is active. */
	assert(!strcmp(radio.rxctcssfreqs, "100.0"));
	assert(!strcmp(radio.txctcssfreqs, "100.0"));
	assert(radio.dcs_level > 0.0);
}

/** @brief Verify that every modern non-audio option reaches the channel state. */
static void test_complete_processing_config_overrides(void)
{
	struct chan_usbradio_pvt radio = {0};
	int saved_haspp = haspp;

	settings_defaults(&settings);
	add_complete_processing_override_fixture();
	assert(settings.profiles[0].override_count < MAX_SECTION_OVERRIDES);
	assert(apply_processing_config_overrides(&radio, "usb") == 0);
	assert(!strcmp(radio.devstr, "usb-complete"));
	assert(!strcmp(radio.serial, "serial-complete"));
	assert(radio.hdwtype == 1 && radio.wanteeprom && radio.frags == 4 && radio.queuesize == 8);
	assert(radio.rxcpusaver && radio.txcpusaver && radio.rxpolarity && radio.txpolarity);
	assert(radio.rxdemod == RX_AUDIO_FLAT && radio.rxcdtype == CD_XPMR_NOISE);
	assert(radio.voxhangtime == 100 && radio.rxsqvoxadj == 20 && radio.rxsqhyst == 2);
	assert(radio.rxnoisefiltype == 1 && radio.rxsquelchdelay == 10 &&
	       radio.rxsquelchadj == 450);
	assert(radio.rxfreq == 146520000 && radio.txfreq == 146520000);
	assert(radio.rxctcssoverride && radio.rxctcssrelax == 1);
	assert(!strcmp(radio.txctcssdefault, "100.0"));
	assert(radio.ctcss_tail_duration_ms == 100 && radio.dcs_turnoff_enabled);
	assert(radio.dcs_turnoff_duration_ms == 180 && radio.dcs_level > 0.0);
	assert(radio.radioduplex == 1 && radio.duplex3 == 999);
	assert(radio.duplex3mode == DUPLEX3_MODE_SOFTWARE && radio.radioactive);
	assert(radio.tracetype == 1 && radio.tracelevel == 2 && radio.fever == 3);
	assert(radio.gpios[0] && radio.pps[2]);
	clear_processing_assignment_fixture(&radio);
	haspp = saved_haspp;
}

/** @brief Verify each typed modern channel setting rejects malformed text. */
static void test_complete_processing_config_override_rejections(void)
{
	static const struct {
		const char *section;
		const char *name;
	} integer_options[] = {
		{"hardware", "hardware_interface_type"},
		{"hardware", "hardware_audio_fragment_count"},
		{"hardware", "hardware_audio_queue_size"},
		{"receive", "vox_hang_ms"},
		{"receive", "vox_threshold"},
		{"receive", "noise_squelch_hysteresis"},
		{"receive", "noise_filter_type"},
		{"receive", "squelch_delay_ms"},
		{"receive", "on_delay_frames"},
		{"receive", "squelch_level"},
		{"ctcss", "receive_relax"},
		{"ctcss", "tail_duration_ms"},
		{"dcs", "turnoff_duration_ms"},
		{"transmit", "settle_ms"},
		{"transmit", "rx_blanking_ms"},
		{"transmit", "off_delay_frames"},
		{"receive", "frequency_hz"},
		{"transmit", "frequency_hz"},
		{"hardware", "hardware_repeater_number"},
		{"hardware", "hardware_area"},
		{"hardware", "hardware_idle_interval"},
		{"hardware", "hardware_turnoff_count"},
		{"hardware", "hardware_voter_reporting"},
		{"hardware", "hardware_clip_led_gpio"},
		{"duplex", "duplex_radio_mode"},
		{"duplex", "duplex_local_repeat_level"},
		{"diagnostics", "diagnostics_trace_type"},
		{"diagnostics", "diagnostics_trace_level"},
		{"diagnostics", "diagnostics_fever"},
	};
	static const struct {
		const char *section;
		const char *name;
	} boolean_options[] = {
		{"hardware", "hardware_eeprom_enabled"}, {"receive", "cpu_saver_enabled"},
		{"transmit", "cpu_saver_enabled"},	 {"receive", "polarity_inverted"},
		{"ctcss", "receive_override_enabled"},	 {"dcs", "turnoff_code_enabled"},
		{"receive", "lsd_polarity_inverted"},	 {"transmit", "lsd_polarity_inverted"},
		{"transmit", "preemphasis_enabled"},	 {"transmit", "polarity_inverted"},
		{"hardware", "hardware_ptt_inverted"},	 {"general", "channel_enabled"},
	};
	static const struct {
		const char *section;
		const char *name;
	} floating_options[] = {
		{"ctcss", "receive_decoder_gain_db"},
		{"ctcss", "transmit_peak_dbfs"},
		{"ctcss", "phase_shift_degrees"},
		{"ctcss", "tail_frequency_hz"},
		{"dcs", "peak_dbfs"},
		{"hardware", "hardware_deemphasis_corner_hz"},
		{"hardware", "hardware_preemphasis_corner_hz"},
	};
	static const char *const malformed_integer[] = {"bad", "1x"};
	static const char *const malformed_floating[] = {"bad", "1x", "nan"};
	struct chan_usbradio_pvt radio;
	size_t index, value;

	for (index = 0; index < ARRAY_LEN(integer_options); ++index) {
		for (value = 0; value < ARRAY_LEN(malformed_integer); ++value) {
			memset(&radio, 0, sizeof(radio));
			settings_defaults(&settings);
			add_processing_override(integer_options[index].section,
						integer_options[index].name,
						malformed_integer[value]);
			assert(apply_processing_config_overrides(&radio, "usb") == -1);
		}
	}
	for (index = 0; index < ARRAY_LEN(boolean_options); ++index) {
		memset(&radio, 0, sizeof(radio));
		settings_defaults(&settings);
		add_processing_override(boolean_options[index].section, boolean_options[index].name,
					"not-a-boolean");
		assert(apply_processing_config_overrides(&radio, "usb") == -1);
	}
	for (index = 0; index < ARRAY_LEN(boolean_options); ++index) {
		memset(&radio, 0, sizeof(radio));
		settings_defaults(&settings);
		add_processing_override(boolean_options[index].section, boolean_options[index].name,
					"no");
		assert(apply_processing_config_overrides(&radio, "usb") == 0);
	}
	for (index = 0; index < ARRAY_LEN(floating_options); ++index) {
		for (value = 0; value < ARRAY_LEN(malformed_floating); ++value) {
			memset(&radio, 0, sizeof(radio));
			settings_defaults(&settings);
			add_processing_override(floating_options[index].section,
						floating_options[index].name,
						malformed_floating[value]);
			assert(apply_processing_config_overrides(&radio, "usb") == -1);
		}
	}
	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("ctcss", "tail_duration_ms", "40000");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	settings_defaults(&settings);
}

/** @brief Verify rejected signaling overrides do not escape their test fixture. */
static void test_signaling_override_rejections(void)
{
	static const struct {
		const char *section;
		const char *name;
		const char *value;
	} cases[] = {
		{"ctcss", "transmit_peak_dbfs", "1"},
		{"ctcss", "transmit_peak_dbfs", "-91"},
		{"ctcss", "transmit_default_hz", "0"},
		{"ctcss", "phase_shift_degrees", "bad"},
		{"ctcss", "phase_shift_degrees", "0"},
		{"ctcss", "tail_duration_ms", "0"},
		{"ctcss", "tail_frequency_hz", "0"},
		{"dcs", "peak_dbfs", "1"},
		{"dcs", "peak_dbfs", "-91"},
		{"dcs", "turnoff_duration_ms", "149"},
		{"dcs", "turnoff_duration_ms", "201"},
		{"dcs", "receive_code", "123X"},
		{"dcs", "transmit_code", "888N"},
		{"receive", "signaling_method", "invalid"},
		{"transmit", "signaling_method", "invalid"},
	};
	struct chan_usbradio_pvt radio = {0};
	size_t index;

	for (index = 0; index < ARRAY_LEN(cases); ++index) {
		memset(&radio, 0, sizeof(radio));
		settings_defaults(&settings);
		add_processing_override(cases[index].section, cases[index].name,
					cases[index].value);
		assert(apply_processing_config_overrides(&radio, "usb") == -1);
	}
	static const struct {
		const char *assignment;
		enum radio_carrier_detect expected;
	} carrier_cases[] = {
		{"usb", CD_HID},	{"usbinvert", CD_HID_INVERT},
		{"dsp", CD_XPMR_NOISE}, {"vox", CD_XPMR_VOX},
		{"pp", CD_PP},		{"ppinvert", CD_PP_INVERT},
		{"no", CD_IGNORE},
	};
	for (index = 0; index < ARRAY_LEN(carrier_cases); ++index) {
		memset(&radio, 0, sizeof(radio));
		settings_defaults(&settings);
		add_processing_override("receive", "cos_assignment",
					carrier_cases[index].assignment);
		assert(apply_processing_config_overrides(&radio, "usb") == 0);
		assert(radio.rxcdtype == carrier_cases[index].expected);
	}
}

/** @brief Reject selected signaling methods that do not have complete inputs. */
static void test_selected_signaling_requirements(void)
{
	struct chan_usbradio_pvt radio = {0};

	settings_defaults(&settings);
	add_processing_override("receive", "signaling_method", "ctcss");
	add_processing_override("ctcss", "receive_source", "no");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);

	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("receive", "signaling_method", "ctcss");
	add_processing_override("ctcss", "receive_source", "dsp");
	add_processing_override("ctcss", "receive_frequencies", "100.0");
	add_processing_override("ctcss", "transmit_frequencies", "");
	/* Receive-only CTCSS does not need a transmit translation map. */
	assert(apply_processing_config_overrides(&radio, "usb") == 0);

	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("receive", "signaling_method", "ctcss");
	add_processing_override("transmit", "signaling_method", "ctcss");
	add_processing_override("ctcss", "receive_source", "dsp");
	add_processing_override("ctcss", "receive_frequencies", "100.0,123.0");
	add_processing_override("ctcss", "transmit_frequencies", "100.0");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);

	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("receive", "signaling_method", "ctcss");
	add_processing_override("ctcss", "receive_source", "dsp");
	add_processing_override("ctcss", "receive_frequencies", "49.0");
	add_processing_override("ctcss", "transmit_frequencies", "100.0");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);

	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("receive", "signaling_method", "ctcss");
	add_processing_override("ctcss", "receive_source", "dsp");
	add_processing_override("ctcss", "receive_frequencies", "100.0");
	add_processing_override("ctcss", "transmit_frequencies", "123.0");
	assert(apply_processing_config_overrides(&radio, "usb") == 0);

	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("transmit", "signaling_method", "ctcss");
	add_processing_override("ctcss", "transmit_default_hz", "");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);

	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("transmit", "signaling_method", "ctcss");
	add_processing_override("ctcss", "transmit_default_hz", "49.0");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);

	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("transmit", "signaling_method", "ctcss");
	add_processing_override("ctcss", "transmit_default_hz", "100.0");
	assert(apply_processing_config_overrides(&radio, "usb") == 0);

	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("receive", "signaling_method", "dcs");
	add_processing_override("dcs", "receive_code", "");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);

	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("receive", "signaling_method", "dcs");
	add_processing_override("dcs", "receive_code", "000N");
	assert(apply_processing_config_overrides(&radio, "usb") == 0);

	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("transmit", "signaling_method", "dcs");
	add_processing_override("dcs", "transmit_code", "");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);

	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("transmit", "signaling_method", "dcs");
	add_processing_override("dcs", "transmit_code", "777I");
	assert(apply_processing_config_overrides(&radio, "usb") == 0);

	/* DCS and CTCSS both use the assigned signaling output.  Reject a
	 * transmit selection which would otherwise load but emit no signaling. */
	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	settings.profiles[0].hardware.output_a_assignment = USBRADIOPLUS_HW_VOICE;
	settings.profiles[0].hardware.output_b_assignment = USBRADIOPLUS_HW_OFF;
	add_processing_override("transmit", "signaling_method", "dcs");
	add_processing_override("dcs", "transmit_code", "023N");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
}

/** @brief Verify receive and transmit signaling methods remain independently selectable. */
static void test_independent_signaling_directions(void)
{
	static const char *const methods[] = {"carrier", "ctcss", "dcs"};
	int saved_haspp = haspp;
	size_t receive_index;
	size_t transmit_index;

	/* Every direction combination must reach the native engine.  In particular,
	 * TX CTCSS cannot depend on a paired RX CTCSS translation list. */
	for (receive_index = 0; receive_index < ARRAY_LEN(methods); ++receive_index) {
		for (transmit_index = 0; transmit_index < ARRAY_LEN(methods); ++transmit_index) {
			struct chan_usbradio_pvt radio = {.name = "usb"};
			urp_radio_state template = {
				.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};

			settings_defaults(&settings);
			add_processing_override("receive", "signaling_method",
						methods[receive_index]);
			add_processing_override("transmit", "signaling_method",
						methods[transmit_index]);
			/* This must be inert unless receive CTCSS is selected. */
			add_processing_override("ctcss", "receive_override_enabled", "yes");
			radio.radio = urp_radio_create(&template, SAMPLES_PER_BLOCK);
			assert(radio.radio);
			assert(!apply_processing_config_overrides(&radio, "usb"));
			assert(!radio_config(&radio));
			assert(radio.radio->b.ctcssRxEnable == (receive_index == 1));
			assert(radio.radio->b.ctcssTxEnable == (transmit_index == 1));
			assert(radio.radio->dcs.enabled_receive == (receive_index == 2));
			assert(radio.radio->dcs.enabled_transmit == (transmit_index == 2));
			assert(radio.rxctcssoverride == (receive_index == 1));
			assert((radio.radio->txcodedefaultsmode == SMODE_CTCSS) ==
			       (transmit_index == 1));
			assert(!urp_radio_destroy(radio.radio));
		}
	}
	haspp = saved_haspp;
}

/** @brief Preserve a latent CTCSS source when another receive method is active. */
static void test_signaling_override_commit_and_tuning_save(void)
{
	static const char *const inactive_methods[] = {"carrier", "dcs"};
	struct ast_config *saved_config_load_result = test_config_load_result;
	struct ast_category *saved_category_get_result = test_category_get_result;
	struct ast_variable *saved_config_variables = test_config_variables;
	int saved_variable_update_result = variable_update_result;
	int saved_config_save_result = config_save_result;
	size_t index;

	for (index = 0; index < ARRAY_LEN(inactive_methods); ++index) {
		struct ast_variable existing = {.name = "receive_source", .value = "no"};
		struct chan_usbradio_pvt radio = {.name = "usb"};

		settings_defaults(&settings);
		add_processing_override("receive", "signaling_method", inactive_methods[index]);
		add_processing_override("ctcss", "receive_source", "usb");
		/* The direct loader must report success after it has committed the complete
		 * candidate, even though CTCSS is inactive in this receive direction. */
		assert(!apply_processing_signaling_overrides(&radio, radio.name));
		assert(radio.rxsdtype == SD_IGNORE);

		memset(updated_variable_name, 0, sizeof(updated_variable_name));
		memset(updated_variable_value, 0, sizeof(updated_variable_value));
		test_config_load_result = (struct ast_config *)(uintptr_t)1;
		test_category_get_result = (struct ast_category *)(uintptr_t)1;
		test_config_variables = &existing;
		variable_update_result = 0;
		config_save_result = 0;
		assert(!save_tuning_config(&radio));
		assert(!strcmp(updated_variable_name, "receive_source"));
		assert(!strcmp(updated_variable_value, "usb"));
	}
	{
		struct ast_variable existing = {.name = "receive_source", .value = "not-a-source"};
		struct chan_usbradio_pvt radio = {.name = "usb", .rxsdtype = SD_XPMR};

		settings_defaults(&settings);
		add_processing_override("ctcss", "receive_source", "not-a-source");
		memset(updated_variable_name, 0, sizeof(updated_variable_name));
		memset(updated_variable_value, 0, sizeof(updated_variable_value));
		test_config_load_result = (struct ast_config *)(uintptr_t)1;
		test_category_get_result = (struct ast_category *)(uintptr_t)1;
		test_config_variables = &existing;
		variable_update_result = 0;
		config_save_result = 0;
		assert(!save_tuning_config(&radio));
		assert(!strcmp(updated_variable_name, "receive_source"));
		assert(!strcmp(updated_variable_value, "dsp"));
	}
	test_config_load_result = saved_config_load_result;
	test_category_get_result = saved_category_get_result;
	test_config_variables = saved_config_variables;
	variable_update_result = saved_variable_update_result;
	config_save_result = saved_config_save_result;
}

/** @brief Append one override to a selected synthetic processing profile. */
static void add_profile_processing_override(size_t profile, const char *section, const char *name,
					    const char *value)
{
	struct section_override *entry;

	assert(profile < settings.profile_count);
	assert(settings.profiles[profile].override_count < MAX_SECTION_OVERRIDES);
	entry = &settings.profiles[profile].overrides[settings.profiles[profile].override_count++];
	ast_copy_string(entry->section, section, sizeof(entry->section));
	ast_copy_string(entry->name, name, sizeof(entry->name));
	ast_copy_string(entry->value, value, sizeof(entry->value));
}

/** @brief Reject a multi-channel reload before it mutates any earlier channel. */
static void test_signaling_reload_preflight_transaction(void)
{
	struct chan_usbradio_pvt first = {.name = "first"};
	struct chan_usbradio_pvt second = {.name = "second"};
	struct chan_usbradio_pvt *saved_channels = usbradio_default.next;
	urp_radio_state template = {.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	int saved_haspp = haspp;

	/* Establish two known carrier-only live engines before presenting a candidate
	 * whose second profile selects signaling without a hardware tone route. */
	settings_defaults(&settings);
	ast_copy_string(settings.profiles[0].name, first.name, sizeof(settings.profiles[0].name));
	ast_copy_string(settings.profiles[0].channel, "RadioPlus/first",
			sizeof(settings.profiles[0].channel));
	first.radio = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(first.radio);
	assert(!apply_processing_config_overrides(&first, first.name));
	assert(!radio_config(&first));

	settings_defaults(&settings);
	ast_copy_string(settings.profiles[0].name, second.name, sizeof(settings.profiles[0].name));
	ast_copy_string(settings.profiles[0].channel, "RadioPlus/second",
			sizeof(settings.profiles[0].channel));
	second.radio = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(second.radio);
	assert(!apply_processing_config_overrides(&second, second.name));
	assert(!radio_config(&second));

	settings_defaults(&settings);
	settings.profiles[1] = settings.profiles[0];
	settings.profile_count = 2;
	ast_copy_string(settings.profiles[0].name, first.name, sizeof(settings.profiles[0].name));
	ast_copy_string(settings.profiles[0].channel, "RadioPlus/first",
			sizeof(settings.profiles[0].channel));
	ast_copy_string(settings.profiles[1].name, second.name, sizeof(settings.profiles[1].name));
	ast_copy_string(settings.profiles[1].channel, "RadioPlus/second",
			sizeof(settings.profiles[1].channel));
	add_profile_processing_override(0, "receive", "signaling_method", "ctcss");
	add_profile_processing_override(1, "transmit", "signaling_method", "dcs");
	add_profile_processing_override(1, "dcs", "transmit_code", "023N");
	settings.profiles[1].hardware.output_a_assignment = USBRADIOPLUS_HW_VOICE;
	settings.profiles[1].hardware.output_b_assignment = USBRADIOPLUS_HW_OFF;
	first.next = &second;
	usbradio_default.next = &first;

	assert(usbradioplus_refresh_all_processing_signaling() == -1);
	assert(!strcmp(first.receive_signaling_method, "carrier"));
	assert(!first.radio->b.ctcssRxEnable);
	assert(!strcmp(second.receive_signaling_method, "carrier"));
	assert(!second.radio->b.ctcssRxEnable);

	usbradio_default.next = saved_channels;
	assert(!urp_radio_destroy(first.radio));
	assert(!urp_radio_destroy(second.radio));
	haspp = saved_haspp;
}

static void add_profile_processing_override(size_t profile, const char *section, const char *name,
					    const char *value);

/** @brief Apply a successful multi-channel signaling reload without touching a radio-less entry. */
static void test_signaling_reload_commit_transaction(void)
{
	struct chan_usbradio_pvt first = {.name = "first"};
	struct chan_usbradio_pvt second = {.name = "second"};
	struct chan_usbradio_pvt no_radio = {.name = "no-radio"};
	struct chan_usbradio_pvt *saved_channels = usbradio_default.next;
	urp_radio_state template = {.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};

	settings_defaults(&settings);
	settings.profiles[1] = settings.profiles[0];
	settings.profile_count = 2;
	ast_copy_string(settings.profiles[0].name, first.name, sizeof(settings.profiles[0].name));
	ast_copy_string(settings.profiles[0].channel, "RadioPlus/first",
			sizeof(settings.profiles[0].channel));
	ast_copy_string(settings.profiles[1].name, second.name, sizeof(settings.profiles[1].name));
	ast_copy_string(settings.profiles[1].channel, "RadioPlus/second",
			sizeof(settings.profiles[1].channel));
	add_profile_processing_override(0, "receive", "signaling_method", "ctcss");
	add_profile_processing_override(0, "transmit", "signaling_method", "ctcss");
	add_profile_processing_override(1, "receive", "signaling_method", "dcs");
	add_profile_processing_override(1, "transmit", "signaling_method", "dcs");

	first.radio = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	second.radio = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(first.radio && second.radio);
	first.next = &no_radio;
	no_radio.next = &second;
	usbradio_default.next = &first;

	/* The preflight pass resolves all live profiles before committing either one;
	 * a radio-less channel is intentionally skipped in both passes. */
	assert(!usbradioplus_refresh_all_processing_signaling());
	assert(!strcmp(first.receive_signaling_method, "ctcss"));
	assert(!strcmp(first.transmit_signaling_method, "ctcss"));
	assert(first.radio->b.ctcssRxEnable && first.radio->b.ctcssTxEnable);
	assert(!strcmp(second.receive_signaling_method, "dcs"));
	assert(!strcmp(second.transmit_signaling_method, "dcs"));
	assert(second.radio->dcs.enabled_receive && second.radio->dcs.enabled_transmit);

	usbradio_default.next = saved_channels;
	assert(!urp_radio_destroy(first.radio));
	assert(!urp_radio_destroy(second.radio));
	settings_defaults(&settings);
}

/** @brief Verify both defensive failure exits of the second signaling-reload pass. */
static void test_signaling_reload_second_pass_failures(void)
{
	struct chan_usbradio_pvt channel = {.name = "refresh-failures"};
	struct chan_usbradio_pvt *saved_channels = usbradio_default.next;
	urp_radio_state template = {.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	size_t resolver_option_count;

	settings_defaults(&settings);
	ast_copy_string(settings.profiles[0].name, channel.name, sizeof(settings.profiles[0].name));
	ast_copy_string(settings.profiles[0].channel, "RadioPlus/refresh-failures",
			sizeof(settings.profiles[0].channel));
	channel.radio = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(channel.radio);
	processing_option_get_calls = 0;
	fail_processing_option_get_call = 0;
	assert(!apply_processing_signaling_overrides(&channel, channel.name));
	resolver_option_count = (size_t)processing_option_get_calls;
	assert(resolver_option_count > 0U);
	usbradio_default.next = &channel;

	/* The first pass consumes exactly one complete candidate. Failing the next
	 * option read proves the defensive second-pass resolver rejects before a
	 * partially updated parser can be published. */
	processing_option_get_calls = 0;
	fail_processing_option_get_call = (int)(resolver_option_count + 1U);
	assert(usbradioplus_refresh_all_processing_signaling() == -1);
	fail_processing_option_get_call = 0;

	/* Candidate parsing is now clean, but rebuilding the legacy code maps can
	 * still fail. The control-plane gate must be released before refresh returns. */
	ast_calloc_calls = 0;
	fail_ast_calloc_call = 1;
	assert(usbradioplus_refresh_all_processing_signaling() == -1);
	fail_ast_calloc_call = 0;
	assert(!atomic_load_explicit(&channel.plus_radio_access.reconfiguring,
				     memory_order_relaxed));

	usbradio_default.next = saved_channels;
	assert(!urp_radio_destroy(channel.radio));
	settings_defaults(&settings);
}

/** @brief Cover syntax and parser-held state edges before a signaling reload commits. */
static void test_signaling_parser_helper_edges(void)
{
	struct chan_usbradio_pvt radio = {.name = "usb"};
	struct chan_usbradio_pvt missing_radio = {.name = "missing-radio"};
	struct chan_usbradio_pvt no_radio = {.name = "no-radio"};
	double frequency = 0.0;
	size_t count = 0;
	int code = urp_ctcss_frequency_index(100.0F);

	assert(code >= 0);
	assert(usbradioplus_test_radio_config_locked(&missing_radio) == 1);
	assert(!set_txctcss_level(NULL));
	refresh_processing_hardware(NULL);
	refresh_processing_hardware(&no_radio);
	usbradioplus_parrot_rx_transition(NULL, 0);
	no_radio.plus_native_renderer = (void *)(uintptr_t)1;
	usbradioplus_parrot_rx_transition(&no_radio, 1);
	no_radio.plus_native_renderer = NULL;
	atomic_init(&no_radio.echoing, 0);
	no_radio.rxkeyed = 0;
	usbradioplus_parrot_rx_transition(&no_radio, 0);
	no_radio.plus_parrot_state.count = 1;
	usbradioplus_parrot_rx_transition(&no_radio, 1);
	assert(atomic_load_explicit(&no_radio.echoing, memory_order_relaxed));
	mock_oss_io = 1;
	mock_write_result = -1;
	radio.pttkick[1] = 1;
	errno = EAGAIN;
	kickptt(&radio);
	mock_write_result = -2;
	mock_oss_io = 0;

	assert(!usbradioplus_test_ctcss_frequency_list_valid(NULL, &count));
	assert(!usbradioplus_test_ctcss_frequency_list_valid("", &count));
	assert(!usbradioplus_test_ctcss_frequency_list_valid("invalid", &count));
	assert(!usbradioplus_test_ctcss_frequency_list_valid("1e999", &count));
	assert(!usbradioplus_test_ctcss_frequency_list_valid("nan", &count));
	assert(!usbradioplus_test_ctcss_frequency_list_valid("49.0", &count));
	assert(!usbradioplus_test_ctcss_frequency_list_valid("100.0x", &count));
	assert(!usbradioplus_test_ctcss_frequency_list_valid("100.0,", &count));
	assert(!usbradioplus_test_ctcss_frequency_list_valid("100.0, \t", &count));
	assert(usbradioplus_test_ctcss_frequency_list_valid("100.0 \t,123.0", &count));
	assert(usbradioplus_test_ctcss_frequency_list_valid("100.0,\t123.0", &count));
	assert(count == 2U);
	assert(usbradioplus_test_ctcss_frequency_lists_mapped("100.0,123.0", "100.0,123.0"));
	assert(!usbradioplus_test_ctcss_frequency_lists_mapped("100.0,123.0", "100.0"));
	assert(!usbradioplus_test_ctcss_frequency_lists_mapped("invalid", "100.0"));
	assert(!usbradioplus_test_ctcss_frequency_lists_mapped("100.0", "invalid"));
	assert(usbradioplus_test_ctcss_frequency_valid("100.0"));
	assert(!usbradioplus_test_ctcss_frequency_valid("100.0,123.0"));

	assert(!usbradioplus_test_dcs_code_valid(NULL));
	assert(!usbradioplus_test_dcs_code_valid("123"));
	assert(!usbradioplus_test_dcs_code_valid("823N"));
	assert(!usbradioplus_test_dcs_code_valid("-23N"));
	assert(!usbradioplus_test_dcs_code_valid("183N"));
	assert(!usbradioplus_test_dcs_code_valid("1-3N"));
	assert(!usbradioplus_test_dcs_code_valid("128N"));
	assert(!usbradioplus_test_dcs_code_valid("12-N"));
	assert(!usbradioplus_test_dcs_code_valid("123X"));
	assert(usbradioplus_test_dcs_code_valid("123N"));
	assert(usbradioplus_test_dcs_code_valid("123n"));
	assert(usbradioplus_test_dcs_code_valid("123I"));
	assert(usbradioplus_test_dcs_code_valid("123i"));

	assert(!usbradioplus_test_native_ctcss_code_frequency(NULL, code, &frequency));
	assert(!usbradioplus_test_native_ctcss_code_frequency("100.0", code, NULL));
	assert(!usbradioplus_test_native_ctcss_code_frequency("100.0", -1, &frequency));
	assert(!usbradioplus_test_native_ctcss_code_frequency("100.0", CTCSS_NUM_CODES,
							      &frequency));
	assert(!usbradioplus_test_native_ctcss_code_frequency("invalid", code, &frequency));
	assert(!usbradioplus_test_native_ctcss_code_frequency("123.0;100.0", code, &frequency));
	assert(!usbradioplus_test_native_ctcss_code_frequency("123.0,", code, &frequency));
	assert(usbradioplus_test_native_ctcss_code_frequency("123.0 \t, 100.0", code, &frequency));
	assert(usbradioplus_test_native_ctcss_code_frequency("123.0,\t100.0", code, &frequency));
	assert(usbradioplus_test_native_ctcss_code_frequency("123.0, 100.0", code, &frequency));
	assert(fabs(frequency - 100.0) < 0.001);

	settings_defaults(&settings);
	assert(apply_processing_signaling_overrides(NULL, "usb") == -1);
	assert(apply_processing_signaling_overrides(&radio, NULL) == -1);
	assert(apply_processing_signaling_overrides(&radio, "missing-profile") == -1);
	assert(usbradioplus_test_resolve_processing_signaling(NULL, 0) == -1);
	assert(usbradioplus_test_resolve_processing_signaling("usb", 1) == -1);
	assert(usbradioplus_test_radio_program_snapshot_retry());
	assert(!usbradioplus_test_radio_access_contention_paths());
	assert(!usbradioplus_test_native_graph_slot_contention_paths());
	settings_defaults(&settings);
	add_processing_override("receive", "cpu_saver_enabled", "invalid");
	assert(apply_processing_signaling_overrides(&radio, "usb") == -1);
	settings_defaults(&settings);
	add_processing_override("receive", "vox_hang_ms", "invalid");
	assert(apply_processing_signaling_overrides(&radio, "usb") == -1);
	settings_defaults(&settings);
	add_processing_override("ctcss", "receive_decoder_gain_db", "nan");
	assert(apply_processing_signaling_overrides(&radio, "usb") == -1);
	settings_defaults(&settings);
}

/** @brief Exercise every resolved signaling-option failure without publishing a partial profile. */
static void test_processing_signaling_resolution_failure_paths(void)
{
	static const struct {
		const char *section;
		const char *name;
		const char *value;
	} malformed[] = {
		{"receive", "signaling_method", "invalid"},
		{"transmit", "signaling_method", "invalid"},
		{"receive", "cpu_saver_enabled", "invalid"},
		{"receive", "vox_hang_ms", "bad"},
		{"receive", "vox_hang_ms", "1x"},
		{"receive", "audio_source", "invalid"},
		{"receive", "cos_assignment", "invalid"},
		{"ctcss", "receive_source", "invalid"},
		{"ctcss", "receive_frequencies", "49.0"},
		{"ctcss", "transmit_frequencies", "49.0"},
		{"ctcss", "transmit_default_hz", "49.0"},
		{"ctcss", "receive_decoder_gain_db", "bad"},
		{"ctcss", "receive_decoder_gain_db", "1x"},
		{"ctcss", "receive_decoder_gain_db", "nan"},
		{"ctcss", "transmit_peak_dbfs", "nan"},
		{"ctcss", "turnoff_mode", "invalid"},
		{"ctcss", "phase_shift_degrees", "nan"},
		{"ctcss", "tail_duration_ms", "bad"},
		{"ctcss", "tail_frequency_hz", "nan"},
		{"dcs", "receive_code", "888N"},
		{"dcs", "transmit_code", "888N"},
		{"dcs", "peak_dbfs", "nan"},
	};
	struct chan_usbradio_pvt radio = {.name = "usb"};
	size_t call_count;
	size_t index;

	/* Make each otherwise-optional read unavailable in turn.  The resolver must
	 * reject the candidate before commit, regardless of which field is absent. */
	settings_defaults(&settings);
	processing_option_get_calls = 0;
	fail_processing_option_get_call = 0;
	assert(!apply_processing_signaling_overrides(&radio, radio.name));
	call_count = (size_t)processing_option_get_calls;
	assert(call_count > 0U);
	for (index = 1; index <= call_count; ++index) {
		memset(&radio, 0, sizeof(radio));
		radio.name = "usb";
		settings_defaults(&settings);
		processing_option_get_calls = 0;
		fail_processing_option_get_call = (int)index;
		assert(apply_processing_signaling_overrides(&radio, radio.name) == -1);
	}
	fail_processing_option_get_call = 0;

	memset(&radio, 0, sizeof(radio));
	radio.name = "usb";
	settings_defaults(&settings);
	processing_hardware_get_calls = 0;
	fail_processing_hardware_get_call = 1;
	assert(apply_processing_signaling_overrides(&radio, radio.name) == -1);
	fail_processing_hardware_get_call = 0;

	for (index = 0; index < ARRAY_LEN(malformed); ++index) {
		memset(&radio, 0, sizeof(radio));
		radio.name = "usb";
		settings_defaults(&settings);
		add_processing_override(malformed[index].section, malformed[index].name,
					malformed[index].value);
		assert(apply_processing_signaling_overrides(&radio, radio.name) == -1);
	}

	/* Inactive CTCSS controls may be empty. Resolve those values without treating
	 * an empty list as a malformed latent setting. */
	memset(&radio, 0, sizeof(radio));
	radio.name = "usb";
	settings_defaults(&settings);
	add_processing_override("ctcss", "receive_frequencies", "");
	add_processing_override("ctcss", "transmit_frequencies", "");
	add_processing_override("ctcss", "transmit_default_hz", "");
	assert(!apply_processing_signaling_overrides(&radio, radio.name));

	/* Cross-field rules are resolved only after all individual syntax checks. */
	memset(&radio, 0, sizeof(radio));
	radio.name = "usb";
	settings_defaults(&settings);
	add_processing_override("receive", "signaling_method", "ctcss");
	add_processing_override("ctcss", "receive_source", "no");
	assert(apply_processing_signaling_overrides(&radio, radio.name) == -1);

	memset(&radio, 0, sizeof(radio));
	radio.name = "usb";
	settings_defaults(&settings);
	add_processing_override("receive", "signaling_method", "ctcss");
	add_processing_override("ctcss", "receive_source", "dsp");
	add_processing_override("ctcss", "receive_frequencies", "");
	assert(apply_processing_signaling_overrides(&radio, radio.name) == -1);

	memset(&radio, 0, sizeof(radio));
	radio.name = "usb";
	settings_defaults(&settings);
	add_processing_override("transmit", "signaling_method", "ctcss");
	add_processing_override("ctcss", "transmit_default_hz", "");
	assert(apply_processing_signaling_overrides(&radio, radio.name) == -1);

	memset(&radio, 0, sizeof(radio));
	radio.name = "usb";
	settings_defaults(&settings);
	add_processing_override("receive", "signaling_method", "ctcss");
	add_processing_override("transmit", "signaling_method", "ctcss");
	add_processing_override("ctcss", "receive_source", "dsp");
	add_processing_override("ctcss", "receive_frequencies", "100.0,123.0");
	add_processing_override("ctcss", "transmit_frequencies", "100.0");
	assert(apply_processing_signaling_overrides(&radio, radio.name) == -1);

	memset(&radio, 0, sizeof(radio));
	radio.name = "usb";
	settings_defaults(&settings);
	settings.profiles[0].hardware.output_a_assignment = USBRADIOPLUS_HW_VOICE;
	settings.profiles[0].hardware.output_b_assignment = USBRADIOPLUS_HW_OFF;
	add_processing_override("transmit", "signaling_method", "dcs");
	add_processing_override("dcs", "transmit_code", "023N");
	assert(apply_processing_signaling_overrides(&radio, radio.name) == -1);
	settings_defaults(&settings);
}

/** @brief Verify optional direct-loader values may be absent without invalidating defaults. */
static void test_processing_config_missing_option_paths(void)
{
	struct chan_usbradio_pvt radio = {0};
	int saved_haspp = haspp;
	size_t call_count;
	size_t index;

	settings_defaults(&settings);
	processing_option_get_calls = 0;
	fail_processing_option_get_call = 0;
	assert(!apply_processing_config_overrides(&radio, "usb"));
	call_count = (size_t)processing_option_get_calls;
	assert(call_count > 0U);
	clear_all_processing_assignments(&radio);
	for (index = 1; index <= call_count; ++index) {
		memset(&radio, 0, sizeof(radio));
		settings_defaults(&settings);
		processing_option_get_calls = 0;
		fail_processing_option_get_call = (int)index;
		assert(!apply_processing_config_overrides(&radio, "usb"));
		clear_all_processing_assignments(&radio);
	}
	fail_processing_option_get_call = 0;
	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	processing_hardware_get_calls = 0;
	fail_processing_hardware_get_call = 1;
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	fail_processing_hardware_get_call = 0;
	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("receive", "signaling_method", "ctcss");
	add_processing_override("ctcss", "receive_source", "dsp");
	add_processing_override("ctcss", "receive_frequencies", "");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	memset(&radio, 0, sizeof(radio));
	settings_defaults(&settings);
	add_processing_override("ctcss", "receive_frequencies", "");
	add_processing_override("ctcss", "transmit_frequencies", "49.0");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	haspp = saved_haspp;
	settings_defaults(&settings);
}

/** @brief Exercise live parser-state reset and failure paths without a native callback. */
static void test_radio_config_parser_failure_paths(void)
{
	struct chan_usbradio_pvt missing_radio = {.name = "missing-radio"};
	struct chan_usbradio_pvt channel = {.name = "parser-failures"};
	urp_radio_state template = {.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	i16 *saved_squelch_adjust;
	i32 *saved_ctcss_adjust;

	assert(radio_config(NULL) == 1);
	assert(radio_config(&missing_radio) == 1);
	channel.radio = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(channel.radio);
	ast_copy_string(channel.receive_signaling_method, "dcs",
			sizeof(channel.receive_signaling_method));
	ast_copy_string(channel.transmit_signaling_method, "carrier",
			sizeof(channel.transmit_signaling_method));
	ast_copy_string(channel.dcs_receive_code, "888N", sizeof(channel.dcs_receive_code));
	ast_copy_string(channel.dcs_transmit_code, "023N", sizeof(channel.dcs_transmit_code));
	assert(radio_config(&channel) == 1);

	ast_copy_string(channel.receive_signaling_method, "carrier",
			sizeof(channel.receive_signaling_method));
	ast_copy_string(channel.transmit_signaling_method, "dcs",
			sizeof(channel.transmit_signaling_method));
	ast_copy_string(channel.dcs_receive_code, "023N", sizeof(channel.dcs_receive_code));
	ast_copy_string(channel.dcs_transmit_code, "888N", sizeof(channel.dcs_transmit_code));
	assert(radio_config(&channel) == 1);

	/* Disabling a selected signaling family must clear the retained legacy state
	 * after parser reconstruction, rather than leaving its previous decoder mode
	 * visible to the next native processing block. */
	ast_copy_string(channel.transmit_signaling_method, "carrier",
			sizeof(channel.transmit_signaling_method));
	ast_copy_string(channel.dcs_transmit_code, "023N", sizeof(channel.dcs_transmit_code));
	channel.radio->smode = SMODE_CTCSS;
	channel.radio->smodewas = SMODE_CTCSS;
	channel.radio->smodetimer = 1;
	assert(!radio_config(&channel));
	assert(channel.radio->smode == SMODE_NULL && channel.radio->smodewas == SMODE_NULL);
	assert(!channel.radio->smodetimer);
	channel.radio->smode = SMODE_DCS;
	channel.radio->smodewas = SMODE_DCS;
	channel.radio->smodetimer = 1;
	assert(!radio_config(&channel));
	assert(channel.radio->smode == SMODE_NULL && channel.radio->smodewas == SMODE_NULL);
	assert(!channel.radio->smodetimer);
	/* A selected DCS transmitter carries the configured tail code and calibration;
	 * a later carrier selection must clear only the decoder's retained DCS state. */
	ast_copy_string(channel.transmit_signaling_method, "dcs",
			sizeof(channel.transmit_signaling_method));
	channel.dcs_turnoff_enabled = 1;
	channel.dcs_turnoff_duration_ms = 180;
	channel.dcs_level = 1234.0;
	assert(!radio_config(&channel));
	assert(channel.radio->dcsTurnoffEnabled);
	assert(channel.radio->dcsTurnoffDuration == 180);
	assert(fabs(channel.radio->dcsPeak - 1234.0) < 0.001);
	channel.dcs_turnoff_enabled = 0;
	assert(!radio_config(&channel));
	assert(!channel.radio->dcsTurnoffEnabled);
	saved_ctcss_adjust = channel.radio->prxCtcssAdjust;
	saved_squelch_adjust = channel.radio->prxSquelchAdjust;
	channel.radio->prxCtcssAdjust = NULL;
	channel.radio->prxSquelchAdjust = NULL;
	ast_copy_string(channel.transmit_signaling_method, "carrier",
			sizeof(channel.transmit_signaling_method));
	assert(!radio_config(&channel));
	channel.radio->prxCtcssAdjust = saved_ctcss_adjust;
	channel.radio->prxSquelchAdjust = saved_squelch_adjust;

	/* Radio-code parsing allocates temporary maps.  A failed allocation must
	 * return an error to the control plane before scalar parser state is reused. */
	ast_calloc_calls = 0;
	fail_ast_calloc_call = 1;
	assert(radio_config(&channel) == 1);
	fail_ast_calloc_call = 0;
	assert(!urp_radio_destroy(channel.radio));
}

/** @brief Verify processing override parse edges. */
static void test_processing_override_parse_edges(void)
{
	struct chan_usbradio_pvt radio = {0};
	size_t index;
	static const int integer_hardware[] = {2, 4, 5, 7, 8, 10, 11, 12, 13};
	static const int boolean_hardware[] = {3, 6, 12};
	static const char *const malformed_numbers[] = {"bad", "1x"};

	for (index = 0; index < ARRAY_LEN(integer_hardware); ++index) {
		for (size_t malformed = 0; malformed < ARRAY_LEN(malformed_numbers); ++malformed) {
			settings_defaults(&settings);
			add_processing_override("hardware",
						hardware_override_options[integer_hardware[index]],
						malformed_numbers[malformed]);
			assert(apply_processing_config_overrides(&radio, "usb") == -1);
		}
	}
	for (index = 0; index < ARRAY_LEN(boolean_hardware); ++index) {
		settings_defaults(&settings);
		add_processing_override(
			"hardware", hardware_override_options[boolean_hardware[index]], "invalid");
		assert(apply_processing_config_overrides(&radio, "usb") == -1);
	}
	for (index = 0; index < ARRAY_LEN(diagnostics_override_options); ++index) {
		for (size_t malformed = 0; malformed < ARRAY_LEN(malformed_numbers); ++malformed) {
			settings_defaults(&settings);
			add_processing_override("diagnostics", diagnostics_override_options[index],
						malformed_numbers[malformed]);
			assert(apply_processing_config_overrides(&radio, "usb") == -1);
		}
	}
	for (index = 0; index < 2; ++index) {
		for (size_t malformed = 0; malformed < ARRAY_LEN(malformed_numbers); ++malformed) {
			settings_defaults(&settings);
			add_processing_override("duplex", duplex_override_options[index],
						malformed_numbers[malformed]);
			assert(apply_processing_config_overrides(&radio, "usb") == -1);
		}
	}
	for (index = 0; index < 3; ++index) {
		static const char *const floating_names[] = {"receive_decoder_gain_db",
							     "hardware_deemphasis_corner_hz",
							     "hardware_preemphasis_corner_hz"};
		static const char *const floating_values[] = {"bad", "1x", "nan"};
		for (size_t malformed = 0; malformed < ARRAY_LEN(floating_values); ++malformed) {
			settings_defaults(&settings);
			add_processing_override(index == 0 ? "ctcss" : "hardware",
						floating_names[index], floating_values[malformed]);
			assert(apply_processing_config_overrides(&radio, "usb") == -1);
		}
	}
	settings_defaults(&settings);
	add_processing_override("general", "channel_enabled", "invalid");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
}

/** @brief Verify shared config loading. */
static void test_shared_config_loading(void)
{
	struct ast_config *valid = (struct ast_config *)(uintptr_t)0x1234;
	struct ast_variable active = {.name = "channel_enabled", .value = "yes"};
	static const char *const leading_flat_sections[] = {"receive", "transmit", "ctcss", "dcs"};
	size_t index;

	/* Flat clean-slate signaling sections can appear before a named channel.
	 * None may reach store_config() as a radio definition or become active. */
	for (index = 0; index < ARRAY_LEN(leading_flat_sections); ++index)
		assert(!usbradioplus_is_radio_channel_section(leading_flat_sections[index]));
	assert(usbradioplus_is_radio_channel_section("usb"));
	assert(!usbradioplus_is_radio_channel_section("receive usb"));
	assert(!usbradioplus_is_radio_channel_section(NULL));

	test_config_category = NULL;
	test_config_variables = NULL;
	test_config_load_result = NULL;
	assert(load_config(0) == AST_MODULE_LOAD_DECLINE);
	test_config_load_result = CONFIG_STATUS_FILEUNCHANGED;
	assert(load_config(1) == 0);
	test_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(load_config(0) == -1);

	settings_defaults(&settings);
	config_destroy_calls = 0;
	parallel_load_calls = 0;
	test_config_load_result = valid;
	assert(load_config(0) == 0);
	assert(config_destroy_calls == 1 && parallel_load_calls == 1);
	assert(!strcmp(pport, PP_PORT));
	assert(pbase == PP_IOPORT);
	test_config_category = "general";
	assert(load_config(0) == 0);
	test_config_category = "hardware usb";
	assert(load_config(0) == 0);
	test_config_category = NULL;

	settings_defaults(&settings);
	add_processing_override("hardware", "hardware_parallel_port_device", "/dev/parport-modern");
	add_processing_override("hardware", "hardware_parallel_port_base_address", "0x278");
	assert(load_config(1) == 0);
	assert(!strcmp(pport, "/dev/parport-modern"));
	assert(pbase == 0x278);
	test_config_variables = NULL;
	test_config_load_result = NULL;
	assert(reload_module() != 0);
	test_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(reload_module() == -1);
	test_config_load_result = valid;
	assert(reload_module() != 0);
	test_config_category = "usb";
	test_config_variables = &active;
	mock_dsp_available = 1;
	assert(reload_module() == 0);
	if (usbradio_default.next) {
		struct chan_usbradio_pvt *created = usbradio_default.next;
		usbradio_default.next = created->next;
		destroy_unlinked_channel(created);
	}
	mock_dsp_available = 0;
	test_config_category = NULL;
	test_config_variables = NULL;
	test_config_load_result = NULL;
}

/** @brief Verify effective processing settings. */
static void test_effective_processing_settings(void)
{
	struct chan_usbradio_pvt radio = {0};

	settings_defaults(&settings);
	radio.name = "usb";
	assert(fabs(effective_rx_input_gain_db(&radio)) < 0.0001);
	settings.profiles[0].chains[TXAGC_LOCAL].input_gain_configured = 1;
	settings.profiles[0].chains[TXAGC_LOCAL].agc.input_gain_db = 6.0;
	assert(fabs(effective_rx_input_gain_db(&radio) - 6.0) < 0.0001);
	assert(fabs(effective_rx_decoder_gain(&radio) - 0.997631F) < 0.0001F);

	radio.rxmixerset = 321;
	radio.txmixaset = 322;
	radio.txmixbset = 323;
	radio.txmixa = TX_OUT_VOICE;
	radio.txmixb = TX_OUT_LSD;
	radio.rxcdtype = CD_HID;
	assert(effective_rxmixerset(&radio) == 500);
	assert(effective_txmixaset(&radio) == 500);
	assert(effective_txmixbset(&radio) == 500);
	assert(effective_txmixa(&radio) == TX_OUT_COMPOSITE);
	assert(effective_txmixb(&radio) == TX_OUT_OFF);
	assert(effective_rxcdtype(&radio) == CD_HID);

	settings.profiles[0].hardware.input_gain_configured = 1;
	settings.profiles[0].hardware.input_gain_db = 0.0;
	settings.profiles[0].hardware.output_a_gain_configured = 1;
	settings.profiles[0].hardware.output_a_gain_db = 0.0;
	settings.profiles[0].hardware.output_b_gain_configured = 1;
	settings.profiles[0].hardware.output_b_gain_db = 6.0;
	settings.profiles[0].hardware.output_a_assignment_configured = 1;
	settings.profiles[0].hardware.output_a_assignment = TX_OUT_COMPOSITE;
	settings.profiles[0].hardware.output_b_assignment_configured = 1;
	settings.profiles[0].hardware.output_b_assignment = TX_OUT_AUX;
	assert(effective_rxmixerset(&radio) == 500);
	assert(effective_txmixaset(&radio) == 500);
	assert(effective_txmixbset(&radio) == 998);
	assert(effective_txmixa(&radio) == TX_OUT_COMPOSITE);
	assert(effective_txmixb(&radio) == TX_OUT_AUX);

	radio.rxcdtype = CD_HID;
	assert(effective_rxcdtype(&radio) == CD_HID);
	radio.rxcdtype = CD_HID_INVERT;
	assert(effective_rxcdtype(&radio) == CD_HID_INVERT);
	radio.rxcdtype = CD_XPMR_NOISE;
	assert(effective_rxcdtype(&radio) == CD_XPMR_NOISE);
	radio.rxcdtype = CD_XPMR_VOX;
	assert(effective_rxcdtype(&radio) == CD_XPMR_VOX);
	radio.rxcdtype = CD_PP;
	assert(effective_rxcdtype(&radio) == CD_PP);
	radio.rxcdtype = CD_PP_INVERT;
	assert(effective_rxcdtype(&radio) == CD_PP_INVERT);
	radio.rxcdtype = CD_IGNORE;
	assert(effective_rxcdtype(&radio) == CD_IGNORE);
}

/** @brief Verify numeric helpers. */
static void test_numeric_helpers(void)
{
	double samples[] = {-0.25, 0.5, -0.75, 0.125};
	short integer_samples[] = {0, -10, 20, INT16_MIN};

	assert(urp_gain_db_to_mixer(0.0) == 500);
	assert(urp_gain_db_to_mixer(20.0) == 999);
	assert(fabs(urp_mixer_to_gain_db(500)) < 0.0001);
	assert(urp_mixer_to_gain_db(0) < -100.0);
	assert(urp_hardware_level_multiplier(0) == 64);
	assert(urp_hardware_level_multiplier(500) > 0);
	assert(urp_hardware_level_multiplier(999) > urp_hardware_level_multiplier(500));
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
}

/** @brief Verify shared hardware layouts. */
static void test_shared_hardware_layouts(void)
{
	struct chan_usbradio_pvt radio = {0};
	int type;

	radio.name = "test";
	for (type = 0; type <= 3; ++type) {
		memset(&radio.gpios, 0, sizeof(radio.gpios));
		radio.hdwtype = type;
		radio.clipledgpio = 0;
		radio.invertptt = 0;
		assert(hidhdwconfig(&radio) == 0);
		assert(radio.hid_io_ptt != 0);
	}
	radio.hdwtype = 4;
	radio.clipledgpio = 0;
	assert(hidhdwconfig(&radio) == 0);

	radio.hdwtype = 0;
	radio.clipledgpio = GPIO_PINCOUNT;
	radio.gpios[0] = "in";
	radio.gpios[1] = "out1";
	radio.gpios[2] = "out";
	radio.gpios[3] = "out";
	radio.invertptt = 1;
	assert(hidhdwconfig(&radio) == 0);
	assert(radio.clipledgpio == 0);
	assert(radio.hid_gpio_val & (1 << 1));
	assert(radio.hid_gpio_val & radio.hid_io_ptt);
	radio.hdwtype = 2;
	radio.clipledgpio = 1;
	memset(&radio.gpios, 0, sizeof(radio.gpios));
	assert(hidhdwconfig(&radio) == 0);
	assert(radio.clipledgpio == 0);

	radio.hdwtype = 1;
	radio.clipledgpio = 1;
	memset(&radio.gpios, 0, sizeof(radio.gpios));
	radio.gpios[1] = "out";
	assert(hidhdwconfig(&radio) == 0);
	assert(radio.hid_gpio_ctl & 1);
}

/** @brief Verify shared control helpers. */
static void test_shared_control_helpers(void)
{
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state radio_state = {0};
	int kick_pipe[2];
	char kick_byte = 1;

	kickptt(NULL);
	radio.pttkick[1] = -1;
	kickptt(&radio);
	assert(pipe(kick_pipe) == 0);
	radio.pttkick[1] = kick_pipe[1];
	kickptt(&radio);
	assert(read(kick_pipe[0], &kick_byte, 1) == 1);
	assert(kick_byte == 0);
	radio.pttkick[1] = kick_pipe[0];
	kickptt(&radio);
	close(kick_pipe[0]);
	close(kick_pipe[1]);

	radio.radio = &radio_state;
	radio.txctcssadj = 200;
	radio.ctcss_level = 1234.0;
	assert(set_txctcss_level(&radio) == 0);
	assert(radio_state.txCtcssPeak == 1234.0);
	radio.radio = NULL;
	assert(set_txctcss_level(&radio) == 0);
}

/** @brief Verify shared receive signaling helpers. */
static void test_shared_receive_signaling_helpers(void)
{
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state radio_state = {0};
	urp_ctcss_decoder ctcss = {0};

	radio.radio = &radio_state;
	radio_state.rxCtcss = &ctcss;
	radio.rxhidsq = 1;
	radio.rxppsq = 1;
	radio_state.rxCarrierDetect = 1;
	assert(usbradioplus_carrier_detected(&radio, CD_HID));
	assert(!usbradioplus_carrier_detected(&radio, CD_HID_INVERT));
	assert(usbradioplus_carrier_detected(&radio, CD_XPMR_NOISE));
	assert(usbradioplus_carrier_detected(&radio, CD_XPMR_VOX));
	assert(usbradioplus_carrier_detected(&radio, CD_PP));
	assert(!usbradioplus_carrier_detected(&radio, CD_PP_INVERT));
	assert(!usbradioplus_carrier_detected(&radio, CD_IGNORE));
	radio.rxhidsq = radio.rxppsq = 0;
	radio_state.rxCarrierDetect = 0;
	assert(!usbradioplus_carrier_detected(&radio, CD_HID));
	assert(usbradioplus_carrier_detected(&radio, CD_HID_INVERT));
	assert(!usbradioplus_carrier_detected(&radio, CD_XPMR_NOISE));
	assert(!usbradioplus_carrier_detected(&radio, CD_XPMR_VOX));
	assert(!usbradioplus_carrier_detected(&radio, CD_PP));
	assert(usbradioplus_carrier_detected(&radio, CD_PP_INVERT));

	radio_state.b.ctcssRxEnable = 0;
	assert(usbradioplus_ctcss_detected(&radio));
	radio_state.b.ctcssRxEnable = 1;
	ctcss.decode = CTCSS_NULL;
	assert(!usbradioplus_ctcss_detected(&radio));
	ctcss.decode = CTCSS_NULL + 1;
	radio_state.smode = SMODE_CTCSS;
	assert(usbradioplus_ctcss_detected(&radio));
	radio_state.smode = 0;
	assert(!usbradioplus_ctcss_detected(&radio));
	radio_state.dcs.enabled_receive = 1;
	radio_state.dcs.valid = 1;
	radio_state.smode = SMODE_DCS;
	assert(usbradioplus_ctcss_detected(&radio));
	radio_state.smode = 0;
	assert(!usbradioplus_ctcss_detected(&radio));
	radio_state.dcs.valid = 0;
	assert(!usbradioplus_ctcss_detected(&radio));
	radio_state.dcs.enabled_receive = 0;
	radio.name = "test";
	strcpy(radio_state.rxctcssfreq, "100.0");
	radio.rxctcssdecode = 0;
	radio_state.b.ctcssRxEnable = 0;
	usbradioplus_refresh_ctcss_decode(&radio);
	assert(!radio.rxctcssdecode);
	radio_state.b.ctcssRxEnable = 1;
	ctcss.decode = 0;
	usbradioplus_refresh_ctcss_decode(&radio);
	assert(!radio.rxctcssdecode);
	ctcss.decode = 1;
	usbradioplus_refresh_ctcss_decode(&radio);
	assert(radio.rxctcssdecode == 1 && !strcmp(radio.rxctcssfreq, "100.0"));
	ast_set_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	module_debug_level = 10;
	file_debug_level = 0;
	ctcss.decode = 2;
	usbradioplus_refresh_ctcss_decode(&radio);
	module_debug_level = 0;
	file_debug_level = 10;
	ctcss.decode = 3;
	usbradioplus_refresh_ctcss_decode(&radio);
	file_debug_level = 0;
	ctcss.decode = 4;
	usbradioplus_refresh_ctcss_decode(&radio);
	ast_clear_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	option_debug = 10;
	ctcss.decode = 5;
	usbradioplus_refresh_ctcss_decode(&radio);
	option_debug = 0;
	module_debug_level = file_debug_level = 0;
}

/** @brief Verify shared eeprom wait. */
static void test_shared_eeprom_wait(void)
{
	struct chan_usbradio_pvt radio = {0};

	usbradioplus_wait_for_eeprom_idle(&radio);
	radio.eepromctl = 1;
	clear_eeprom_target = &radio;
	usbradioplus_wait_for_eeprom_idle(&radio);
	assert(!radio.eepromctl);
	clear_eeprom_target = NULL;
}

/** @brief Verify one output frame per tick through PTT and receive-state transitions. */
static void test_continuous_soundcard_output(void)
{
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state radio_state = {0};
	const short silence[FRAME_SIZE * 2 * 6] = {0};
	short output[FRAME_SIZE * 2 * 6];
	short original[FRAME_SIZE * 2 * 6];
	const int ptt_states[][2] = {{0, 0}, {0, 0}, {1, 0}, {1, 1},
				     {0, 1}, {0, 1}, {0, 0}, {0, 0}};
	unsigned int expected_writes = 0;
	size_t index;
	unsigned int receiver;

	radio.name = "continuous-output";
	radio.radio = &radio_state;
	atomic_init(&radio.plus_radio_tx_active, 0);
#ifdef URP_TEST_MODERN
	radio.pa.active = 1;
	modern_write_result = paNoError;
#else
	radio.sounddev = 7;
	radio.total_blocks = 8;
	radio.queuesize = 4;
	mock_oss_io = 1;
	mock_ioctl_failure = ULONG_MAX;
	mock_oss_fragment_size = 3840;
	mock_oss_output_space_calls = 0;
	fail_oss_output_space_call = 0;
	extreme_oss_output_space_call = 0;
	mock_oss_fragment_total = 8;
	mock_write_result = -2;
#endif
	for (index = 0; index < ARRAY_LEN(output); ++index)
		output[index] = (short)((index % 2 ? -1 : 1) * (int)(index + 1));
	memcpy(original, output, sizeof(original));
	mock_sound_write_calls = 0;
	for (receiver = 0; receiver < 8; ++receiver) {
		radio.rxcarrierdetect = receiver & 1;
		radio.rxctcssdecode = (receiver >> 1) & 1;
		radio.rxkeyed = (receiver >> 2) & 1;
		radio_state.rxCarrierDetect = radio.rxcarrierdetect;
		for (index = 0; index < ARRAY_LEN(ptt_states); ++index) {
			unsigned int tick;
			const short *expected;

			radio_state.txPttIn = ptt_states[index][0];
			radio_state.txPttOut = ptt_states[index][1];
			atomic_store_explicit(&radio.plus_radio_tx_active,
					      radio_state.txPttIn || radio_state.txPttOut,
					      memory_order_release);
			expected = radio_state.txPttIn || radio_state.txPttOut ? original : silence;
			for (tick = 0; tick < 2; ++tick) {
#ifndef URP_TEST_MODERN
				/* An empty queue must not add a second frame before this tick. */
				mock_oss_fragments = 8 - (int)tick;
#endif
				assert(soundcard_writeframe(&radio, output) == (int)sizeof(output));
				assert(mock_sound_write_calls == ++expected_writes);
				assert(mock_sound_write_bytes == sizeof(output));
				assert(!memcmp(mock_sound_write_data, expected, sizeof(output)));
				assert(!memcmp(output, original, sizeof(output)));
				assert(radio.plus_sound_dropped_frames == 0);
				assert(radio.plus_sound_short_writes == 0);
			}
		}
	}
#ifndef URP_TEST_MODERN
	mock_oss_io = 0;
#endif
}

/** @brief Verify PTT remains asserted through accepted DAC playout plus one block. */
static void test_tx_playout_hold(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state state = {0};
	short silence[4] = {0};
	short audio[4] = {0, 0, 1, 0};

	assert(!usbradioplus_pcm_has_audio(NULL, ARRAY_LEN(audio)));
	assert(!usbradioplus_pcm_has_audio(silence, ARRAY_LEN(silence)));
	assert(usbradioplus_pcm_has_audio(audio, ARRAY_LEN(audio)));
	assert(!usbradioplus_tx_playout_hold_draining(NULL));
	usbradioplus_tx_playout_hold_prepare(NULL);
	usbradioplus_tx_playout_hold_apply(NULL);
	usbradioplus_tx_playout_hold_publish(NULL);
	usbradioplus_tx_playout_hold_reset(NULL);
	usbradioplus_tx_playout_hold_note_output(NULL, 1, 1, 1);

	atomic_init(&channel.plus_radio_tx_active, 0);
	atomic_init(&channel.plus_hardware_ptt_request, 0);
	/* Teardown can publish a channel before its signaling state is attached. */
	usbradioplus_tx_playout_hold_prepare(&channel);
	usbradioplus_tx_playout_hold_apply(&channel);
	usbradioplus_tx_playout_hold_publish(&channel);
	usbradioplus_tx_playout_hold_note_output(&channel, 1, 1, 1);
	assert(!atomic_load_explicit(&channel.plus_radio_tx_active, memory_order_acquire));
	assert(!atomic_load_explicit(&channel.plus_hardware_ptt_request, memory_order_acquire));

	channel.radio = &state;
	state.txrxblankingtime = 40;

	/* A normal key publishes raw PTT and accepts audio as the timer origin. */
	state.txPttIn = state.txPttOut = 1;
	usbradioplus_tx_playout_hold_apply(&channel);
	usbradioplus_tx_playout_hold_publish(&channel);
	assert(atomic_load_explicit(&channel.plus_radio_tx_active, memory_order_acquire));
	assert(atomic_load_explicit(&channel.plus_hardware_ptt_request, memory_order_acquire));
	usbradioplus_tx_playout_hold_note_output(&channel, 0, 1, 3);
	assert(channel.plus_tx_playout_hold.callbacks_remaining == 0U);
	usbradioplus_tx_playout_hold_note_output(&channel, 1, 1, 3);
	assert(channel.plus_tx_playout_hold.callbacks_remaining == 3U);
	/* A speech pause still consumes accepted DAC time but cannot clear the
	 * deadline while the signaling engine remains keyed. */
	usbradioplus_tx_playout_hold_note_output(&channel, 1, 0, 0);
	assert(channel.plus_tx_playout_hold.callbacks_remaining == 2U);
	usbradioplus_tx_playout_hold_prepare(&channel);
	usbradioplus_tx_playout_hold_apply(&channel);
	assert(state.txPttOut && !usbradioplus_tx_playout_hold_draining(&channel));

	/* Logical unkey holds physical PTT but turns subsequent DAC blocks into silence. */
	state.txPttIn = state.txPttOut = 0;
	usbradioplus_tx_playout_hold_apply(&channel);
	assert(usbradioplus_tx_playout_hold_draining(&channel));
	assert(state.txPttOut);
	usbradioplus_tx_playout_hold_publish(&channel);
	assert(!atomic_load_explicit(&channel.plus_radio_tx_active, memory_order_acquire));
	assert(atomic_load_explicit(&channel.plus_hardware_ptt_request, memory_order_acquire));

	/* A blocked or short write never advances the post-DAC countdown. */
	usbradioplus_tx_playout_hold_note_output(&channel, 0, 0, 0);
	assert(channel.plus_tx_playout_hold.callbacks_remaining == 2U);
	usbradioplus_tx_playout_hold_note_output(&channel, 1, 0, 0);
	assert(channel.plus_tx_playout_hold.callbacks_remaining == 1U);
	usbradioplus_tx_playout_hold_prepare(&channel);
	usbradioplus_tx_playout_hold_apply(&channel);
	assert(state.txPttOut && usbradioplus_tx_playout_hold_draining(&channel));

	/* The final accepted silent block releases virtual PTT and restarts RX blanking. */
	usbradioplus_tx_playout_hold_note_output(&channel, 1, 0, 0);
	usbradioplus_tx_playout_hold_prepare(&channel);
	usbradioplus_tx_playout_hold_apply(&channel);
	usbradioplus_tx_playout_hold_publish(&channel);
	assert(!state.txPttOut && !usbradioplus_tx_playout_hold_draining(&channel));
	assert(state.txrxblankingtimer == state.txrxblankingtime);
	assert(!atomic_load_explicit(&channel.plus_hardware_ptt_request, memory_order_acquire));

	/* A ready rekey discards an obsolete tail deadline and lets new PCM arm one. */
	state.txPttIn = state.txPttOut = 1;
	usbradioplus_tx_playout_hold_apply(&channel);
	usbradioplus_tx_playout_hold_note_output(&channel, 1, 1, 2);
	state.txPttIn = state.txPttOut = 0;
	usbradioplus_tx_playout_hold_apply(&channel);
	assert(usbradioplus_tx_playout_hold_draining(&channel));
	usbradioplus_tx_playout_hold_prepare(&channel);
	state.txPttIn = state.txPttOut = 1;
	usbradioplus_tx_playout_hold_apply(&channel);
	assert(!usbradioplus_tx_playout_hold_draining(&channel));
	assert(channel.plus_tx_playout_hold.callbacks_remaining == 0U);
	usbradioplus_tx_playout_hold_note_output(&channel, 1, 1, 0);
	assert(channel.plus_tx_playout_hold.callbacks_remaining == 1U);

	state.txPttIn = state.txPttOut = 0;
	usbradioplus_tx_playout_hold_apply(&channel);
	assert(usbradioplus_tx_playout_hold_draining(&channel));
	usbradioplus_tx_playout_hold_reset(&channel);
	assert(!state.txPttOut && !channel.plus_tx_playout_hold.callbacks_remaining);
}

#ifndef URP_TEST_MODERN
/** @brief Verify oss audio helpers. */
static void test_oss_audio_helpers(void)
{
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state radio_state = {0};
	short output[FRAME_SIZE * 2 * 6] = {0};
	unsigned long failures[] = {SNDCTL_DSP_SETFMT, SNDCTL_DSP_STEREO, SNDCTL_DSP_SPEED};
	size_t index;

	mock_oss_io = 1;
	mock_ioctl_failure = ULONG_MAX;
	mock_oss_fragment_size = 3840;
	mock_oss_bytes = -1;
	mock_oss_output_delay = -1;
	mock_oss_output_space_calls = 0;
	fail_oss_output_space_call = 0;
	extreme_oss_output_space_call = 0;
	radio.name = "test";
	radio.sounddev = 7;
	radio.queuesize = 4;
	mock_oss_fragment_total = 8;
	mock_oss_fragments = 6;
	assert(used_blocks(&radio) == 2);
	assert(radio.total_blocks == 8);
	mock_oss_fragments = 4;
	assert(used_blocks(&radio) == 4);
	mock_oss_fragments = 8;
	assert(used_blocks(&radio) == 0);
	radio.total_blocks = 0;
	radio.queuesize = 8;
	mock_oss_fragment_total = 2;
	mock_oss_fragments = 1;
	assert(used_blocks(&radio) == 1);
	assert(radio.total_blocks == 2);
	assert(radio.queuesize == QUEUE_SIZE);
	radio.total_blocks = 0;
	radio.queuesize = 8;
	mock_oss_fragment_total = 8;
	mock_oss_fragments = 8;
	assert(used_blocks(&radio) == 0);
	assert(radio.queuesize == 7);
	mock_oss_fragment_total = 8;
	mock_ioctl_failure = SNDCTL_DSP_GETOSPACE;
	assert(used_blocks(&radio) == 1);
	assert(used_blocks(&radio) == 1);

	mock_ioctl_failure = ULONG_MAX;
	mock_close_calls = 0;
	radio.sounddev = 7;
	assert(setformat(&radio, O_CLOSE) == 0);
	assert(radio.sounddev == -1 && mock_close_calls == 1);
	assert(radio.total_blocks == 0);
	mock_open_result = -1;
	assert(setformat(&radio, O_RDWR) == -1);
	mock_open_result = 7;
	for (index = 0; index < ARRAY_LEN(failures); ++index) {
		radio.sounddev = -1;
		mock_ioctl_failure = failures[index];
		assert(setformat(&radio, O_RDWR) == -1);
	}
	mock_ioctl_failure = ULONG_MAX;
	mock_oss_caps = DSP_CAP_DUPLEX;
	mock_oss_speed = 44100;
	radio.sounddev = -1;
	radio.devicenum = 2;
	radio.owner = (struct ast_channel *)(uintptr_t)1;
	radio.frags = 1;
	assert(setformat(&radio, O_RDWR) == 0 && radio.duplex == M_FULL);
	radio.sounddev = -1;
	mock_oss_caps = 0;
	radio.duplex = M_READ;
	assert(setformat(&radio, O_RDWR) == 0 && radio.duplex == M_READ);
	radio.sounddev = -1;
	mock_oss_caps = DSP_CAP_DUPLEX;
	mock_ioctl_failure = SNDCTL_DSP_GETCAPS;
	assert(setformat(&radio, O_RDWR) == 0 && radio.duplex == M_READ);
	mock_ioctl_failure = ULONG_MAX;
	mock_oss_caps = DSP_CAP_DUPLEX;
	radio.sounddev = -1;
	mock_oss_speed = 48000;
	assert(setformat(&radio, O_WRONLY) == 0 && radio.duplex == M_WRITE);
	radio.sounddev = -1;
	assert(setformat(&radio, O_RDONLY) == 0 && radio.duplex == M_READ);
	radio.sounddev = -1;
	assert(setformat(&radio, O_APPEND) == 0);
	radio.sounddev = -1;
	mock_ioctl_failure = SNDCTL_DSP_SETFRAGMENT;
	assert(setformat(&radio, O_RDWR) == 0);
	radio.sounddev = -1;
	assert(setformat(&radio, O_RDWR) == 0);

	mock_ioctl_failure = ULONG_MAX;
	radio.sounddev = -1;
	mock_oss_speed = 44100;
	assert(setformat(&radio, O_RDWR) == 0);
	radio.sounddev = -1;
	assert(setformat(&radio, O_RDWR) == 0);
	mock_oss_speed = 48000;
	radio.radio = &radio_state;
	radio.sounddev = 7;
	mock_sound_write_calls = 0;
	radio_state.txPttIn = radio_state.txPttOut = 0;
	assert(soundcard_writeframe(&radio, output) == (int)sizeof(output));
	assert(mock_sound_write_calls == 1);
	radio_state.txPttIn = 1;
	radio.total_blocks = 8;
	radio.queuesize = 1;
	mock_oss_fragments = 0;
	assert(soundcard_writeframe(&radio, output) == 0);
	assert(mock_sound_write_calls == 1 && radio.plus_sound_dropped_frames == 1);
	radio_state.txPttIn = 0;
	assert(soundcard_writeframe(&radio, output) == 0);
	assert(mock_sound_write_calls == 1 && radio.plus_sound_dropped_frames == 2);
	radio_state.txPttIn = 1;
	radio.total_blocks = 8;
	radio.queuesize = 8;
	mock_oss_fragments = 8;
	/* A valid open device that cannot report space must reject the frame before
	 * issuing a DAC write. */
	{
		unsigned int writes_before = mock_sound_write_calls;
		uint64_t dropped_before = radio.plus_sound_dropped_frames;

		mock_ioctl_failure = SNDCTL_DSP_GETOSPACE;
		assert(soundcard_writeframe(&radio, output) == 0);
		assert(mock_sound_write_calls == writes_before);
		assert(radio.plus_sound_dropped_frames == dropped_before);
		mock_ioctl_failure = ULONG_MAX;
	}
	mock_write_result = -1;
	assert(soundcard_writeframe(&radio, output) == -1);
	assert(mock_sound_write_calls == 2 && radio.plus_sound_short_writes == 1);
	mock_write_result = 1;
	assert(soundcard_writeframe(&radio, output) == 1);
	assert(mock_sound_write_calls == 3 && radio.plus_sound_short_writes == 2);
	mock_write_result = 0;
	assert(soundcard_writeframe(&radio, output) == 0);
	assert(mock_sound_write_calls == 4 && radio.plus_sound_short_writes == 3);
	radio_state.txPttIn = 0;
	mock_write_result = -1;
	assert(soundcard_writeframe(&radio, output) == -1);
	assert(mock_sound_write_calls == 5 && radio.plus_sound_short_writes == 4);
	radio_state.txPttIn = 1;
	mock_oss_fragments = 7;
	mock_write_result = -2;
	assert(soundcard_writeframe(&radio, output) == (int)sizeof(output));
	assert(mock_sound_write_calls == 6 && radio.plus_sound_short_writes == 4);
	mock_oss_fragments = 8;
	mock_write_result = -2;
	assert(soundcard_writeframe(&radio, output) == (int)sizeof(output));
	assert(mock_sound_write_calls == 7 && radio.plus_sound_short_writes == 4);
	/* A free fragment smaller than the direct native frame must not consume it. */
	mock_oss_bytes = (int)sizeof(output) - 1;
	assert(soundcard_writeframe(&radio, output) == 0);
	assert(mock_sound_write_calls == 7 && radio.plus_sound_dropped_frames == 3);
	mock_oss_bytes = -1;
	/* A complete audio write arms from GETODELAY; short writes cannot replace
	 * that deadline. The fallback derives the same count from GETOSPACE. */
	output[0] = 1;
	atomic_init(&radio.plus_radio_tx_active, 1);
	usbradioplus_tx_playout_hold_reset(&radio);
	radio_state.txPttIn = radio_state.txPttOut = 1;
	mock_oss_output_delay = (int)(sizeof(output) * 2U);
	mock_write_result = -2;
	assert(soundcard_writeframe(&radio, output) == (int)sizeof(output));
	assert(radio.plus_tx_playout_hold.callbacks_remaining == 3U);
	mock_write_result = 1;
	assert(soundcard_writeframe(&radio, output) == 1);
	assert(radio.plus_tx_playout_hold.callbacks_remaining == 3U);
	mock_oss_output_delay = -1;
	mock_write_result = -2;
	mock_oss_fragments = 6;
	usbradioplus_tx_playout_hold_reset(&radio);
	assert(soundcard_writeframe(&radio, output) == (int)sizeof(output));
	assert(radio.plus_tx_playout_hold.callbacks_remaining == 3U);
	/* A successful but invalid OSS delay report takes the same safe fallback. */
	mock_oss_output_delay = INT_MIN;
	mock_oss_fragments = 6;
	usbradioplus_tx_playout_hold_reset(&radio);
	assert(soundcard_writeframe(&radio, output) == (int)sizeof(output));
	assert(radio.plus_tx_playout_hold.callbacks_remaining == 3U);
	mock_oss_output_delay = -1;
	/* GETOSPACE can fail after a frame was admitted. The hold calculation must
	 * then use that admission snapshot rather than dropping the accepted frame. */
	mock_oss_output_space_calls = 0;
	fail_oss_output_space_call = 2;
	usbradioplus_tx_playout_hold_reset(&radio);
	assert(soundcard_writeframe(&radio, output) == (int)sizeof(output));
	assert(radio.plus_tx_playout_hold.callbacks_remaining == 4U);
	fail_oss_output_space_call = 0;
	/* A full admission snapshot has no device backlog; retain only the frame
	 * that was just accepted plus the one callback safety hold. */
	mock_oss_fragments = 8;
	mock_oss_bytes = mock_oss_fragment_total * 3840;
	mock_oss_output_space_calls = 0;
	fail_oss_output_space_call = 2;
	usbradioplus_tx_playout_hold_reset(&radio);
	assert(soundcard_writeframe(&radio, output) == (int)sizeof(output));
	assert(radio.plus_tx_playout_hold.callbacks_remaining == 2U);
	fail_oss_output_space_call = 0;
	mock_oss_bytes = -1;
	/* OSS counter values can exceed the callback counter's public range. */
	mock_oss_output_space_calls = 0;
	extreme_oss_output_space_call = 2;
	usbradioplus_tx_playout_hold_reset(&radio);
	assert(soundcard_writeframe(&radio, output) == (int)sizeof(output));
	assert(radio.plus_tx_playout_hold.callbacks_remaining == UINT_MAX);
	extreme_oss_output_space_call = 0;
	mock_oss_fragments = 8;
	mock_oss_output_delay = -1;
	radio.duplex3 = 500;
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
	radio.micplaymax = 100;
	radio.micmax = 100;
	radio.spkrmax = 100;
	mixer_write(&radio);
	radio.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	mixer_write(&radio);
	radio.sounddev = -1;
	mock_open_result = -1;
	assert(soundcard_writeframe(&radio, output) == 0);
	assert(mock_sound_write_calls == 14);
	mock_open_result = 7;
	mock_oss_io = 0;
}

/** @brief Verify oss channel write and call. */
static void test_oss_channel_write_and_call(void)
{
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state radio_state = {0};
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	short samples[160] = {0};
	struct ast_frame frame = {.datalen = sizeof(samples), .data.ptr = samples};

	radio.name = "test";
	radio.radio = &radio_state;
	radio.sounddev = 7;
	radio.plus_app_rpt_samples = ARRAY_LEN(samples);
	assert(!rpcr_init(&radio.plus_program_ring, URP_PROGRAM_RING_SAMPLES, RPCR_SINC_BEST));
	test_channel_private = &radio;
	assert(usbradio_write(channel, &frame) == 0);
	radio.hasusb = 1;
	assert(usbradio_write(channel, &frame) == 0);
	radio.echoing = 1;
	assert(usbradio_write(channel, &frame) == 0);
	radio.plus_advanced = 1;
	assert(usbradio_write(channel, &frame) == 0);
	radio.plus_advanced = 0;
	radio.echoing = 0;
	assert(usbradio_write(channel, &frame) == 0);
	radio.txkeyed = 1;
	assert(usbradio_write(channel, &frame) == 0);
	assert(rpcr_available(&radio.plus_program_ring) > 0);
	rpcr_destroy(&radio.plus_program_ring);
	mock_oss_io = 1;
	mock_open_result = -1;
	radio.sounddev = -1;
	assert(usbradio_write(channel, &frame) == 0);
	mock_oss_io = 0;
	mock_open_result = 7;
	pthread_create_calls = 0;
	assert(usbradio_call(channel, "destination", 1000) == 0);
	assert(!radio.stophid && radio.lasthidtime == 1234 && pthread_create_calls == 1);
	assert(setstate_calls > 0);
	test_channel_private = NULL;
}

/** @brief Verify oss channel hangup. */
static void test_oss_channel_hangup(void)
{
	struct chan_usbradio_pvt radio = {0};
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	struct ast_module_info module_info = {0};

	usbradioplus_test_set_module_info(&module_info);
	radio.owner = channel;
	radio.sounddev = -1;
	test_channel_private = &radio;
	assert(usbradio_hangup(channel) == 0);
	assert(!radio.owner && radio.stophid);
	radio.owner = channel;
	radio.hookstate = 1;
	radio.sounddev = 7;
	test_channel_private = &radio;
	mock_oss_io = 1;
	assert(usbradio_hangup(channel) == 0);
	assert(!radio.hookstate && radio.sounddev == -1);
	mock_oss_io = 0;
	test_channel_private = NULL;
	usbradioplus_test_set_module_info(NULL);
}

/** @brief Verify oss parallel pulser. */
static void test_oss_parallel_pulser(void)
{
	pthread_t thread;

	haspp = hasout = 0;
	usbradio_start_parallel_pulser();
	haspp = hasout = 1;
	usbradio_start_parallel_pulser();

	haspp = 2;
	pp_val = 0;
	memset(pp_pulsetimer, 0, sizeof(pp_pulsetimer));
	pp_pulsetimer[2] = 10;
	pp_pulsemask = 0;
	parallel_write_calls = 0;
	stop_pulser_on_usleep = 1;
	assert(pthread_create(&thread, NULL, pulserthread, NULL) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(pp_pulsemask == 1);
	assert(pp_val == 1);
	assert(parallel_write_calls == 2);
	stoppulser = 0;
	pp_val = 0;
	pp_pulsemask = 0;
	pp_pulsetimer[2] = 5;
	mock_tvnow_step = 10;
	stop_pulser_on_usleep = 1;
	assert(pthread_create(&thread, NULL, pulserthread, NULL) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(pp_pulsetimer[2] == 0 && pp_pulsemask == 0);
	haspp = 1;
	pp_pulsetimer[2] = 0;
	stop_pulser_on_usleep = 1;
	assert(pthread_create(&thread, NULL, pulserthread, NULL) == 0);
	assert(pthread_join(thread, NULL) == 0);
	mock_tvnow_step = 0;
	stop_pulser_on_usleep = 0;
}

/** @brief Verify oss hid worker device retry. */
static void test_oss_hid_worker_device_retry(void)
{
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state radio_state = {0};
	pthread_t thread;

	radio.name = "test";
	radio.radio = &radio_state;
	radio.pttkick[0] = radio.pttkick[1] = -1;
	usbradio_default.next = &radio;
	radio.next = NULL;
	installed_usb_device = NULL;
	stop_hid_radio_on_usleep = &radio;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(radio.stophid && radio.gpio_set && !radio.hasusb);
	stop_hid_radio_on_usleep = NULL;
	usbradio_default.next = NULL;
}

/** @brief Exercise OSS HID recovery using scripted USB availability.
 * @param radio Named radio profile or signaling state, as declared.
 */
static void run_oss_hid_retry(struct chan_usbradio_pvt *radio)
{
	pthread_t thread;

	radio->stophid = 0;
	stop_hid_radio_on_usleep = radio;
	assert(pthread_create(&thread, NULL, hidthread, radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(radio->stophid);
	stop_hid_radio_on_usleep = NULL;
}

/** @brief Verify oss hid attach failures. */
static void test_oss_hid_attach_failures(void)
{
	struct chan_usbradio_pvt radio = {0};
	struct chan_usbradio_pvt other = {0};
	struct chan_usbradio_pvt candidate = {0};
	urp_radio_state radio_configuration = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	pthread_t thread;

	settings_defaults(&settings);
	strcpy(settings.profiles[0].name, "test");
	strcpy(settings.profiles[0].channel, "RadioPlus/test");
	settings.profiles[0].enabled = 0;
	radio.name = "test";
	radio.radio = urp_radio_create(&radio_configuration, URP_LINK_SAMPLES);
	assert(radio.radio);
	radio.pttkick[0] = radio.pttkick[1] = -1;
	strcpy(radio.devstr, "usb-test");
	usbradio_default.next = &radio;
	radio.next = NULL;
	installed_usb_device = "usb-test";
	mock_usb_device_number = -1;
	run_oss_hid_retry(&radio);

	mock_usb_device_number = 0;
	mock_hid_device_available = 0;
	run_oss_hid_retry(&radio);
	mock_hid_device_available = 1;
	mock_usb_open_success = 0;
	run_oss_hid_retry(&radio);
	mock_usb_open_success = 1;
	mock_usb_claim_calls = 0;
	mock_usb_claim_result = -1;
	mock_usb_detach_result = -1;
	run_oss_hid_retry(&radio);
	mock_usb_claim_calls = 0;
	mock_usb_detach_result = 0;
	mock_usb_second_claim_result = -1;
	run_oss_hid_retry(&radio);

	/* Automatic assignment reports an empty USB inventory without changing devstr. */
	mock_usb_second_claim_result = 0;
	radio.devstr[0] = '\0';
	radio.serial[0] = '\0';
	mock_no_usb_devices = 1;
	run_oss_hid_retry(&radio);
	assert(!radio.devstr[0]);
	run_oss_hid_retry(&radio);
	mock_no_usb_devices = 0;

	/* Automatic assignment skips an occupied device and records the next serial. */
	other.name = "other";
	strcpy(other.devstr, "usb-test");
	other.usbass = 1;
	radio.next = &other;
	other.next = NULL;
	mock_second_usb_device = 1;
	mock_usb_serial_result = 1;
	mock_hid_device_available = 0;
	run_oss_hid_retry(&radio);
	assert(!strcmp(radio.devstr, "usb-second"));
	assert(!strcmp(radio.serial, mock_usb_serial));
	mock_second_usb_device = 0;
	mock_usb_serial_result = 0;

	/* Reject a device already owned by another configured channel. */
	mock_usb_claim_result = 0;
	mock_usb_second_claim_result = 0;
	strcpy(other.devstr, "usb-test");
	other.usbass = 1;
	strcpy(radio.devstr, "usb-test");
	radio.next = &other;
	other.next = NULL;
	run_oss_hid_retry(&radio);

	/* Serial enumeration covers missing serials and an exact match. */
	other.usbass = 0;
	radio.next = NULL;
	radio.devstr[0] = '\0';
	strcpy(radio.serial, "serial-test");
	mock_usb_serial_result = 0;
	mock_hid_device_available = 0;
	run_oss_hid_retry(&radio);
	radio.devstr[0] = '\0';
	mock_usb_serial_result = 1;
	mock_usb_serial = "different-serial";
	run_oss_hid_retry(&radio);
	assert(radio.devstr[0]);
	mock_usb_serial = "serial-test";
	strcpy(radio.serial, "serial-test");
	radio.devstr[0] = '\0';
	mock_usb_serial_result = 1;
	mock_usb_serial_by_device = 1;
	mock_second_usb_device = 1;
	installed_usb_device = "usb-second";
	mock_hid_device_available = 0;
	run_oss_hid_retry(&radio);
	assert(!strcmp(radio.devstr, "usb-second"));
	mock_usb_serial_by_device = 0;
	mock_second_usb_device = 0;
	installed_usb_device = "usb-test";

	/* Assigned nonmatching channels do not block a distinct installed candidate. */
	radio.serial[0] = '\0';
	strcpy(radio.devstr, "missing");
	strcpy(other.devstr, "usb-other");
	other.usbass = 1;
	strcpy(candidate.devstr, "usb-test");
	candidate.usbass = 0;
	radio.next = &other;
	other.next = &candidate;
	candidate.next = NULL;
	mock_usb_device_number = 0;
	run_oss_hid_retry(&radio);
	assert(!strcmp(radio.devstr, "usb-test"));
	radio.next = &other;
	other.next = NULL;

	/* A stale configured device may fall back to an installed configured device. */
	radio.serial[0] = '\0';
	strcpy(radio.devstr, "missing");
	strcpy(other.devstr, "usb-test");
	radio.next = &other;
	other.usbass = 0;
	installed_usb_device = "usb-test";
	mock_usb_device_number = -1;
	run_oss_hid_retry(&radio);
	mock_usb_device_number = 0;
	other.usbass = 1;
	run_oss_hid_retry(&radio);
	other.usbass = 0;
	strcpy(radio.devstr, "missing");
	run_oss_hid_retry(&radio);
	assert(!strcmp(radio.devstr, "usb-test"));
	/* Repeated discovery failures suppress duplicate device diagnostics. */
	strcpy(radio.devstr, "missing");
	radio.device_error = 1;
	installed_usb_device = NULL;
	run_oss_hid_retry(&radio);
	assert(radio.device_error);
	installed_usb_device = "usb-test";

	/* A detach followed by a successful second claim reaches normal setup. */
	radio.pttkick[0] = radio.pttkick[1] = -1;
	radio.stophid = 0;
	mock_hid_device_available = 1;
	mock_usb_open_success = 1;
	mock_usb_claim_calls = 0;
	mock_usb_claim_result = -1;
	mock_usb_second_claim_result = 0;
	mock_usb_detach_result = 0;
	mock_pipe_failure = 1;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	mock_pipe_failure = 0;
	mock_usb_claim_result = 0;

	/* Once attachment succeeds, pipe creation failure terminates the worker. */
	radio.pttkick[0] = radio.pttkick[1] = -1;
	mock_hid_device_available = 1;
	mock_usb_open_success = 1;
	mock_pipe_failure = 1;
	radio.stophid = 0;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(radio.pttkick[0] == -1 && radio.pttkick[1] == -1);
	mock_pipe_failure = 0;

	/* A failed nonblocking setup must close both freshly-created pipe ends and
	 * release the claimed HID interface rather than leaving a future control
	 * writer blocked on a partially initialized wake pipe. */
	radio.pttkick[0] = radio.pttkick[1] = -1;
	radio.stophid = 0;
	mock_fcntl_calls = 0;
	/* Fail the outer F_SETFL; failing F_GETFL would be ORed with O_NONBLOCK. */
	mock_fcntl_fail_call = 2;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(radio.pttkick[0] == -1 && radio.pttkick[1] == -1);
	mock_fcntl_fail_call = 0;

	/* The second endpoint is independently required to be nonblocking. */
	radio.pttkick[0] = radio.pttkick[1] = -1;
	radio.stophid = 0;
	mock_fcntl_calls = 0;
	mock_fcntl_fail_call = 4;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(radio.pttkick[0] == -1 && radio.pttkick[1] == -1);
	mock_fcntl_fail_call = 0;

	mock_usb_serial_result = 0;
	mock_hid_device_available = 0;
	mock_usb_open_success = 0;
	mock_usb_claim_result = 0;
	mock_usb_second_claim_result = 0;
	mock_usb_detach_result = 0;
	mock_usb_device_number = 0;
	mock_no_usb_devices = 0;
	mock_second_usb_device = 0;
	installed_usb_device = NULL;
	usbradio_default.next = NULL;
	assert(!urp_radio_destroy(radio.radio));
}

/** Select the advanced-interface variant of hardware attachment. */
static int advanced_hid_mode;

/** @brief Verify hardware attachment for the selected controller interface. */
static void test_oss_hid_worker_attach(void)
{
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state radio_configuration = {0};
	struct ast_variable explicit_rxvoice = {.name = "rxvoiceadj", .value = "0.625"};
	pthread_t thread;

	settings_defaults(&settings);
	settings.profiles[0].enabled = 0;
	strcpy(settings.profiles[0].name, "test");
	strcpy(settings.profiles[0].channel, "RadioPlus/test");
	radio.name = "test";
	strcpy(radio.devstr, "usb-test");
	radio.plus_advanced = advanced_hid_mode;
	radio.owner = (struct ast_channel *)(uintptr_t)1;
	radio.sounddev = -1;
	radio.pttkick[0] = radio.pttkick[1] = -1;
	radio.frags = 1;
	radio.queuesize = 2;
	radio.plus_app_rpt_rate = URP_RATE_LINK;
	radio.plus_app_rpt_samples = URP_LINK_SAMPLES;
	radio.plus_deemphasis_corner_hz = 300.0;
	radio.plus_preemphasis_corner_hz = 300.0;
	radio.valid_gpios = (1 << 0) | (1 << 2);
	radio.gpios[0] = "in";
	radio.gpios[1] = "out";
	radio.gpios[2] = "in";
	radio.gpios[3] = "in";
	radio.txmixa = TX_OUT_VOICE;
	radio.txmixb = TX_OUT_LSD;
	strcpy(radio.txctcssfreq, "100.0");
	radio_configuration.pRxCodeSrc = "0";
	radio_configuration.pTxCodeSrc = "0";
	radio_configuration.pTxCodeDefault = "0";
	radio.radio = urp_radio_create(&radio_configuration, URP_LINK_SAMPLES);
	assert(radio.radio);
	assert(usbradioplus_dsp_init(&radio) == 0);
	usbradio_default.next = &radio;
	radio.next = NULL;
	installed_usb_device = "usb-test";
	mock_hid_device_available = 1;
	mock_usb_open_success = 1;
	mock_usb_claim_result = 0;
	mock_usb_detach_result = 0;
	mock_oss_io = 1;
	mock_new_mixer_name = 1;
	mock_usb_device.descriptor.idProduct = C108AH_PRODUCT_ID;
	mock_open_result = 7;
	mock_ioctl_failure = ULONG_MAX;
	mock_poll_enabled = 1;
	mock_poll_result = 0;
	mock_poll_revents = 0;
	mock_hid_input_calls = 0;
	stop_hid_after_inputs = 2;
	toggle_hid_inputs_mask = 1;
	mock_hid_inputs[0] = 0x10;
	mock_tvnow_step = 100;
	radio.hid_gpio_pulsetimer[0] = 10;
	radio.hid_gpio_pulsetimer[1] = 1000;
	usbradioplus_publish_hardware_ptt(&radio, 1);
	radio.devtype = C108AH_PRODUCT_ID;
	haspp = 2;
	radio.pps[2] = "ptt";
	radio.pps[3] = "out";
	radio.pps[10] = "in";
	radio.pps[11] = "out";
	radio.pps[12] = "cor";
	radio.pps[13] = "ctcss";
	radio.pps[14] = "in";
	mock_parallel_inputs = 0xff;
	toggle_parallel_inputs_mask = 0x40;
	stop_hid_on_input = &radio;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(radio.stophid && radio.hasusb && radio.usbass);
	ast_set_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	module_debug_level = 10;
	file_debug_level = 0;

	/* The audio callback only posts the clip request; the HID owner turns it
	 * into one timed GPIO pulse after inspecting its current timer state. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	stop_hid_after_inputs = 1;
	radio.clipledgpio = 1;
	radio.hid_gpio_pulsetimer[0] = 0;
	atomic_store_explicit(&radio.plus_clip_led_request, 1, memory_order_release);
	stop_hid_on_input = &radio;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(radio.hid_gpio_pulsetimer[0] == CLIP_LED_HOLD_TIME_MS);
	assert(!atomic_load_explicit(&radio.plus_clip_led_request, memory_order_acquire));

	/* A stale request is consumed even when this interface has no clip LED. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	stop_hid_after_inputs = 1;
	radio.clipledgpio = 0;
	atomic_store_explicit(&radio.plus_clip_led_request, 1, memory_order_release);
	stop_hid_on_input = &radio;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!atomic_load_explicit(&radio.plus_clip_led_request, memory_order_acquire));

	/* A request arriving while the LED pulse is active is consumed without
	 * restarting its timer. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	stop_hid_after_inputs = 1;
	radio.clipledgpio = 1;
	radio.hid_gpio_pulsetimer[0] = 1;
	mock_tvnow_step = 0;
	atomic_store_explicit(&radio.plus_clip_led_request, 1, memory_order_release);
	stop_hid_on_input = &radio;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(radio.hid_gpio_pulsetimer[0] == 1);
	assert(!atomic_load_explicit(&radio.plus_clip_led_request, memory_order_acquire));
	mock_tvnow_step = 100;

	/* An in-progress radio-program publication cannot defer an unkey.  The
	 * worker must use its no-program release branch for the parallel PTT line. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	/* Publish the unkey in one polling pass, then stop on the next so the
	 * worker must apply the unavailable-program release before cleanup runs. */
	stop_hid_after_inputs = 2;
	parallel_write_calls = 0;
	unkey_without_program_after_inputs = 1;
	unkey_without_program_on_input = &radio;
	stop_hid_on_input = &radio;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!unkey_without_program_on_input && !radio.lasttx);
	assert(parallel_write_calls > 0);
	assert(!atomic_load_explicit(&radio.plus_hardware_ptt_applied, memory_order_acquire));
	atomic_store_explicit(&radio.plus_radio_program_generation, 0U, memory_order_release);

	/* A missing program snapshot only skips radio programming.  It must not
	 * prevent an otherwise requested physical key-up. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	stop_hid_after_inputs = 2;
	parallel_write_calls = 0;
	radio.invertptt = 0;
	mock_parallel_ptt_mask = 1U;
	parallel_saw_ptt_asserted = 0;
	key_without_program_after_inputs = 1;
	key_without_program_on_input = &radio;
	stop_hid_on_input = &radio;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!key_without_program_on_input && parallel_saw_ptt_asserted);
	assert(parallel_write_calls > 0);
	mock_parallel_ptt_mask = 0;
	atomic_store_explicit(&radio.plus_radio_program_generation, 0U, memory_order_release);
	usbradioplus_publish_hardware_ptt(&radio, 0);

	/* A new, valid radio-program snapshot is applied even if PTT did not
	 * change; the programming update must not manufacture a key transition. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	stop_hid_after_inputs = 1;
	parallel_write_calls = 0;
	radio.lasttx = 0;
	radio.rxfreq = 146520000U;
	radio.txfreq = 146520000U;
	usbradioplus_program_radio(&radio);
	usbradioplus_publish_hardware_ptt(&radio, 0);
	stop_hid_on_input = &radio;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!radio.lasttx && parallel_write_calls > 0);

	/* A fresh radio program and a PTT transition may arrive in one worker pass.
	 * Publish them after setup so the worker observes both in this iteration. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	stop_hid_after_inputs = 1;
	parallel_write_calls = 0;
	mock_parallel_ptt_mask = 1U;
	parallel_saw_ptt_asserted = 0;
	radio.lasttx = 0;
	radio.rxfreq = 146520000U;
	radio.txfreq = 146520000U;
	usbradioplus_publish_hardware_ptt(&radio, 0);
	program_and_key_after_inputs = 1;
	program_and_key_on_input = &radio;
	stop_hid_on_input = &radio;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!program_and_key_on_input && parallel_saw_ptt_asserted);
	mock_parallel_ptt_mask = 0;
	usbradioplus_publish_hardware_ptt(&radio, 0);

	/* Once setup's program snapshot is current, a later PTT request must key
	 * without reprogramming the radio. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	stop_hid_after_inputs = 2;
	parallel_write_calls = 0;
	mock_parallel_ptt_mask = 1U;
	parallel_saw_ptt_asserted = 0;
	radio.lasttx = 0;
	radio.rxfreq = 146520000U;
	radio.txfreq = 146520000U;
	usbradioplus_publish_hardware_ptt(&radio, 0);
	key_after_program_after_inputs = 2;
	key_after_program_on_input = &radio;
	stop_hid_on_input = &radio;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!key_after_program_on_input && parallel_saw_ptt_asserted);
	mock_parallel_ptt_mask = 0;
	usbradioplus_publish_hardware_ptt(&radio, 0);

	/* A valid EEPROM read can be followed by a requested write in the same
	 * service interval; an adapter checksum failure must still clear control. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	stop_hid_after_inputs = 1;
	toggle_hid_inputs_mask = 1;
	radio.wanteeprom = 1;
	mock_eeprom_result = 0;
	mock_eeprom_valid_magic = 1;
	test_config_load_result = (struct ast_config *)(uintptr_t)1;
	test_config_variables = &explicit_rxvoice;
	radio.eepromctl = 1;
	eeprom_write_after_read = &radio;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!radio.eepromctl);
	test_config_load_result = NULL;
	test_config_variables = NULL;
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	radio.eepromctl = 1;
	radio.txmixb = TX_OUT_COMPOSITE;
	eeprom_write_after_read = NULL;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!radio.eepromctl);
	radio.txmixb = TX_OUT_LSD;
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	radio.eepromctl = 2;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!radio.eepromctl);
	module_debug_level = 0;
	file_debug_level = 10;

	/* A readable EEPROM with a foreign signature is ignored. Exercise an
	 * inverted PTT release and a non-retryable kick-pipe read in the same interval. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	radio.eepromctl = 1;
	mock_eeprom_result = 0;
	mock_eeprom_valid_magic = 0;
	radio.invertptt = 1;
	radio.lasttx = 1;
	usbradioplus_publish_hardware_ptt(&radio, 0);
	mock_poll_revents = POLLIN;
	mock_read_result = -1;
	mock_read_errno = EIO;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!radio.eepromctl && radio.invertptt);
	mock_read_errno = EAGAIN;

	/* A wake pipe can close cleanly or report its other nonblocking errno. Both
	 * are nonfatal because the worker always polls its atomic PTT request too. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	stop_hid_after_inputs = 1;
	mock_read_result = 0;
	mock_poll_revents = POLLIN;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	mock_read_result = -1;
	mock_read_errno = EWOULDBLOCK;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	mock_read_errno = EAGAIN;

	/* A readable kick byte requests the unkeyed state without forcing HID recovery. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	mock_read_result = 1;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!radio.lasttx);
	/* A real nonblocking kick pipe becomes empty after its readable byte. The
	 * fixture must do the same so the worker's drain-until-EAGAIN loop reaches
	 * normal shutdown instead of spinning forever on a fictional byte. */
	assert(mock_read_result == -1 && mock_read_errno == EAGAIN);

	/* With a stable valid programming snapshot and unchanged PTT, the second
	 * HID pass must be a no-op.  High input bits also represent idle COR and
	 * CTCSS, so neither HID signaling bit may be published. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	stop_hid_after_inputs = 2;
	toggle_hid_inputs_mask = 0;
	memset(mock_hid_inputs, 0, sizeof(mock_hid_inputs));
	radio.hdwtype = 0;
	assert(hidhdwconfig(&radio) == 0);
	assert(radio.hid_io_cor && radio.hid_io_ctcss);
	mock_hid_inputs[radio.hid_io_cor_loc] |= radio.hid_io_cor;
	mock_hid_inputs[radio.hid_io_ctcss_loc] |= radio.hid_io_ctcss;
	radio.invertptt = 0;
	radio.lasttx = 0;
	radio.rxfreq = 146520000U;
	radio.txfreq = 146520000U;
	usbradioplus_program_radio(&radio);
	usbradioplus_publish_hardware_ptt(&radio, 0);
	mock_poll_revents = 0;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(mock_hid_input_calls == 2);
	assert(!(atomic_load_explicit(&radio.plus_hardware_inputs, memory_order_acquire) &
		 URP_HARDWARE_INPUT_HID_CARRIER));
	assert(!(atomic_load_explicit(&radio.plus_hardware_inputs, memory_order_acquire) &
		 URP_HARDWARE_INPUT_HID_CTCSS));
	memset(mock_hid_inputs, 0, sizeof(mock_hid_inputs));
	mock_hid_inputs[0] = 0x10;
	toggle_hid_inputs_mask = 1;
	ast_clear_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	file_debug_level = 0;
	option_debug = 10;
	mock_poll_revents = 0;
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	mock_eeprom_result = 1;
	mock_eeprom_valid_magic = 0;
	eeprom_write_after_read = NULL;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!radio.eepromctl);

	/* Stable low inputs cover the no-change path for GPIO and parallel status.
	 * Omitting a parallel PTT assignment also exercises USB-only keying. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	stop_hid_after_inputs = 2;
	toggle_hid_input_index = radio.hid_gpio_loc;
	mock_hid_inputs[radio.hid_gpio_loc] = 1 << 2;
	toggle_hid_inputs_mask = 0;
	toggle_parallel_inputs_mask = 0;
	radio.pps[2] = NULL;
	radio.pps[11] = "in";
	radio.had_gpios_in = 1;
	radio.last_gpios_in = 0;
	radio.had_pp_in = 1;
	radio.last_pp_in = 0;
	mock_parallel_inputs = 0x80;
	radio.invertptt = 0;
	radio.lasttx = 1;
	usbradioplus_publish_hardware_ptt(&radio, 0);
	mock_poll_revents = 0;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!radio.lasttx);
	/* Losing the attached interface returns to discovery and closes the old
	 * libusb handle before retrying. */
	radio.stophid = 0;
	radio.wanteeprom = 0;
	mock_hid_input_calls = 0;
	stop_hid_on_input = NULL;
	drop_hid_on_input = &radio;
	stop_hid_radio_on_usleep = &radio;
	mock_hid_device_available = 1;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(radio.stophid && !radio.hasusb);
	drop_hid_on_input = NULL;
	stop_hid_radio_on_usleep = NULL;
	mock_hid_device_available = 1;

	/* A failed poll sleeps briefly and retries until shutdown is requested. */
	radio.stophid = 0;
	radio.wanteeprom = 0;
	stop_hid_on_input = NULL;
	stop_hid_radio_on_usleep = &radio;
	mock_poll_result = -1;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(radio.stophid);
	stop_hid_radio_on_usleep = NULL;
	mock_poll_result = 0;
	stop_hid_on_input = NULL;
	mock_eeprom_result = 0;
	mock_oss_io = 0;
	mock_new_mixer_name = 0;
	mock_usb_device.descriptor.idProduct = 0;
	mock_hid_device_available = 0;
	mock_usb_open_success = 0;
	mock_poll_enabled = 0;
	mock_tvnow_step = 0;
	option_debug = 0;
	module_debug_level = file_debug_level = 0;
	haspp = 0;
	toggle_parallel_inputs_mask = 0;
	installed_usb_device = NULL;
	usbradio_default.next = NULL;
	if (radio.pttkick[0] >= 0)
		close(radio.pttkick[0]);
	if (radio.pttkick[1] >= 0)
		close(radio.pttkick[1]);
	usbradioplus_dsp_destroy(&radio);
	urp_radio_destroy(radio.radio);
}

/** @brief Verify oss hid worker first radio construction. */
static void test_oss_hid_worker_first_radio_construction(void)
{
	struct chan_usbradio_pvt radio = {0};
	struct ast_variable ignored_option = {.name = "unsupported", .value = "1"};
	pthread_t thread;

	settings_defaults(&settings);
	settings.profiles[0].enabled = 0;
	ast_copy_string(settings.profiles[0].name, "first-radio",
			sizeof(settings.profiles[0].name));
	ast_copy_string(settings.profiles[0].channel, "RadioPlus/first-radio",
			sizeof(settings.profiles[0].channel));
	radio.name = "first-radio";
	radio.plus_advanced = advanced_hid_mode == 1;
	radio.radioduplex = advanced_hid_mode == 2;
	strcpy(radio.devstr, "usb-test");
	radio.sounddev = -1;
	radio.pttkick[0] = radio.pttkick[1] = -1;
	radio.plus_app_rpt_rate = URP_RATE_LINK;
	radio.plus_app_rpt_samples = URP_LINK_SAMPLES;
	radio.plus_deemphasis_corner_hz = 250.0;
	radio.plus_preemphasis_corner_hz = 250.0;
	radio.radioactive = 1;
	radio.txpreemphasis = 1;
	radio.txmixa = TX_OUT_OFF;
	radio.txmixb = TX_OUT_LSD;
	radio.txctcssadj = 250;
	assert(usbradioplus_dsp_init(&radio) == 0);
	usbradio_default.next = &radio;
	radio.next = NULL;
	installed_usb_device = "usb-test";
	mock_hid_device_available = 1;
	mock_usb_open_success = 1;
	mock_usb_claim_calls = 0;
	mock_usb_claim_result = 0;
	mock_usb_device_number = 0;
	mock_oss_io = 1;
	mock_open_result = 7;
	mock_usb_device.descriptor.idProduct = C108_PRODUCT_ID + 1;
	mock_poll_enabled = 1;
	mock_poll_result = 0;
	test_config_load_result = (struct ast_config *)(uintptr_t)1;
	test_config_variables = &ignored_option;
	mock_hid_input_calls = 0;
	stop_hid_after_inputs = 1;
	stop_hid_on_input = &radio;
	fail_radio_state_allocation = 1;
	stop_hid_radio_on_usleep = &radio;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!radio.radio);
	fail_radio_state_allocation = 0;
	stop_hid_radio_on_usleep = NULL;
	/* A malformed processing override is reported but must not prevent the HID
	 * worker from bringing up an otherwise valid radio interface. */
	add_processing_override("hardware", "hardware_interface_type", "invalid");
	radio.stophid = 0;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(radio.radio && radio.hasusb && radio.radioactive);
	settings.profiles[0].override_count = 0;

	/* Reuse the attached-channel fixture to cover a first construction with
	 * voice but no output assigned for a configured CTCSS tone. */
	urp_radio_destroy(radio.radio);
	radio.radio = NULL;
	if (radio.pttkick[0] >= 0)
		close(radio.pttkick[0]);
	if (radio.pttkick[1] >= 0)
		close(radio.pttkick[1]);
	radio.pttkick[0] = radio.pttkick[1] = -1;
	radio.stophid = radio.hasusb = radio.usbass = 0;
	radio.radioactive = 0;
	radio.txpreemphasis = 0;
	radio.txmixa = TX_OUT_VOICE;
	radio.txmixb = TX_OUT_OFF;
	strcpy(radio.txctcssfreq, "100.0");
	test_config_load_result = NULL;
	test_config_variables = NULL;
	mock_hid_input_calls = 0;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(radio.radio && radio.hasusb);
	stop_hid_on_input = NULL;
	mock_poll_enabled = 0;
	mock_oss_io = 0;
	mock_hid_device_available = 0;
	mock_usb_device.descriptor.idProduct = 0;
	mock_usb_open_success = 0;
	installed_usb_device = NULL;
	test_config_load_result = NULL;
	test_config_variables = NULL;
	usbradio_default.next = NULL;
	if (radio.pttkick[0] >= 0)
		close(radio.pttkick[0]);
	if (radio.pttkick[1] >= 0)
		close(radio.pttkick[1]);
	usbradioplus_dsp_destroy(&radio);
	urp_radio_destroy(radio.radio);
}

#endif

/** @brief Verify oss tune write paths. */
static void test_oss_tune_write_paths(void)
{
	struct chan_usbradio_pvt radio = {0};
#ifdef URP_TEST_MODERN
	struct ast_radio_device radio_device = {.devstr = "usb-assigned",
						.serial = "serial-assigned"};
	struct ast_variable serial_selector = {.name = "serial", .value = "configured-serial"};
#endif
	struct ast_variable duplex_mode = {.name = "duplex3mode", .value = "hardware"};
	struct ast_variable device = {.name = "devstr", .value = "usb-test", .next = &duplex_mode};

	radio.name = "test";
	strcpy(radio.devstr, "usb-test");
	strcpy(radio.serial, "serial-test");
#ifdef URP_TEST_MODERN
	radio.radio_device = &radio_device;
#else
	radio.micmax = 100;
#endif
	radio.rxmixerset = 500;
	radio.txmixaset = 501;
	radio.txmixbset = 502;
	radio.rxctcssadj = 0.75F;
	radio.txctcssadj = 503;
	radio.rxsquelchadj = 504;
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_HID;
	radio.rxdemod = RX_AUDIO_FLAT;
	radio.txmixa = TX_OUT_VOICE;
	radio.txmixb = TX_OUT_LSD;
	radio.duplex3mode = DUPLEX3_MODE_SOFTWARE;

	test_config_load_result = CONFIG_STATUS_FILEMISSING;
	tune_write(&radio);
	test_config_load_result = CONFIG_STATUS_FILEINVALID;
	tune_write(&radio);
	test_config_load_result = (struct ast_config *)(uintptr_t)1;
	test_category_get_result = NULL;
	tune_write(&radio);
	test_category_get_result = (struct ast_category *)(uintptr_t)1;
	test_config_variables = &device;
	separate_processing_config_result = 1;
	test_processing_config_load_result = CONFIG_STATUS_FILEMISSING;
	config_save_result = 1;
	variable_update_result = -1;
	variable_new_failure = 1;
	strcpy(radio.devstr, "usb-changed");
	radio.txpreemphasis = 1;
	radio.wanteeprom = 1;
	radio.eepromctl = 1;
	usbradio_default.next = &radio;
	clear_eeprom_on_usleep = 1;
	tune_write(&radio);
	assert(radio.eepromctl == 2);
	assert(radio.eeprom[EEPROM_USER_RXMIXERSET] == 500);
	clear_eeprom_on_usleep = 0;
	variable_new_failure = 0;
	variable_update_result = 0;
	strcpy(radio.devstr, "usb-test");
	radio.txpreemphasis = 0;
	config_save_result = 0;
	separate_processing_config_result = 0;
	radio.wanteeprom = 0;
	tune_write(&radio);
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
	tune_write(&radio);
	radio.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	radio.serial[0] = '\0';
	tune_write(&radio);
	radio.devstr[0] = '\0';
	tune_write(&radio);
	strcpy(radio.devstr, "usb-test");
#ifdef URP_TEST_MODERN
	/* A serial-only explicit selector evaluates both automatic-assignment
	 * predicates, and an existing serial variable is updated from an empty
	 * current serial value. */
	radio.devstr[0] = '\0';
	strcpy(radio.serial, "configured-serial");
	tune_write(&radio);
	strcpy(radio.devstr, "configured-device");
	radio.serial[0] = '\0';
	device.next = &serial_selector;
	serial_selector.next = &duplex_mode;
	tune_write(&radio);
	device.next = &duplex_mode;

	radio.devstr[0] = '\0';
	modern_automatic_device_count = 2;
	variable_update_result = 0;
	tune_write(&radio);
	radio_device.serial = NULL;
	tune_write(&radio);
	radio_device.devstr = "";
	tune_write(&radio);
	radio_device.devstr = "usb-assigned";
	variable_update_result = -1;
	variable_new_failure = 1;
	radio_device.serial = "serial-assigned";
	tune_write(&radio);
	radio_device.serial = NULL;
	device.inherited = 1;
	tune_write(&radio);
	device.inherited = 0;
	variable_new_failure = 0;
	variable_update_result = 0;
	modern_automatic_device_count = 0;
#endif
	test_config_variables = NULL;
	mock_second_usb_device = 0;
	tune_write(&radio);
	mock_second_usb_device = 1;
	tune_write(&radio);
	mock_second_usb_device = 0;
	test_category_get_result = (struct ast_category *)(uintptr_t)1;
	test_config_load_result = NULL;
	usbradio_default.next = NULL;
}

#ifndef URP_TEST_MODERN

/** @brief Verify oss channel creation and request. */
static void test_oss_channel_creation_and_request(void)
{
	struct chan_usbradio_pvt radio = {0};
	struct ast_module_info module_info = {0};
	struct ast_format_cap *formats = (struct ast_format_cap *)(uintptr_t)1;
	urp_radio_state radio_configuration = {0};
	struct ast_channel *channel;
	int cause = 0;

	usbradioplus_test_set_module_info(&module_info);
	radio.name = "test";
	radio.sounddev = -1;
	fail_channel_alloc = 1;
	assert(!usbradio_new(&radio, "s", "default", AST_STATE_DOWN, NULL, NULL));
	fail_channel_alloc = 0;
	channel = usbradio_new(&radio, "s", "default", AST_STATE_DOWN, NULL, NULL);
	assert(channel && radio.owner == channel && test_channel_private == &radio);
	radio.owner = NULL;
	radio.hasusb = 1;
	radio.sounddev = -1;
	mock_oss_io = 1;
	mock_open_result = -1;
	assert(usbradio_new(&radio, "s", "default", AST_STATE_DOWN, NULL, NULL));
	radio.owner = NULL;
	radio.sounddev = -1;
	mock_open_result = 7;
	assert(usbradio_new(&radio, "s", "default", AST_STATE_DOWN, NULL, NULL));
	mock_oss_io = 0;
	radio.hasusb = 0;
	radio.owner = NULL;
	pbx_start_result = 0;
	channel = usbradio_new(&radio, "s", "default", AST_STATE_UP, NULL, NULL);
	assert(channel && radio.owner == channel);
	radio.owner = NULL;
	pbx_start_result = 1;
	hangup_calls = 0;
	assert(!usbradio_new(&radio, "s", "default", AST_STATE_UP, NULL, NULL));
	assert(hangup_calls == 1 && !radio.owner);
	pbx_start_result = 0;

	usbradio_default.next = NULL;
	assert(!usbradio_request("RadioPlus", formats, NULL, NULL, "missing", &cause));
	usbradio_default.next = &radio;
	radio.next = NULL;
	format_compatible = 0;
	assert(!usbradio_request("RadioPlus", formats, NULL, NULL, "test", &cause));
	format_compatible = 1;
	radio.owner = (struct ast_channel *)(uintptr_t)3;
	assert(!usbradio_request("RadioPlus", formats, NULL, NULL, "test", &cause));
	assert(cause == AST_CAUSE_BUSY);
	radio.owner = NULL;
	fail_channel_alloc = 1;
	assert(!usbradio_request("RadioPlus", formats, NULL, NULL, "test", &cause));
	fail_channel_alloc = 0;
	radio_configuration.pRxCodeSrc = "0";
	radio_configuration.pTxCodeSrc = "0";
	radio_configuration.pTxCodeDefault = "0";
	radio.radio = urp_radio_create(&radio_configuration, URP_LINK_SAMPLES);
	assert(radio.radio);
	radio.owner = NULL;
	assert(usbradio_request("RadioPlus", formats, NULL, NULL, "test", &cause));
	assert(!radio.remoted);
	urp_radio_destroy(radio.radio);
	usbradioplus_test_set_module_info(NULL);
	usbradio_default.next = NULL;
}

/** @brief Verify oss channel read guards. */
static void test_oss_channel_read_guards(void)
{
	struct chan_usbradio_pvt radio = {0};
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;

	radio.name = "test";
	radio.sounddev = 7;
	urp_sample_queue_init(&radio.echo_queue, radio.echo_samples, URP_ECHO_QUEUE_SAMPLES);
	test_channel_private = &radio;
	radio.lasthidtime = 1230;
	assert(!usbradio_read(channel));
	radio.lasthidtime = 1232;
	assert(usbradio_read(channel) == &ast_null_frame);
	radio.rxkeyed = 1;
	radio.owner = channel;
	radio.duplex3 = 0;
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
	assert(usbradio_read(channel) == &ast_null_frame);
	assert(!radio.rxkeyed && !radio.lastrx);
	radio.rxkeyed = 1;
	radio.duplex3 = 999;
	radio.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	assert(usbradio_read(channel) == &ast_null_frame);
	assert(!radio.rxkeyed && !radio.lastrx);
	radio.rxkeyed = 1;
	radio.duplex3 = 999;
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
	assert(usbradio_read(channel) == &ast_null_frame);
	assert(!radio.rxkeyed && !radio.lastrx);
	radio.rxkeyed = radio.plus_advanced = 1;
	assert(usbradio_read(channel) == &ast_null_frame);
	assert(!radio.rxkeyed);
	radio.plus_advanced = 0;

	radio.hasusb = 1;
	mock_oss_io = 1;
	mock_read_result = -1;
	mock_read_errno = EIO;
	assert(usbradio_read(channel) == &ast_null_frame);
	assert(!radio.hasusb);
	radio.hasusb = 1;
	mock_read_errno = EAGAIN;
	radio.readerrs = 0;
	assert(usbradio_read(channel) == &ast_null_frame);
	assert(radio.readerrs == 1 && radio.hasusb);
	radio.readerrs = 1;
	assert(usbradio_read(channel) == &ast_null_frame);
	assert(radio.readerrs == 2 && radio.hasusb);
	radio.readerrs = READERR_THRESHOLD + 1;
	assert(usbradio_read(channel) == &ast_null_frame);
	assert(!radio.hasusb && !radio.readerrs);
	radio.hasusb = 1;
	radio.readpos = AST_FRIENDLY_OFFSET;
	radio.readerrs = 1;
	mock_read_result = 16;
	assert(usbradio_read(channel) == &ast_null_frame);
	assert(radio.readpos == AST_FRIENDLY_OFFSET + 16 && !radio.readerrs);
	mock_read_result = -1;
	mock_oss_io = 0;
	test_channel_private = NULL;
}

/** @brief Drive one complete OSS receiver block through the channel harness.
 * @param radio Named radio profile or signaling state, as declared.
 * @param channel Radio channel or channel index, as declared.
 * @return Result used by the test's assertions.
 */
static struct ast_frame *oss_read_complete(struct chan_usbradio_pvt *radio,
					   struct ast_channel *channel)
{
	struct ast_frame *frame;
	unsigned int inputs = 0U;
	unsigned int writes_before = mock_sound_write_calls;

	/* The fixture's direct receive fields model the state sampled by the HID
	 * worker. Publish that sample before the audio callback imports its atomic
	 * snapshot, just as the real HID worker does. */
	if (radio->rxhidsq)
		inputs |= URP_HARDWARE_INPUT_HID_CARRIER;
	if (radio->rxhidctcss)
		inputs |= URP_HARDWARE_INPUT_HID_CTCSS;
	if (radio->rxppsq)
		inputs |= URP_HARDWARE_INPUT_PARALLEL_CARRIER;
	if (radio->rxppctcss)
		inputs |= URP_HARDWARE_INPUT_PARALLEL_CTCSS;
	usbradioplus_publish_hardware_inputs(radio, inputs);
	mock_read_result = (ssize_t)(sizeof(radio->usbradio_read_buf) - radio->readpos);
	frame = usbradio_read(channel);
	/* Direct rendering fills the app and DAC frame in this callback. */
	assert(mock_sound_write_calls == writes_before + 1);
	assert(mock_sound_write_bytes == sizeof(radio->usbradio_write_buf));
	assert(radio->plus_sound_dropped_frames == 0);
	assert(radio->plus_sound_short_writes == 0);
	return frame;
}

/** @brief Verify oss complete read frame. */
static void test_oss_complete_read_frame(void)
{
	struct chan_usbradio_pvt radio = {0};
	urp_radio_state radio_configuration = {0};
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	struct ast_frame *frame;

	settings_defaults(&settings);
	settings.profiles[0].enabled = 0;
	strcpy(settings.profiles[0].name, "test");
	strcpy(settings.profiles[0].channel, "RadioPlus/test");
	radio.name = "test";
	radio.owner = channel;
	radio.sounddev = 7;
	radio.hasusb = 1;
	radio.radioactive = 1;
	radio.plus_app_rpt_rate = URP_RATE_LINK;
	radio.plus_app_rpt_samples = URP_LINK_SAMPLES;
	radio.plus_hardware_applied = 1;
	radio.plus_deemphasis_corner_hz = 300.0;
	radio.plus_preemphasis_corner_hz = 300.0;
	radio.readpos = AST_FRIENDLY_OFFSET;
	urp_sample_queue_init(&radio.echo_queue, radio.echo_samples, URP_ECHO_QUEUE_SAMPLES);
	radio_configuration.pRxCodeSrc = "100.0";
	radio_configuration.pTxCodeSrc = "0";
	radio_configuration.pTxCodeDefault = "0";
	radio.radio = urp_radio_create(&radio_configuration, URP_LINK_SAMPLES);
	assert(radio.radio);
	assert(usbradioplus_dsp_init(&radio) == 0);
	test_channel_private = &radio;
	mock_oss_io = 1;
	mock_ioctl_failure = ULONG_MAX;
	mock_oss_fragment_total = mock_oss_fragments = 8;
	mock_oss_fragment_size = 3840;
	mock_oss_bytes = -1;
	mock_oss_output_space_calls = 0;
	fail_oss_output_space_call = 0;
	extreme_oss_output_space_call = 0;
	radio.total_blocks = 8;
	radio.queuesize = 7;
	mock_write_result = -2;
	mock_read_errno = 0;
	channel_state = AST_STATE_UP;
	/* A control-plane radio reconfiguration may temporarily deny the callback
	 * access to signaling state.  The real-time path must keep its cadence with
	 * zeroed app_rpt and DAC buffers instead of touching that state or stalling. */
	memset(radio.usbradio_read_buf_8k, 0x5a, sizeof(radio.usbradio_read_buf_8k));
	memset(radio.usbradio_write_buf, 0x5a, sizeof(radio.usbradio_write_buf));
	atomic_store_explicit(&radio.plus_radio_access.reconfiguring, 1, memory_order_seq_cst);
	mock_read_result = (ssize_t)(sizeof(radio.usbradio_read_buf) - radio.readpos);
	{
		unsigned int writes_before = mock_sound_write_calls;

		assert(usbradio_read(channel) == &ast_null_frame);
		assert(radio.readpos == AST_FRIENDLY_OFFSET);
		assert(mock_sound_write_calls == writes_before + 1U);
	}
	for (size_t sample = 0; sample < radio.plus_app_rpt_samples; ++sample)
		assert(radio.usbradio_read_buf_8k[AST_FRIENDLY_OFFSET + sample] == 0);
	for (size_t sample = 0; sample < ARRAY_LEN(radio.usbradio_write_buf); ++sample)
		assert(radio.usbradio_write_buf[sample] == 0);
	assert(!atomic_load_explicit(&radio.plus_radio_tx_active, memory_order_acquire));
	/* A full DAC follows the same no-wait reconfiguration path without writing
	 * an unadmitted frame. */
	mock_oss_bytes = (int)sizeof(radio.usbradio_write_buf) - 1;
	mock_read_result = (ssize_t)(sizeof(radio.usbradio_read_buf) - radio.readpos);
	{
		unsigned int writes_before = mock_sound_write_calls;

		assert(usbradio_read(channel) == &ast_null_frame);
		assert(mock_sound_write_calls == writes_before);
	}
	mock_oss_bytes = -1;
	radio.plus_sound_dropped_frames = 0;
	atomic_store_explicit(&radio.plus_radio_access.reconfiguring, 0, memory_order_seq_cst);
	radio.clipledgpio = 1;
	mock_audio_clipping = 1;
	frame = oss_read_complete(&radio, channel);
	assert(frame == &radio.read_f && frame->frametype == AST_FRAME_VOICE);
	assert(frame->samples == URP_LINK_SAMPLES);
	/* OSS capture posts the same worker-owned clip request as the modern
	 * callback; it must not mutate GPIO timing from the audio path. */
	assert(atomic_load_explicit(&radio.plus_clip_led_request, memory_order_acquire));
	atomic_store_explicit(&radio.plus_clip_led_request, 0, memory_order_release);
	radio.hid_gpio_pulsetimer[0] = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(atomic_load_explicit(&radio.plus_clip_led_request, memory_order_acquire));
	atomic_store_explicit(&radio.plus_clip_led_request, 0, memory_order_release);
	radio.rxcdtype = CD_IGNORE;
	radio.rxsdtype = SD_HID;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.rxcdtype = CD_HID;
	assert(radio.hid_gpio_pulsetimer[0] == 1);
	/* The preceding capture was also clipped, but emulate the worker consuming
	 * that request before checking that a disabled LED posts nothing new. */
	atomic_store_explicit(&radio.plus_clip_led_request, 0, memory_order_release);
	radio.clipledgpio = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!atomic_load_explicit(&radio.plus_clip_led_request, memory_order_acquire));
	ast_set_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	module_debug_level = 10;
	file_debug_level = 0;
	urp_radio_destroy(radio.radio);
	radio_configuration.pRxCodeSrc = "0";
	radio.radio = urp_radio_create(&radio_configuration, URP_LINK_SAMPLES);
	assert(radio.radio);
	mock_audio_clipping = 0;
	channel_state = AST_STATE_DOWN;
	assert(oss_read_complete(&radio, channel) == &ast_null_frame);
	channel_state = AST_STATE_UP;

	/* Hardware COS drives key/unkey signaling independently of the audio DSP. */
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_IGNORE;
	radio.rxhidsq = 1;
	radio.duplex3 = 999;
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.rxkeyed && radio.lastrx);
	radio.rxhidsq = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.rxkeyed && !radio.lastrx);
	/* Mixer routing is untouched when local repeat is disabled. */
	radio.duplex3 = 0;
	radio.rxhidsq = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.rxkeyed && radio.lastrx);
	radio.rxhidsq = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.rxkeyed && !radio.lastrx);
	/* Software local repeat never changes the hardware monitor mixer. */
	radio.duplex3 = 999;
	radio.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	radio.rxhidsq = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.rxkeyed && radio.lastrx);
	radio.rxhidsq = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.rxkeyed && !radio.lastrx);
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
	radio.rxcdtype = CD_HID_INVERT;
	radio.rxhidsq = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.rxkeyed);
	radio.rxcdtype = CD_PP;
	radio.rxppsq = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.rxcdtype = CD_PP_INVERT;
	radio.rxppsq = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.rxcdtype = CD_HID_INVERT;
	radio.rxhidsq = 1;
	radio.radio->rxExtCarrierDetect = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.radio->rxExtCarrierDetect);
	radio.rxcdtype = CD_HID;
	radio.rxhidsq = 1;
	radio.radio->txPttOut = 1;
	radio.radioduplex = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.radio->txPttOut = 0;
	radio.radioduplex = 0;
	module_debug_level = 0;
	file_debug_level = 10;

	/* app_rpt and the tuning utility are the only transmit-key owners. */
	radio.txkeyed = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.radio->txPttIn);
	radio.txkeyed = 0;
	radio.txtestkey = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.radio->txPttIn);
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.radio->txPttIn);
	radio.txtestkey = 0;
	radio.txkeyed = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.radio->txPttIn);
	radio.txoffdelay = 2;
	radio.txkeyed = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.txoffcnt);
	radio.txkeyed = 0;
	radio.txoffcnt = MS_TO_FRAMES(TX_OFF_DELAY_MAX);
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	/* The audio queue can still hold PTT after app_rpt has unkeyed; RX holdoff
	 * begins only after the effective physical request falls. */
	assert(!radio.txoffcnt);
	radio.txoffdelay = 0;
	ast_clear_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	file_debug_level = 0;
	option_debug = 10;

	/* Legacy 8 kHz echo mode clears, records, and plays its queue. */
	{
		short echo[FRAME_SIZE] = {0};

		for (size_t echo_sample = 0; echo_sample < FRAME_SIZE; ++echo_sample)
			assert(urp_sample_queue_push_sample(&radio.echo_queue, echo[echo_sample]));
		radio.echomode = 0;
		assert(oss_read_complete(&radio, channel) == &radio.read_f);
		assert(!urp_sample_queue_samples(&radio.echo_queue));
		for (size_t echo_sample = 0; echo_sample < FRAME_SIZE; ++echo_sample)
			assert(urp_sample_queue_push_sample(&radio.echo_queue, echo[echo_sample]));
		radio.echomode = 1;
		radio.echoing = 0;
		assert(oss_read_complete(&radio, channel) == &radio.read_f);
		assert(radio.echoing);
	}
	radio.rxkeyed = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.echoing);
	radio.rxkeyed = 1;
	radio.echomax = 2;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(urp_sample_queue_samples(&radio.echo_queue) == FRAME_SIZE);
	/* Echo capture caps its duration and safely stops if its SPSC ring is full. */
	urp_sample_queue_reset(&radio.echo_queue);
	radio.echomax = URP_ECHO_QUEUE_SAMPLES / FRAME_SIZE + 1U;
	radio.echo_queue.capacity = 1U;
	usbradioplus_echo_record(&radio, (const short[2]){0, 0}, 2U);
	assert(urp_sample_queue_samples(&radio.echo_queue) == 1U);
	urp_sample_queue_init(&radio.echo_queue, radio.echo_samples, URP_ECHO_QUEUE_SAMPLES);
	/* Exercise every short-circuit and capacity boundary in sample-ring capture. */
	radio.echomax = 0;
	radio.plus_app_rpt_samples = 0;
	atomic_store_explicit(&radio.echoing, 0, memory_order_release);
	usbradioplus_echo_record(&radio, (const short[1]){0}, 1U);
	radio.echomax = 1;
	atomic_store_explicit(&radio.echoing, 1, memory_order_release);
	usbradioplus_echo_record(&radio, (const short[1]){0}, 1U);
	atomic_store_explicit(&radio.echoing, 0, memory_order_release);
	radio.plus_app_rpt_samples = 1;
	usbradioplus_echo_record(&radio, (const short[1]){0}, 1U);
	usbradioplus_echo_record(&radio, (const short[1]){0}, 1U);
	usbradioplus_echo_record(&radio, (const short[1]){0}, 0U);
	urp_sample_queue_init(&radio.echo_queue, radio.echo_samples, URP_ECHO_QUEUE_SAMPLES);
	radio.plus_app_rpt_samples = URP_LINK_SAMPLES;
	radio.echoing = 0;
	radio.rxhidsq = 1;
	radio.radioduplex = 1;
	radio.echomax = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(urp_sample_queue_samples(&radio.echo_queue) == FRAME_SIZE);
	radio.echoing = 1;
	radio.rxcdtype = CD_IGNORE;
	radio.rxsdtype = SD_IGNORE;
	radio.rxkeyed = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.echomode = 0;
	radio.rxhidsq = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.rxcdtype = CD_HID;

	/* Exercise the external CTCSS indication choices and override. */
	radio.rxcdtype = CD_HID;
	radio.rxhidsq = 1;
	radio.rxsdtype = SD_HID;
	radio.rxhidctcss = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.rxsdtype = SD_HID_INVERT;
	radio.rxhidctcss = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.rxsdtype = SD_PP;
	radio.rxppctcss = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.rxsdtype = SD_PP_INVERT;
	radio.rxppctcss = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.rxsdtype = SD_XPMR;
	radio.rxctcssoverride = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.rxctcssoverride = 0;
	radio.rxcdtype = CD_IGNORE;
	radio.rxsdtype = SD_HID;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.rxsdtype = SD_IGNORE;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	/* Carrier alone must not key when an external CTCSS indication is required. */
	radio.rxcdtype = CD_HID;
	radio.rxhidsq = 1;
	radio.rxsdtype = SD_HID;
	radio.rxhidctcss = 0;
	radio.rxkeyed = radio.lastrx = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.rxkeyed);

	/* Receiver on-delay holds keying until the configured frame count. */
	radio.rxsdtype = SD_IGNORE;
	radio.rxcdtype = CD_HID;
	radio.rxhidsq = 1;
	radio.rxkeyed = radio.lastrx = 0;
	radio.rxondelay = 2;
	radio.rxoncnt = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.rxkeyed && radio.rxoncnt == 1);
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.rxkeyed && radio.rxoncnt == 2);
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.rxkeyed);
	/* Transmit holdoff is evaluated independently of receiver on-delay. */
	radio.rxkeyed = radio.lastrx = 0;
	radio.txoffdelay = 2;
	radio.txoffcnt = 0;
	radio.rxoncnt = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.rxkeyed && radio.rxoncnt == 1);
	radio.txoffdelay = 0;
	radio.rxondelay = 0;
	radio.rxctcssdecode = 1;
	strcpy(radio.rxctcssfreq, "100.0");
	radio.rxkeyed = radio.lastrx = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.lastrx);
	radio.rxctcssdecode = 0;
	radio.rxkeyed = radio.lastrx = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.lastrx);

	/* Software duplex echo records the native-rate receive branch. */
	radio.echomode = 1;
	radio.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	radio.rxhidsq = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.rxkeyed);
	radio.echomode = 0;
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;

	/* Status messages are emitted after the audio frame is processed. */
	radio.radio->b.txCtcssReady = 1;
	strcpy(radio.radio->txctcssfreq, "100.0");
	radio.sendvoter = 1;
	radio.count_rssi_update = 1;
	radio.rxkeyed = 1;
	radio.rxhidsq = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.radio->b.txCtcssReady && radio.count_rssi_update == 10);
	radio.count_rssi_update = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.count_rssi_update = 3;
	radio.rxkeyed = 1;
	radio.rxhidsq = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.count_rssi_update == 2);
	radio.count_rssi_update = 1;
	radio.rxkeyed = 0;
	radio.rxhidsq = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);

	/* Asterisk's detector may mute reserved digits, return ordinary digits, or
	 * suppress a duplicate begin frame while retaining the voice frame. */
	radio.usedtmf = 1;
	radio.dsp = NULL;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.dsp = (struct ast_dsp *)(uintptr_t)1;
	dsp_result_type = AST_FRAME_VOICE;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	dsp_result_type = AST_FRAME_DTMF_END;
	dsp_result_digit = 'm';
	frame = oss_read_complete(&radio, channel);
	assert(frame->frametype == AST_FRAME_NULL && !frame->subclass.integer);
	dsp_result_type = AST_FRAME_DTMF_END;
	dsp_result_digit = 'u';
	frame = oss_read_complete(&radio, channel);
	assert(frame->frametype == AST_FRAME_NULL && !frame->subclass.integer);
	dsp_result_type = AST_FRAME_DTMF_END;
	dsp_result_digit = '5';
	option_verbose = 1;
	frame = oss_read_complete(&radio, channel);
	assert(frame->frametype == AST_FRAME_DTMF_END && !radio.toneflag);
	option_verbose = 0;
	dsp_result_type = AST_FRAME_DTMF_END;
	dsp_result_digit = '7';
	frame = oss_read_complete(&radio, channel);
	assert(frame->frametype == AST_FRAME_DTMF_END);
	dsp_result_type = AST_FRAME_DTMF_BEGIN;
	dsp_result_digit = '6';
	radio.toneflag = 0;
	frame = oss_read_complete(&radio, channel);
	assert(frame->frametype == AST_FRAME_DTMF_BEGIN && radio.toneflag);
	frame_free_calls = 0;
	radio.toneflag = 1;
	frame = oss_read_complete(&radio, channel);
	assert(frame == &radio.read_f && frame_free_calls == 1);
	dsp_result_type = -1;
	radio.usedtmf = 0;
	radio.dsp = NULL;

	/* An advanced carrier tick remains PCM even when the app_rpt detector
	 * fixture would otherwise replace it with a DTMF control event. */
	usbradioplus_interface_mode(&radio, 1);
	radio.usedtmf = 1;
	radio.dsp = (struct ast_dsp *)&radio;
	radio.echomode = radio.echoing = 1;
	dsp_result_type = AST_FRAME_DTMF_BEGIN;
	frame = oss_read_complete(&radio, channel);
	assert(frame->frametype == AST_FRAME_VOICE && frame->samples == 960);
	assert(frame->datalen == 1920 &&
	       ast_format_get_sample_rate(frame->subclass.format) == 48000);
	assert(!radio.echoing && !urp_sample_queue_samples(&radio.echo_queue));
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
	radio.duplex3 = 999;
	radio.radioduplex = 0;
	radio.radio->txPttOut = 1;
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_IGNORE;
	radio.rxkeyed = radio.lastrx = 0;
	radio.rxhidsq = 1;
	assert(oss_read_complete(&radio, channel)->frametype == AST_FRAME_VOICE);
	radio.rxhidsq = 0;
	assert(oss_read_complete(&radio, channel)->frametype == AST_FRAME_VOICE);
	dsp_result_type = -1;
	radio.usedtmf = radio.echomode = 0;
	radio.dsp = NULL;
	usbradioplus_interface_mode(&radio, 0);

	/* A short DAC write is counted by the adapter. Direct rendering does not
	 * retain an unacknowledged output record for a later callback. */
	{
		uint64_t short_writes_before = radio.plus_sound_short_writes;

		mock_write_result = 0;
		mock_read_result =
			(ssize_t)(sizeof(radio.usbradio_read_buf) - (size_t)radio.readpos);
		assert(usbradio_read(channel) == &radio.read_f);
		assert(radio.plus_sound_short_writes == short_writes_before + 1U);
		mock_write_result = -2;
	}

	/* An OSS fragment count alone is not enough: reject a short free-space
	 * report before the callback consumes the sole program ring. */
	{
		short program[URP_LINK_SAMPLES];
		size_t available_before;
		uint64_t dropped_before = radio.plus_sound_dropped_frames;
		unsigned int writes_before = mock_sound_write_calls;

		for (size_t sample = 0; sample < ARRAY_LEN(program); ++sample)
			program[sample] = (short)(sample + 1U);
		usbradioplus_queue_program(&radio, program, ARRAY_LEN(program));
		available_before = rpcr_available(&radio.plus_program_ring);
		mock_oss_fragments = mock_oss_fragment_total;
		mock_oss_bytes = (int)sizeof(radio.usbradio_write_buf) - 1;
		mock_read_result =
			(ssize_t)(sizeof(radio.usbradio_read_buf) - (size_t)radio.readpos);
		assert(usbradio_read(channel));
		assert(mock_sound_write_calls == writes_before);
		assert(rpcr_available(&radio.plus_program_ring) == available_before);
		assert(radio.plus_sound_dropped_frames == dropped_before + 1U);

		mock_oss_bytes = -1;
		mock_read_result =
			(ssize_t)(sizeof(radio.usbradio_read_buf) - (size_t)radio.readpos);
		assert(usbradio_read(channel));
		assert(mock_sound_write_calls == writes_before + 1U);
		assert(rpcr_available(&radio.plus_program_ring) < available_before);
	}

	/* Post-PTT blanking overrides hardware COR, and a stale tx-off counter
	 * remains bounded. These terminal probes leave no later test state to alter. */
	radio.plus_sound_dropped_frames = 0;
	radio.plus_sound_short_writes = 0;
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_IGNORE;
	radio.rxhidsq = 1;
	radio.radio->txrxblankingtimer = 100;
	radio.rxcarrierdetect = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.rxcarrierdetect);
	radio.radio->txrxblankingtimer = 0;
	radio.radio->txPttOut = 0;
	radio.txoffdelay = 1;
	radio.txoffcnt = MS_TO_FRAMES(TX_OFF_DELAY_MAX) + 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.txoffcnt == MS_TO_FRAMES(TX_OFF_DELAY_MAX));

	mock_read_result = -1;
	option_debug = 0;
	module_debug_level = file_debug_level = 0;
	mock_oss_io = 0;
	test_channel_private = NULL;
	usbradioplus_dsp_destroy(&radio);
	urp_radio_destroy(radio.radio);
}

/** @brief Verify oss module lifecycle guards. */
static void test_oss_module_lifecycle_guards(void)
{
	struct ast_variable active = {.name = "channel_enabled", .value = "yes"};
	struct chan_usbradio_pvt active_owner = {0};

	fail_format_cap_alloc = 1;
	assert(load_module() == AST_MODULE_LOAD_DECLINE);
	fail_format_cap_alloc = 0;
	hid_mklist_result = 1;
	assert(load_module() == AST_MODULE_LOAD_DECLINE);
	hid_mklist_result = 0;
	test_config_load_result = CONFIG_STATUS_FILEMISSING;
	assert(load_module() == AST_MODULE_LOAD_FAILURE);
	separate_processing_config_result = 1;
	test_processing_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(load_module() == AST_MODULE_LOAD_FAILURE);
	test_processing_config_load_result = (struct ast_config *)(uintptr_t)1;
	test_config_load_result = (struct ast_config *)(uintptr_t)1;
	usbradio_default.next = NULL;
	active.next = NULL;
	test_config_category = "usb";
	test_config_variables = &active;
	ast_strdup_calls = 0;
	fail_ast_strdup_call = 1;
	assert(load_module() == AST_MODULE_LOAD_DECLINE);
	fail_ast_strdup_call = 0;
	usbradio_default.next = NULL;

	separate_processing_config_result = 0;
	test_config_load_calls = 0;
	test_config_load_second_result = CONFIG_STATUS_FILEINVALID;
	test_config_category = "usb";
	assert(load_module() == AST_MODULE_LOAD_DECLINE);
	test_config_load_second_result = NULL;
	settings_defaults(&settings);
	usbradio_default.next = NULL;

	test_config_category = "usb";
	test_config_variables = &active;
	haspp = 0;
	memset(usbradio_default.pps, 0, sizeof(usbradio_default.pps));
	advanced_register_result = -1;
	assert(load_module() == AST_MODULE_LOAD_FAILURE);
	assert(unload_module() == 0);
	usbradio_default.next = NULL;
	advanced_register_result = 0;
	assert(load_module() == AST_MODULE_LOAD_SUCCESS);
	assert(unload_module() == 0);
	usbradio_default.next = NULL;
	channel_register_result = 1;
	assert(load_module() == AST_MODULE_LOAD_FAILURE);
	assert(unload_module() == 0);
	usbradio_default.next = NULL;
	channel_register_result = 0;
	cli_register_result = 1;
	assert(load_module() == AST_MODULE_LOAD_FAILURE);
	cli_register_result = 0;
	assert(unload_module() == 0);
	usbradio_default.next = NULL;
	cli_register_calls = cli_unregister_calls = channel_unregister_calls = 0;
	assert(load_module() == AST_MODULE_LOAD_SUCCESS);
	assert(cli_register_calls >= 2);
	assert(unload_module() == 0);
	assert(cli_unregister_calls >= 2 && channel_unregister_calls >= 1);
	usbradio_default.next = NULL;
	active.next = NULL;
	haspp = 0;
	hasout = 0;
	assert(load_module() == AST_MODULE_LOAD_SUCCESS);
	assert(unload_module() == 0);
	usbradio_default.next = NULL;
	active_owner.name = "active-owner";
	active_owner.sounddev = 7;
	active_owner.dsp = (struct ast_dsp *)(uintptr_t)1;
	active_owner.owner = (struct ast_channel *)(uintptr_t)1;
	usbradio_default.next = &active_owner;
	mock_oss_io = 1;
	assert(unload_module() == -1);
	/* An active Asterisk owner can still enter the native callback.  Failed
	 * unload leaves its device and worker state intact rather than partially
	 * tearing down a live callback path. */
	assert(active_owner.sounddev == 7);
	/* Once ownership is gone, unload performs the complete legacy cleanup: it
	 * closes the OSS device, marks it unavailable, and frees the DTMF detector. */
	active_owner.owner = NULL;
	mock_close_calls = 0;
	ast_dsp_free_calls = 0;
	assert(unload_module() == 0);
	assert(active_owner.sounddev == -1);
	assert(mock_close_calls == 1);
	assert(ast_dsp_free_calls == 1);
	mock_oss_io = 0;
	active_owner.dsp = NULL;
	usbradio_default.next = NULL;
	test_config_category = NULL;
	test_config_variables = NULL;
	test_config_load_result = NULL;
	separate_processing_config_result = 0;
}
#endif

/** @brief Verify squelch copy. */
static void test_native_fifo_and_squelch_copy(void)
{
	struct chan_usbradio_pvt radio = {0};
	short *capture = (short *)(radio.usbradio_read_buf + AST_FRIENDLY_OFFSET);
	size_t i;

	for (i = 0; i < ARRAY_LEN(radio.plus_squelch_native); i++)
		capture[i] = (short)(i - 100);
	usbradioplus_prepare_squelch_audio(&radio);
	assert(memcmp(capture, radio.plus_squelch_native, sizeof(radio.plus_squelch_native)) == 0);
}

/** @brief Render one complete native callback frame into its direct destinations. */
static void native_tick_then_process(struct chan_usbradio_pvt *channel)
{
	usbradioplus_native_tick(channel, 1);
}

/** @brief Render one direct frame for waveform-oriented assertions. */
static void native_tick_then_process_and_consume(struct chan_usbradio_pvt *channel)
{
	native_tick_then_process(channel);
}

/** @brief Verify callback-owned native echo recording, playback, and clearing. */
static void test_parrot_transitions(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio_config = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	struct usbradioplus_native_renderer_stats statistics;
	struct audiostatistics tx_statistics;
	short *capture = (short *)(channel.usbradio_read_buf + AST_FRIENDLY_OFFSET);
	short *transmit = (short *)channel.usbradio_write_buf;
	size_t sample;

	/* No renderer exists before DSP setup, so the public snapshots must fail
	 * rather than exposing a partially initialized diagnostics buffer. */
	assert(usbradioplus_native_renderer_start(NULL) == -1);
	usbradioplus_native_renderer_stop(NULL);
	usbradioplus_native_renderer_stats_reset(NULL);
	usbradioplus_native_renderer_clear_parrot(NULL);
	usbradioplus_native_renderer_clear_legacy_echo(NULL);
	usbradioplus_native_tick(NULL, 1);
	assert(usbradioplus_native_renderer_stats_read(&channel, &statistics) == -1);
	assert(usbradioplus_native_renderer_stats_read(NULL, &statistics) == -1);
	assert(usbradioplus_native_renderer_stats_read(&channel, NULL) == -1);
	assert(usbradioplus_native_renderer_tx_audio_stats_read(&channel, &tx_statistics) == -1);
	assert(usbradioplus_native_renderer_tx_audio_stats_read(NULL, &tx_statistics) == -1);
	assert(usbradioplus_native_renderer_tx_audio_stats_read(&channel, NULL) == -1);
	settings_defaults(&settings);
	strcpy(settings.profiles[0].name, "parrot");
	strcpy(settings.profiles[0].channel, "RadioPlus/parrot");
	settings.profiles[0].enabled = 1;
	channel.name = "parrot";
	channel.rxdemod = RX_AUDIO_FLAT;
	channel.rxkeyed = 1;
	channel.duplex3 = 999;
	channel.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	channel.echomode = 1;
	channel.plus_deemphasis_corner_hz = 300.0;
	channel.plus_preemphasis_corner_hz = 300.0;
	channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
	assert(channel.radio);
	/* Before startup, the callback must retain a silent audio cadence while
	 * preserving the signaling-owned PTT request. */
	atomic_init(&channel.plus_hardware_ptt_request, 0);
	channel.radio->txPttOut = 1;
	usbradioplus_native_tick(&channel, 1);
	assert(atomic_load_explicit(&channel.plus_hardware_ptt_request, memory_order_acquire));
	channel.radio->txPttOut = 0;
	assert(!usbradioplus_dsp_init(&channel));
	/* Starting an already-owned renderer is intentionally idempotent. */
	assert(!usbradioplus_native_renderer_start(&channel));
	usbradioplus_interface_mode(&channel, 0);
	assert(!usbradioplus_prepare_native_processing(&channel));
	/* The callback renders directly. Keep the public hardware route explicit so
	 * the recorded frame is observable at the DAC rather than reaching through
	 * private parrot state. */
	atomic_store_explicit(&channel.plus_applied_txmixa, URP_TX_OUTPUT_VOICE,
			      memory_order_release);
	atomic_store_explicit(&channel.plus_applied_txmixb, URP_TX_OUTPUT_DISABLED,
			      memory_order_release);
	atomic_store_explicit(&channel.plus_applied_tx_output_gain_a, M_Q8, memory_order_release);
	atomic_store_explicit(&channel.plus_applied_tx_output_gain_b, M_Q8, memory_order_release);
	for (sample = 0; sample < URP_NATIVE_SAMPLES; ++sample) {
		capture[2U * sample] = (short)(1000 + sample);
		capture[2U * sample + 1U] = 0;
		channel.radio->rxCarrierGate[sample] = 1;
	}

	/* A qualified local frame starts a fresh callback-owned recording. */
	native_tick_then_process(&channel);
	assert(!usbradioplus_native_renderer_stats_read(&channel, &statistics));
	assert(statistics.native_frames == 1U);
	assert(!usbradioplus_native_renderer_tx_audio_stats_read(&channel, &tx_statistics));
	assert(statistics.parrot_samples == URP_NATIVE_SAMPLES);
	assert(!statistics.parrot_playing);
	assert(!atomic_load_explicit(&channel.echoing, memory_order_acquire));

	/* Releasing RX renders playback in the following callback and publishes the
	 * direct DAC result with its public playback accounting. */
	channel.rxkeyed = 0;
	memset(channel.usbradio_write_buf, 0, sizeof(channel.usbradio_write_buf));
	native_tick_then_process(&channel);
	assert(!usbradioplus_native_renderer_stats_read(&channel, &statistics));
	assert(statistics.parrot_playback_frames == 1U);
	assert(statistics.parrot_samples == URP_NATIVE_SAMPLES);
	assert(!statistics.parrot_playing);
	assert(!atomic_load_explicit(&channel.echoing, memory_order_acquire));
	assert(urp_pcm_peak(transmit, URP_NATIVE_SAMPLES * 2U) > 0U);

	/* Clearing is a control-plane request consumed by the renderer at a frame
	 * boundary; it must discard the recording without callback-side mutation. */
	usbradioplus_native_renderer_clear_parrot(&channel);
	native_tick_then_process(&channel);
	assert(!usbradioplus_native_renderer_stats_read(&channel, &statistics));
	assert(statistics.parrot_samples == 0U);
	assert(!statistics.parrot_playing);
	assert(!atomic_load_explicit(&channel.echoing, memory_order_acquire));

	/* Reset is asynchronous in production and is consumed at a complete renderer
	 * frame boundary.  The request must yield a coherent fresh snapshot, not a
	 * partially cleared live structure. */
	usbradioplus_native_renderer_stats_reset(&channel);
	native_tick_then_process(&channel);
	assert(!usbradioplus_native_renderer_stats_read(&channel, &statistics));
	assert(statistics.native_frames == 1U);
	assert(statistics.parrot_samples == 0U);
	assert(!statistics.parrot_playing);

	/* Exercise successful tuner updates against a ready native renderer. The
	 * lightweight dispatch test deliberately does not start one, so it only
	 * reaches these commands' rollback paths. */
	channel.hasusb = 1;
	channel.txkeyed = 1;
	radio_print_audio_stats_calls = 0;
	tune_menusupport(1, &channel, "Z");
	assert(radio_print_audio_stats_calls == 1);
	channel.txkeyed = 0;
	channel.hasusb = 0;
	channel.echomode = 0;
	tune_menusupport(1, &channel, "k1");
	assert(channel.echomode);
	tune_menusupport(1, &channel, "k0");
	assert(!channel.echomode);
	tune_menusupport(1, &channel, "D500");
	assert(channel.duplex3 == 500);
	tune_menusupport(1, &channel, "M0");
	assert(channel.duplex3mode == DUPLEX3_MODE_HARDWARE);
	tune_menusupport(1, &channel, "M1");
	assert(channel.duplex3mode == DUPLEX3_MODE_SOFTWARE);

	usbradioplus_dsp_destroy(&channel);
	assert(usbradioplus_native_renderer_stats_read(&channel, &statistics) == -1);
	assert(usbradioplus_native_renderer_tx_audio_stats_read(&channel, &tx_statistics) == -1);
	assert(!urp_radio_destroy(channel.radio));
}

/** @brief Verify the shared program ring and parrot storage. */
static void test_program_ring_and_parrot_storage(void)
{
	struct chan_usbradio_pvt radio = {0};
	short samples[200];
	size_t i;

	assert(usbradioplus_ensure_parrot_capacity(NULL) == -1);
	radio.plus_app_rpt_samples = URP_LINK_SAMPLES;
	radio.plus_app_rpt_rate = URP_RATE_LINK;
	assert(!rpcr_init(&radio.plus_program_ring, URP_PROGRAM_RING_SAMPLES, RPCR_SINC_BEST));
	assert(!rpcr_set_rates(&radio.plus_program_ring, URP_RATE_LINK, URP_RATE_NATIVE));
	for (i = 0; i < ARRAY_LEN(samples); ++i)
		samples[i] = (short)i;
	usbradioplus_queue_program(&radio, NULL, ARRAY_LEN(samples));
	usbradioplus_queue_program(&radio, samples, 0);
	assert(rpcr_available(&radio.plus_program_ring) == 0);
	usbradioplus_queue_program(&radio, samples, ARRAY_LEN(samples));
	assert(rpcr_available(&radio.plus_program_ring) == ARRAY_LEN(samples));
	assert(radio.plus_program_ring.storage[159] == 159);
	/* Fill the producer side through its real one-sample publication API. */
	while (rpcr_available(&radio.plus_program_ring) < radio.plus_program_ring.capacity)
		assert(rpcr_producer_push_sample(&radio.plus_program_ring, 0));
	usbradioplus_queue_program(&radio, samples, 1);
	assert(radio.plus_link_queue_overflows == 1);
	assert(rpcr_available(&radio.plus_program_ring) == radio.plus_program_ring.capacity);
	rpcr_destroy(&radio.plus_program_ring);

	assert(!usbradioplus_native_echo(&radio));
	radio.duplex3 = 999;
	assert(!usbradioplus_native_echo(&radio));
	radio.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	assert(usbradioplus_native_echo(&radio));
	/* Before renderer startup, the request safely uses the inactive queue's
	 * consumer endpoint.  A NULL control request is deliberately harmless. */
	usbradioplus_native_renderer_clear_legacy_echo(NULL);
	urp_sample_queue_init(&radio.echo_queue, radio.echo_samples, URP_ECHO_QUEUE_SAMPLES);
	assert(urp_sample_queue_push_sample(&radio.echo_queue, 1));
	atomic_store_explicit(&radio.echoing, 1, memory_order_release);
	usbradioplus_echo_clear(&radio);
	assert(!atomic_load_explicit(&radio.echoing, memory_order_acquire));
	assert(!urp_sample_queue_samples(&radio.echo_queue));

	assert(usbradioplus_ensure_parrot_capacity(&radio) == 0);
	assert(radio.plus_parrot);
	assert(radio.plus_parrot_capacity == (size_t)DEFAULT_ECHO_MAX * URP_NATIVE_SAMPLES);
	assert(usbradioplus_ensure_parrot_capacity(&radio) == 0);
	free(radio.plus_parrot);
	radio.plus_parrot = NULL;
	assert(usbradioplus_ensure_parrot_capacity(&radio) == 0);
	free(radio.plus_parrot);
	radio.plus_parrot = NULL;
	radio.plus_parrot_capacity = 0;
	fail_realloc = 1;
	assert(usbradioplus_ensure_parrot_capacity(&radio) == -1);
	fail_realloc = 0;
}

/** @brief Render app_rpt audio directly from the sole SPSC program ring. */
static void test_program_ring_native_tick(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio_config = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	short program[URP_LINK_SAMPLES];
	struct rpcr_observation observation;
	size_t available_before;

	settings_defaults(&settings);
	strcpy(settings.profiles[0].name, "program-ring");
	strcpy(settings.profiles[0].channel, "RadioPlus/program-ring");
	settings.profiles[0].enabled = 0;
	channel.name = "program-ring";
	channel.plus_app_rpt_rate = URP_RATE_LINK;
	channel.plus_app_rpt_samples = URP_LINK_SAMPLES;
	channel.plus_deemphasis_corner_hz = 300.0;
	channel.plus_preemphasis_corner_hz = 300.0;
	channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
	assert(channel.radio);
	assert(!usbradioplus_dsp_init(&channel));
	usbradioplus_interface_mode(&channel, 0);
	/* DSP initialization establishes atomic control state, so publish the
	 * app_rpt key after that initialization just as the channel control path
	 * does. */
	atomic_store_explicit(&channel.txkeyed, 1, memory_order_release);
	for (size_t i = 0; i < ARRAY_LEN(program); ++i)
		program[i] = (short)(1000 + i);

	/* An empty keyed tick reports a real shortfall. The controller target never
	 * holds subsequent source PCM for a startup reserve. */
	channel.radio->txPttOut = 1;
	native_tick_then_process(&channel);
	assert(channel.plus_link_queue_underflows == 1);
	/* Audio starvation only conceals the DAC frame. The direct renderer publishes
	 * the signaling engine's PTT decision independently of queued program PCM. */
	assert(atomic_load_explicit(&channel.plus_hardware_ptt_request, memory_order_acquire));
	usbradioplus_queue_program(&channel, program, ARRAY_LEN(program));
	available_before = rpcr_available(&channel.plus_program_ring);
	assert(available_before == ARRAY_LEN(program));
	native_tick_then_process(&channel);
	/* One under-target app_rpt block is consumed immediately. A sinc startup
	 * may still conceal its first output samples, but it must not defer the raw
	 * read cursor until the 110 ms drift setpoint is reached. */
	assert(rpcr_available(&channel.plus_program_ring) < available_before);
	rpcr_observe(&channel.plus_program_ring, &observation);
	assert(observation.target_samples == channel.plus_program_target_samples);
	for (unsigned int frame = 0;
	     frame < (channel.plus_program_target_samples / URP_LINK_SAMPLES) + 2U; ++frame)
		usbradioplus_queue_program(&channel, program, ARRAY_LEN(program));
	int rendered = 0;
	for (unsigned int tick = 0; tick < 40U; ++tick) {
		native_tick_then_process(&channel);
		rendered |= channel.plus_link_native[0] != 0 || channel.plus_link_native[1] != 0;
	}
	assert(channel.plus_native_frames == 42);
	assert(!channel.plus_src_errors);
	assert(rendered);
	/* The receiver downsampler has its own fail-closed path. The dynamic
	 * program ring is not involved in this source conversion. */
	src_process_calls = 0;
	fail_src_process_call = 1;
	native_tick_then_process(&channel);
	fail_src_process_call = 0;
	assert(channel.plus_src_errors == 1U);
	/* A nominal conversion with incomplete input consumption is also invalid. */
	{
		unsigned int src_errors = channel.plus_src_errors;

		src_process_calls = 0;
		partial_src_process_call = 1;
		native_tick_then_process(&channel);
		partial_src_process_call = 0;
		assert(channel.plus_src_errors == src_errors + 1U);
	}

	usbradioplus_dsp_destroy(&channel);
	assert(!urp_radio_destroy(channel.radio));
}

/** @brief Verify direct rendering admits program PCM only when the DAC accepts it. */
static void test_native_renderer_transmit_admission(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio_config = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	short program[URP_LINK_SAMPLES];
	short *dac_pcm = (short *)channel.usbradio_write_buf;
	size_t available_before;
	int rendered = 0;

	settings_defaults(&settings);
	strcpy(settings.profiles[0].name, "renderer-admission");
	strcpy(settings.profiles[0].channel, "RadioPlus/renderer-admission");
	settings.profiles[0].enabled = 0;
	channel.name = "renderer-admission";
	channel.plus_app_rpt_rate = URP_RATE_LINK;
	channel.plus_app_rpt_samples = URP_LINK_SAMPLES;
	channel.plus_deemphasis_corner_hz = 300.0;
	channel.plus_preemphasis_corner_hz = 300.0;
	channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
	assert(channel.radio);
	assert(!usbradioplus_dsp_init(&channel));
	usbradioplus_interface_mode(&channel, 0);
	atomic_store_explicit(&channel.txkeyed, 1, memory_order_release);
	channel.radio->txPttOut = 1;
	atomic_store_explicit(&channel.plus_applied_txmixa, URP_TX_OUTPUT_VOICE,
			      memory_order_release);
	atomic_store_explicit(&channel.plus_applied_txmixb, URP_TX_OUTPUT_DISABLED,
			      memory_order_release);
	atomic_store_explicit(&channel.plus_applied_tx_output_gain_a, M_Q8, memory_order_release);
	atomic_store_explicit(&channel.plus_applied_tx_output_gain_b, M_Q8, memory_order_release);
	for (size_t sample = 0; sample < ARRAY_LEN(program); ++sample)
		program[sample] = (short)(1000 + sample);
	for (unsigned int frame = 0; frame < 16U; ++frame)
		usbradioplus_queue_program(&channel, program, ARRAY_LEN(program));
	available_before = rpcr_available(&channel.plus_program_ring);

	/* A full physical device renders neither a stale DAC record nor consumes the
	 * sole SPSC program ring. PTT remains the signaling engine's decision. */
	memset(dac_pcm, 0x5a, URP_NATIVE_SAMPLES * 2U * sizeof(*dac_pcm));
	usbradioplus_native_tick(&channel, 0);
	assert(rpcr_available(&channel.plus_program_ring) == available_before);
	assert(urp_pcm_peak(dac_pcm, URP_NATIVE_SAMPLES * 2U) == 0U);
	assert(atomic_load_explicit(&channel.plus_hardware_ptt_request, memory_order_acquire));

	/* Once the device accepts a frame, the same callback consumes program PCM and
	 * writes its current DAC block directly—there is no output queue or ACK. */
	for (unsigned int frame = 0; frame < 16U; ++frame) {
		usbradioplus_native_tick(&channel, 1);
		rendered |= urp_pcm_peak(dac_pcm, URP_NATIVE_SAMPLES * 2U) != 0U;
	}
	assert(rpcr_available(&channel.plus_program_ring) < available_before);
	assert(rendered);
	assert(channel.plus_native_frames == 17U);

	usbradioplus_dsp_destroy(&channel);
	assert(!urp_radio_destroy(channel.radio));
}

/** @brief Verify native transmit rendering adds no voice filtering after its FFmpeg graph. */
static void test_native_tick_voice_graph_ownership(void)
{
	static const unsigned int frequencies[] = {100U, 6000U};
	const double amplitude = 8192.0;

	for (size_t tone = 0; tone < ARRAY_LEN(frequencies); ++tone) {
		struct chan_usbradio_pvt channel = {0};
		urp_radio_state radio_config = {
			.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
		struct txagc_chain *voice;
		struct usbradioplus_native_graph_set *graphs;
		struct txagc_config final_config;
		struct txagc_avfilter expected_filter;
		short program[URP_NATIVE_SAMPLES];
		double expected[URP_NATIVE_SAMPLES];
		short *transmit = (short *)channel.usbradio_write_buf;

		settings_defaults(&settings);
		strcpy(settings.profiles[0].name, "voice-graph-ownership");
		strcpy(settings.profiles[0].channel, "RadioPlus/voice-graph-ownership");
		settings.profiles[0].enabled = 1;
		voice = &settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY];
		/* Deliberately leave a transparent final graph: this test compares the
		 * graph's direct output to the DAC payload on both sides of the former
		 * 300--3000 Hz fixed band-pass. */
		voice->enabled = 1;
		voice->agc.input_gain_db = 0.0;
		voice->agc.output_gain_db = 0.0;
		voice->agc.equalizer_enabled = 0;
		voice->agc.deesser_enabled = 0;
		voice->agc.agc_enabled = 0;
		voice->agc.expander_enabled = 0;
		voice->agc.compressor_enabled = 0;
		voice->agc.limiter_enabled = 0;
		voice->agc.lookahead_limiter_enabled = 0;
		voice->agc.post_limiter_lowpass_enabled = 0;
		channel.name = "voice-graph-ownership";
		channel.plus_app_rpt_rate = URP_RATE_NATIVE;
		channel.plus_app_rpt_samples = URP_NATIVE_SAMPLES;
		channel.plus_deemphasis_corner_hz = 250.0;
		channel.plus_preemphasis_corner_hz = 500.0;
		channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
		assert(channel.radio);
		assert(!usbradioplus_dsp_init(&channel));
		usbradioplus_interface_mode(&channel, 1);
		channel.txpreemphasis = 0;
		assert(!usbradioplus_prepare_native_processing(&channel));
		atomic_store_explicit(&channel.txkeyed, 1, memory_order_release);
		channel.radio->txPttOut = 1;
		atomic_store_explicit(&channel.plus_applied_txmixa, URP_TX_OUTPUT_VOICE,
				      memory_order_release);
		atomic_store_explicit(&channel.plus_applied_txmixb, URP_TX_OUTPUT_DISABLED,
				      memory_order_release);
		atomic_store_explicit(&channel.plus_applied_tx_output_gain_a, M_Q8,
				      memory_order_release);
		atomic_store_explicit(&channel.plus_applied_tx_output_gain_b, M_Q8,
				      memory_order_release);
		for (size_t sample = 0; sample < ARRAY_LEN(program); ++sample)
			program[sample] =
				(short)lround(amplitude * sin(2.0 * M_PI * frequencies[tone] *
							      sample / URP_RATE_NATIVE));
		for (unsigned int frame = 0; frame < URP_PROGRAM_RING_FRAMES; ++frame)
			usbradioplus_queue_program(&channel, program, ARRAY_LEN(program));
		assert(!channel.plus_link_queue_overflows);
		for (unsigned int tick = 0; tick < 4U; ++tick)
			native_tick_then_process(&channel);

		graphs = usbradioplus_native_graphs_acquire(&channel);
		assert(graphs);
		assert(fabs(graphs->receive_deemphasis.config.emphasis_corner_hz - 250.0) < 0.001);
		assert(fabs(graphs->final.config.emphasis_corner_hz - 500.0) < 0.001);
		final_config = graphs->final.config;
		usbradioplus_native_graphs_release(&channel);
		assert(!final_config.preemphasis_enabled);
		assert(!final_config.post_limiter_lowpass_enabled);
		assert(!final_config.dcs_spectral_shaping_enabled);
		assert(final_config.dcs_spectral_lowpass_hz == 0.0);
		for (size_t sample = 0; sample < ARRAY_LEN(expected); ++sample)
			expected[sample] = channel.plus_link_native[sample];
		/* The native program ring may make a sub-code sinc rounding adjustment,
		 * but it must retain both the low and high spectral probes before the
		 * final FFmpeg graph receives them. */
		assert(urp_double_peak(expected, ARRAY_LEN(expected)) > amplitude * 0.8);
		txagc_avfilter_init(&expected_filter);
		assert(!txagc_avfilter_prepare(&expected_filter, &final_config, URP_RATE_NATIVE));
		assert(!txagc_avfilter_process_prepared(&expected_filter, expected,
							ARRAY_LEN(expected)));
		for (size_t sample = 0; sample < ARRAY_LEN(expected); ++sample) {
			short expected_pcm =
				(short)lrint(fmax(INT16_MIN, fmin(INT16_MAX, expected[sample])));

			/* Routing/quantization may follow the graph, but no native tick stage
			 * may add voice filtering after it. */
			assert(transmit[sample * 2U] == expected_pcm);
			assert(!transmit[sample * 2U + 1U]);
		}
		txagc_avfilter_destroy(&expected_filter);
		usbradioplus_dsp_destroy(&channel);
		assert(!urp_radio_destroy(channel.radio));
	}
}

/** @brief Verify native graph reload retains one reader generation without waiting. */
static void test_native_graph_slot_deferred_reclaim(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio_config = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	struct usbradioplus_native_graph_set *first;
	struct usbradioplus_native_graph_set *second;

	settings_defaults(&settings);
	strcpy(settings.profiles[0].name, "native-slot");
	strcpy(settings.profiles[0].channel, "RadioPlus/native-slot");
	settings.profiles[0].enabled = 0;
	channel.name = "native-slot";
	channel.plus_app_rpt_rate = URP_RATE_LINK;
	channel.plus_app_rpt_samples = URP_LINK_SAMPLES;
	channel.plus_deemphasis_corner_hz = 300.0;
	channel.plus_preemphasis_corner_hz = 300.0;
	channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
	assert(channel.radio);
	/* Teardown callers may have no channel to pin or release. */
	assert(!usbradioplus_native_graphs_acquire(NULL));
	usbradioplus_native_graphs_release(NULL);
	assert(!usbradioplus_dsp_init(&channel));

	/* The retained reader represents a callback that began before publication.
	 * Reload must not wait for it or free its generation. */
	first = usbradioplus_native_graphs_acquire(&channel);
	assert(first);
	assert(atomic_load_explicit(&channel.plus_native_graphs.readers, memory_order_relaxed) ==
	       1U);
	assert(!usbradioplus_prepare_native_processing(&channel));
	second = atomic_load_explicit(&channel.plus_native_graphs.active, memory_order_acquire);
	assert(second && second != first);
	/* The held callback generation remains valid until this reader releases it;
	 * publication must retire rather than reclaim it under the callback. */
	assert(first->app_rpt_rate == URP_RATE_LINK);
	/* A third generation would exceed the bounded retirement allowance, so the
	 * control plane rejects it and leaves the published graph unchanged. */
	assert(usbradioplus_prepare_native_processing(&channel));
	assert(atomic_load_explicit(&channel.plus_native_graphs.active, memory_order_acquire) ==
	       second);
	usbradioplus_native_graphs_release(&channel);
	assert(atomic_load_explicit(&channel.plus_native_graphs.readers, memory_order_relaxed) ==
	       0U);
	/* Once the old callback has drained, the next control-plane preparation
	 * reclaims it and publishes normally without callback participation. */
	assert(!usbradioplus_prepare_native_processing(&channel));
	assert(!channel.plus_native_graphs.owned->next_retired);

	usbradioplus_dsp_destroy(&channel);
	assert(!urp_radio_destroy(channel.radio));
}

/** @brief Exercise native graph transactions without involving a native audio callback. */
static void test_native_graph_transaction_paths(void)
{
	struct chan_usbradio_pvt channel = {0};
	struct chan_usbradio_pvt uninitialized = {.name = "uninitialized"};
	struct chan_usbradio_pvt *saved_channels = usbradio_default.next;
	struct usbradioplus_native_graph_set *held;
	struct usbradioplus_native_graph_transaction *transaction = NULL;
	char *saved_name;
	int saved_ctcss_filter_mode;
	int failure;
	urp_radio_state radio_config = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};

	/* Public transaction helpers must tolerate a missing transaction and no
	 * initialized radio.  Reloading no live graph is still a valid no-op. */
	assert(usbradioplus_stage_all_native_processing(NULL) == -1);
	usbradioplus_discard_native_processing_transaction(NULL);
	usbradioplus_publish_native_processing_transaction(NULL);
	usbradio_default.next = NULL;
	assert(!usbradioplus_stage_all_native_processing(&transaction));
	assert(transaction);
	usbradioplus_discard_native_processing_transaction(transaction);
	transaction = NULL;
	assert(!usbradioplus_prepare_all_native_processing());

	/* A staging allocation failure must leave the caller's output clear. */
	ast_calloc_calls = 0;
	fail_ast_calloc_call = 1;
	assert(usbradioplus_stage_all_native_processing(&transaction) == -1);
	assert(!transaction);
	fail_ast_calloc_call = 0;

	settings_defaults(&settings);
	ast_copy_string(settings.profiles[0].name, "graph-transaction",
			sizeof(settings.profiles[0].name));
	ast_copy_string(settings.profiles[0].channel, "RadioPlus/graph-transaction",
			sizeof(settings.profiles[0].channel));
	channel.name = "graph-transaction";
	channel.plus_app_rpt_rate = URP_RATE_LINK;
	channel.plus_app_rpt_samples = URP_LINK_SAMPLES;
	channel.plus_deemphasis_corner_hz = 300.0;
	channel.plus_preemphasis_corner_hz = 300.0;
	channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
	assert(channel.radio);
	assert(!usbradioplus_dsp_init(&channel));
	/* Interface-mode changes retain the prior graph if preparation fails. */
	processing_composite_get_calls = 0;
	fail_processing_composite_get_call = 1;
	usbradioplus_interface_mode(&channel, 0);
	fail_processing_composite_get_call = 0;

	/* The private builder rejects incomplete call contracts before it allocates or
	 * changes a published graph.  Exercise those guards directly through the
	 * test-only entry point rather than exposing them to an audio callback. */
	assert(usbradioplus_test_native_graph_set_build(NULL, NULL) == -1);
	assert(usbradioplus_test_native_graph_set_build(&channel, NULL) == -1);
	saved_name = channel.name;
	channel.name = NULL;
	assert(usbradioplus_test_native_graph_set_build(&channel, &held) == -1);
	channel.name = saved_name;
	channel.name = "missing-profile";
	assert(usbradioplus_test_native_graph_set_build(&channel, &held) == -1);
	channel.name = saved_name;
	processing_composite_get_calls = 0;
	fail_processing_composite_get_call = 1;
	held = NULL;
	assert(usbradioplus_test_native_graph_set_build(&channel, &held) == -1);
	assert(!held);
	fail_processing_composite_get_call = 0;
	ast_calloc_calls = 0;
	fail_ast_calloc_call = 1;
	assert(usbradioplus_test_native_graph_set_build(&channel, &held) == -1);
	fail_ast_calloc_call = 0;

	/* A failing preparation must destroy the incomplete candidate and preserve the
	 * installed immutable graph.  Each fixed stage has one independent failure
	 * branch, so inject one failure at every preparation position. */
	for (failure = 1; failure <= 6; ++failure) {
		held = NULL;
		avfilter_prepare_calls = 0;
		fail_avfilter_prepare_call = failure;
		assert(usbradioplus_test_native_graph_set_build(&channel, &held) == -1);
		assert(!held);
	}
	fail_avfilter_prepare_call = 0;

	/* Notch mode builds one graph per configured CTCSS code after the six fixed
	 * stages.  Fail that seventh preparation to cover the separate notch cleanup
	 * path without changing production graph behavior. */
	saved_ctcss_filter_mode = settings.profiles[0].chains[TXAGC_LOCAL].agc.ctcss_filter_mode;
	settings.profiles[0].chains[TXAGC_LOCAL].agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	ast_copy_string(channel.rxctcssfreqs, "100.0", sizeof(channel.rxctcssfreqs));
	held = NULL;
	avfilter_prepare_calls = 0;
	fail_avfilter_prepare_call = 7;
	assert(usbradioplus_test_native_graph_set_build(&channel, &held) == -1);
	assert(!held);
	fail_avfilter_prepare_call = 0;
	settings.profiles[0].chains[TXAGC_LOCAL].agc.ctcss_filter_mode = saved_ctcss_filter_mode;
	/* An uninitialized channel is ignored while the initialized one is staged. */
	uninitialized.next = &channel;
	usbradio_default.next = &uninitialized;
	/* The public all-channel helper discards every private graph if one stage
	 * fails, leaving the active generation available to the native callback. */
	avfilter_prepare_calls = 0;
	fail_avfilter_prepare_call = 1;
	assert(usbradioplus_prepare_all_native_processing() == -1);
	fail_avfilter_prepare_call = 0;

	assert(!usbradioplus_stage_all_native_processing(&transaction));
	assert(transaction);
	usbradioplus_discard_native_processing_transaction(transaction);
	transaction = NULL;
	assert(!usbradioplus_stage_all_native_processing(&transaction));
	assert(transaction);
	usbradioplus_publish_native_processing_transaction(transaction);
	transaction = NULL;

	/* The transaction object allocates before its plan array.  Failing the
	 * second allocation leaves the current graph published and intact. */
	ast_calloc_calls = 0;
	fail_ast_calloc_call = 2;
	assert(usbradioplus_stage_all_native_processing(&transaction) == -1);
	assert(!transaction);
	fail_ast_calloc_call = 0;

	/* Speech-spectrum filtering is owned by the configured processing graph.
	 * The DCS-only shaper cannot enter the final voice/telemetry graph. */
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.dcs_spectral_shaping_enabled = 1;
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.dcs_spectral_lowpass_hz = 250.0;
	assert(!usbradioplus_prepare_native_processing(&channel));
	held = usbradioplus_native_graphs_acquire(&channel);
	assert(held);
	assert(!held->final.config.dcs_spectral_shaping_enabled);
	assert(held->final.config.dcs_spectral_lowpass_hz == 0.0);
	usbradioplus_native_graphs_release(&channel);
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.dcs_spectral_shaping_enabled = 0;
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.dcs_spectral_lowpass_hz = 0.0;

	/* A held callback generation bounds retirement.  Staging must reject a
	 * third generation rather than waiting for the reader or leaking graphs. */
	held = usbradioplus_native_graphs_acquire(&channel);
	assert(held);
	assert(!usbradioplus_prepare_native_processing(&channel));
	assert(usbradioplus_stage_all_native_processing(&transaction) == -1);
	assert(!transaction);
	usbradioplus_native_graphs_release(&channel);
	assert(!usbradioplus_prepare_all_native_processing());

	usbradio_default.next = saved_channels;
	usbradioplus_dsp_destroy(&channel);
	assert(!urp_radio_destroy(channel.radio));
	settings_defaults(&settings);
}

/** One control-plane parser reconfiguration executed beside a held audio reader. */
struct radio_reconfigure_thread_context {
	/** Channel whose parser-owned signaling state is reconfigured. */
	struct chan_usbradio_pvt *channel;
	/** Set before entering the control-plane reconfiguration call. */
	_Atomic int started;
	/** Set only after radio_config() has left the exclusion window. */
	_Atomic int completed;
	/** Reconfiguration status returned to the test thread. */
	int result;
};

/** @brief Run parser reconfiguration on a real control-plane thread. */
static void *run_radio_reconfigure_thread(void *opaque)
{
	struct radio_reconfigure_thread_context *context = opaque;

	atomic_store_explicit(&context->started, 1, memory_order_release);
	context->result = radio_config(context->channel);
	atomic_store_explicit(&context->completed, 1, memory_order_release);
	return NULL;
}

/** @brief Verify parser reconfiguration excludes, but never blocks, audio readers. */
static void test_radio_access_reconfigure_exclusion(void)
{
	struct chan_usbradio_pvt channel = {0};
	struct chan_usbradio_pvt missing_radio = {0};
	urp_radio_state radio_config = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	struct radio_reconfigure_thread_context context = {.channel = &channel};
	pthread_t thread;
	unsigned int spin;

	settings_defaults(&settings);
	strcpy(settings.profiles[0].name, "parser-reconfigure");
	strcpy(settings.profiles[0].channel, "RadioPlus/parser-reconfigure");
	settings.profiles[0].enabled = 0;
	channel.name = "parser-reconfigure";
	channel.plus_app_rpt_rate = URP_RATE_LINK;
	channel.plus_app_rpt_samples = URP_LINK_SAMPLES;
	channel.plus_deemphasis_corner_hz = 300.0;
	channel.plus_preemphasis_corner_hz = 300.0;
	channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
	assert(channel.radio);
	/* Lightweight construction and teardown do not use the reader counter.
	 * Null and radio-less callers must fail closed without touching it. */
	assert(!usbradioplus_radio_access_acquire(NULL));
	usbradioplus_radio_access_release(NULL);
	assert(!usbradioplus_radio_access_acquire(&missing_radio));
	usbradioplus_radio_access_release(&missing_radio);
	assert(usbradioplus_radio_access_acquire(&channel));
	usbradioplus_radio_access_release(&channel);
	assert(!usbradioplus_dsp_init(&channel));

	/* The held reference models a callback already using decoder-owned memory.
	 * The reconfigure thread must set its exclusion flag and wait for this reader,
	 * while a new callback immediately declines the unsafe parser span. */
	assert(usbradioplus_radio_access_acquire(&channel));
	strcpy(channel.rxctcssfreqs, "100.0");
	strcpy(channel.txctcssfreqs, "100.0");
	strcpy(channel.txctcssdefault, "100.0");
	assert(!pthread_create(&thread, NULL, run_radio_reconfigure_thread, &context));
	for (spin = 0U;
	     spin < 1000000U &&
	     !atomic_load_explicit(&channel.plus_radio_access.reconfiguring, memory_order_acquire);
	     ++spin)
		sched_yield();
	assert(atomic_load_explicit(&context.started, memory_order_acquire));
	assert(atomic_load_explicit(&channel.plus_radio_access.reconfiguring,
				    memory_order_acquire));
	assert(atomic_load_explicit(&channel.plus_radio_access.readers, memory_order_acquire) ==
	       1U);
	assert(!atomic_load_explicit(&context.completed, memory_order_acquire));
	assert(!usbradioplus_radio_access_acquire(&channel));

	/* Release lets the control plane finish its parse; no callback has to wait
	 * on the writer, and newly admitted readers observe the completed state. */
	usbradioplus_radio_access_release(&channel);
	assert(!pthread_join(thread, NULL));
	assert(atomic_load_explicit(&context.completed, memory_order_acquire));
	assert(!context.result);
	assert(!atomic_load_explicit(&channel.plus_radio_access.reconfiguring,
				     memory_order_acquire));
	assert(atomic_load_explicit(&channel.plus_radio_access.readers, memory_order_acquire) ==
	       0U);
	assert(!strcmp(channel.radio->pRxCodeSrc, "100.0"));
	assert(usbradioplus_radio_access_acquire(&channel));
	usbradioplus_radio_access_release(&channel);

	usbradioplus_dsp_destroy(&channel);
	assert(!urp_radio_destroy(channel.radio));
}

/** @brief Exercise native-tick policies that are independent of program-ring pacing. */
static void test_native_tick_processing_edges(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio_config = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	short program[URP_NATIVE_SAMPLES];
	short *capture = (short *)(channel.usbradio_read_buf + AST_FRIENDLY_OFFSET);
	short *transmit = (short *)channel.usbradio_write_buf;
	const unsigned int configured_ctcss_peak =
		(unsigned int)lround(32767.0 * pow(10.0, -12.0 / 20.0));
	unsigned long rendered_ctcss_peak;

	settings_defaults(&settings);
	strcpy(settings.profiles[0].name, "tick-edges");
	strcpy(settings.profiles[0].channel, "RadioPlus/tick-edges");
	settings.profiles[0].enabled = 1;
	settings.profiles[0].chains[TXAGC_LOCAL].enabled = 1;
	settings.profiles[0].chains[TXAGC_LOCAL].rnnoise_enabled = 1;
	settings.profiles[0].chains[TXAGC_LOCAL].ctcss_filter_configured = 1;
	settings.profiles[0].chains[TXAGC_LOCAL].agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	settings.profiles[0].chains[TXAGC_LOCAL].agc.ctcss_notch_width_hz = 5.0;
	channel.rxcdtype = CD_XPMR_NOISE;
	channel.name = "tick-edges";
	channel.rxdemod = RX_AUDIO_FLAT;
	channel.rxkeyed = 1;
	channel.duplex3 = 999;
	channel.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	channel.txpreemphasis = 1;
	channel.plus_deemphasis_corner_hz = 300.0;
	channel.plus_preemphasis_corner_hz = 300.0;
	channel.plus_app_rpt_rate = URP_RATE_NATIVE;
	channel.plus_app_rpt_samples = URP_NATIVE_SAMPLES;
	strcpy(channel.rxctcssfreq, "100.0");
	strcpy(channel.rxctcssfreqs, "100.0");
	channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
	assert(channel.radio);
	assert(!usbradioplus_dsp_init(&channel));
	urp_sample_queue_init(&channel.echo_queue, channel.echo_samples, URP_ECHO_QUEUE_SAMPLES);
	/* Exercise native-rate PCM while retaining the legacy echo/parrot branch. */
	usbradioplus_interface_mode(&channel, 0);
	atomic_store_explicit(&channel.txkeyed, 1, memory_order_release);
	channel.plus_app_rpt_rate = URP_RATE_NATIVE;
	channel.plus_app_rpt_samples = URP_NATIVE_SAMPLES;
	channel.plus_program_target_samples =
		(URP_RATE_NATIVE * URP_PROGRAM_RING_TARGET_MS + 999U) / 1000U;
	assert(!rpcr_set_rates(&channel.plus_program_ring, URP_RATE_NATIVE, URP_RATE_NATIVE));
	assert(!usbradioplus_prepare_native_processing(&channel));
	/* A native callback never waits for the setup thread. An odd publication
	 * generation falls back to one atomic routing snapshot after two retries. */
	atomic_store_explicit(&channel.plus_hardware_generation, 1U, memory_order_release);
	native_tick_then_process(&channel);
	atomic_store_explicit(&channel.plus_hardware_generation, 0U, memory_order_release);
	/* A graph reload may briefly leave no published generation. The callback
	 * must discard that frame rather than dereference a retired graph. */
	{
		struct usbradioplus_native_graph_set *active = atomic_exchange_explicit(
			&channel.plus_native_graphs.active, NULL, memory_order_seq_cst);
		urp_radio_state *saved_radio = channel.radio;

		assert(active);
		memset(channel.usbradio_read_buf_8k + AST_FRIENDLY_OFFSET, 0x5a,
		       URP_NATIVE_SAMPLES * sizeof(short));
		memset(channel.usbradio_write_buf, 0x5a, sizeof(channel.usbradio_write_buf));
		/* A normal signaling state retains its PTT decision while graphs reload. */
		usbradioplus_native_tick(&channel, 1);
		assert(urp_pcm_peak((short *)(channel.usbradio_read_buf_8k + AST_FRIENDLY_OFFSET),
				    URP_NATIVE_SAMPLES) == 0U);
		assert(urp_pcm_peak((short *)channel.usbradio_write_buf, URP_NATIVE_SAMPLES * 2U) ==
		       0U);
		/* Teardown can remove the radio state during that same unpublished span. */
		channel.radio = NULL;
		usbradioplus_native_tick(&channel, 1);
		assert(urp_pcm_peak((short *)(channel.usbradio_read_buf_8k + AST_FRIENDLY_OFFSET),
				    URP_NATIVE_SAMPLES) == 0U);
		assert(urp_pcm_peak((short *)channel.usbradio_write_buf, URP_NATIVE_SAMPLES * 2U) ==
		       0U);
		channel.radio = saved_radio;
		atomic_store_explicit(&channel.plus_native_graphs.active, active,
				      memory_order_seq_cst);
	}
	/* A teardown race can similarly remove the radio state before the input
	 * snapshot. The callback still consumes silence and leaves PTT unasserted. */
	{
		urp_radio_state *saved_radio = channel.radio;

		channel.radio = NULL;
		atomic_store_explicit(&channel.plus_hardware_ptt_request, 1, memory_order_release);
		usbradioplus_native_tick(&channel, 1);
		assert(!atomic_load_explicit(&channel.plus_hardware_ptt_request,
					     memory_order_acquire));
		channel.radio = saved_radio;
	}
	/* Teardown can clear only the optional decoder while radio signaling remains
	 * available to the callback. */
	{
		urp_ctcss_decoder *saved_ctcss = channel.radio->rxCtcss;
		struct usbradioplus_native_graph_set *graphs = atomic_load_explicit(
			&channel.plus_native_graphs.active, memory_order_acquire);
		int saved_decode = saved_ctcss->decode;
		int saved_notch_configured;

		assert(graphs);
		saved_notch_configured = graphs->ctcss_notch[0].configured;
		graphs->ctcss_notch[0].configured = 1;
		observed_avfilter_process_state = &graphs->ctcss_notch[0];
		observed_avfilter_process_calls = 0U;
		channel.radio->rxCtcss = NULL;
		native_tick_then_process(&channel);
		assert(observed_avfilter_process_calls == 0U);
		/* A valid decoded index with no prepared notch must also bypass it. */
		channel.radio->rxCtcss = saved_ctcss;
		channel.radio->rxCtcss->decode = 0;
		graphs->ctcss_notch[0].configured = 0;
		native_tick_then_process(&channel);
		assert(observed_avfilter_process_calls == 0U);
		channel.radio->rxCtcss->decode = saved_decode;
		observed_avfilter_process_state = NULL;
		graphs->ctcss_notch[0].configured = saved_notch_configured;
	}
	channel.radio->rxCtcss->decode = urp_ctcss_frequency_index(100.0F);
	for (size_t i = 0; i < URP_NATIVE_SAMPLES; ++i) {
		capture[2U * i] = (short)(1000 + i);
		program[i] = (short)(2000 + i);
		channel.radio->rxCarrierGate[i] = 1;
	}
	for (unsigned int frame = 0;
	     frame < channel.plus_program_target_samples / URP_NATIVE_SAMPLES + 1U; ++frame)
		usbradioplus_queue_program(&channel, program, ARRAY_LEN(program));
	/* Decoder state is normally a valid table index. An out-of-range value must
	 * bypass the selected PL-notch graph without indexing past its fixed bank. */
	channel.radio->rxCtcss->decode = CTCSS_NUM_CODES;
	native_tick_then_process(&channel);
	channel.radio->rxCtcss->decode = urp_ctcss_frequency_index(100.0F);
	option_debug = 5;
	native_tick_then_process(&channel);
	option_debug = 0;
	ast_set_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	module_debug_level = 5;
	native_tick_then_process(&channel);
	module_debug_level = 0;
	file_debug_level = 5;
	native_tick_then_process(&channel);
	file_debug_level = 0;
	/* Module-debug enabled with neither threshold satisfied. */
	native_tick_then_process(&channel);
	ast_clear_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	{
		struct rpcr_observation observation;

		rpcr_observe(&channel.plus_program_ring, &observation);
		/* Native-rate playout bypasses sinc conversion, not clock recovery. */
		assert(observation.target_samples == channel.plus_program_target_samples);
	}

	/* Test-tone generation bypasses dynamics but not the native output path. */
	channel.plus_test_tone_enabled = 1;
	native_tick_then_process(&channel);
	assert(channel.plus_test_tone_phase > 0.0);
	channel.plus_test_tone_enabled = 0;

	/* The DCS-only shaper cannot affect transmitter voice audio. */
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.dcs_spectral_shaping_enabled = 1;
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.dcs_spectral_lowpass_hz = 250.0;
	assert(!usbradioplus_prepare_native_processing(&channel));
	native_tick_then_process(&channel);
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.dcs_spectral_shaping_enabled = 0;
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.dcs_spectral_lowpass_hz = 0.0;

	/* CPU saving bypasses optional dynamics only while receive is unqualified. */
	channel.rxcpusaver = 1;
	channel.rxkeyed = 0;
	assert(!usbradioplus_prepare_native_processing(&channel));
	native_tick_then_process(&channel);
	channel.rxkeyed = 1;
	native_tick_then_process(&channel);
	channel.rxcpusaver = 0;
	channel.rxkeyed = 1;
	assert(!usbradioplus_prepare_native_processing(&channel));
	/* Bypass and error paths reset state without altering the program ring. */
	settings.profiles[0].chains[TXAGC_LOCAL].rnnoise_enabled = 0;
	assert(!usbradioplus_prepare_native_processing(&channel));
	native_tick_then_process(&channel);
	settings.profiles[0].chains[TXAGC_LOCAL].rnnoise_enabled = 1;
	assert(!usbradioplus_prepare_native_processing(&channel));
	src_process_calls = 0;
	fail_src_process_call = 1;
	native_tick_then_process(&channel);
	{
		struct usbradioplus_native_renderer_stats statistics;

		assert(!usbradioplus_native_renderer_stats_read(&channel, &statistics));
		assert(statistics.rnnoise_errors);
	}
	fail_src_process_call = 0;
	/* Cover each native control choice without involving hardware state. */
	channel.rxsquelchdelay = 1;
	assert(!usbradioplus_prepare_native_processing(&channel));
	/* Route signaling alone so CTCSS and DCS behavior is verified at the DAC,
	 * rather than by reaching into callback-owned oscillators. */
	atomic_store_explicit(&channel.plus_applied_txmixa, URP_TX_OUTPUT_TONE,
			      memory_order_release);
	atomic_store_explicit(&channel.plus_applied_txmixb, URP_TX_OUTPUT_DISABLED,
			      memory_order_release);
	atomic_store_explicit(&channel.plus_applied_tx_output_gain_a, M_Q8, memory_order_release);
	atomic_store_explicit(&channel.plus_applied_tx_output_gain_b, M_Q8, memory_order_release);
	channel.radio->txCtcssEnabled = 1;
	channel.radio->txCtcssFreq10 = 1000;
	channel.radio->txCtcssPeak = configured_ctcss_peak;
	channel.radio->b.txCtcssOff = 0;
	/* CTCSS peak is defined at the PCM renderer, not by the CM119 output gain.
	 * Render a full oscillator cycle so the sampled peak is exact. */
	rendered_ctcss_peak = 0;
	for (unsigned int frame = 0; frame < 40U; ++frame) {
		unsigned long peak;

		memset(channel.usbradio_write_buf, 0, sizeof(channel.usbradio_write_buf));
		native_tick_then_process_and_consume(&channel);
		peak = urp_pcm_peak(transmit, URP_NATIVE_SAMPLES * 2U);
		if (peak > rendered_ctcss_peak)
			rendered_ctcss_peak = peak;
	}
	assert(rendered_ctcss_peak >= configured_ctcss_peak - 1U);
	assert(rendered_ctcss_peak <= configured_ctcss_peak + 1U);
	atomic_store_explicit(&channel.plus_applied_tx_output_gain_a, 64, memory_order_release);
	rendered_ctcss_peak = 0;
	for (unsigned int frame = 0; frame < 40U; ++frame) {
		unsigned long peak;

		memset(channel.usbradio_write_buf, 0, sizeof(channel.usbradio_write_buf));
		native_tick_then_process_and_consume(&channel);
		peak = urp_pcm_peak(transmit, URP_NATIVE_SAMPLES * 2U);
		if (peak > rendered_ctcss_peak)
			rendered_ctcss_peak = peak;
	}
	assert(rendered_ctcss_peak >= configured_ctcss_peak - 1U);
	assert(rendered_ctcss_peak <= configured_ctcss_peak + 1U);
	channel.radio->txCtcssTailToneHz = 55.0;
	memset(channel.usbradio_write_buf, 0, sizeof(channel.usbradio_write_buf));
	native_tick_then_process_and_consume(&channel);
	assert(urp_pcm_peak(transmit, URP_NATIVE_SAMPLES * 2U) > 0U);
	channel.radio->b.txCtcssOff = 1;
	memset(channel.usbradio_write_buf, 0, sizeof(channel.usbradio_write_buf));
	native_tick_then_process_and_consume(&channel);
	assert(urp_pcm_peak(transmit, URP_NATIVE_SAMPLES * 2U) == 0U);
	channel.radio->txCtcssEnabled = 0;
	channel.radio->b.txCtcssOff = 0;
	native_tick_then_process_and_consume(&channel);
	channel.radio->txCtcssEnabled = 1;
	channel.radio->txCtcssTailToneHz = 0.0;
	channel.radio->b.txCtcssOff = 1;
	native_tick_then_process_and_consume(&channel);
	channel.radio->txCtcssEnabled = 0;
	channel.rxsquelchdelay = 0;
	assert(!usbradioplus_prepare_native_processing(&channel));
	/* Exercise normal DCS, its end-of-transmission tone, and the silent PTT
	 * cleanup interval without changing any CTCSS state. Route only signaling
	 * to output A so the rendered waveform is deterministic. */
	channel.radio->dcs.enabled_transmit = 1;
	channel.radio->dcsPeak = 1000.0;
	channel.radio->dcs.transmit_code = 23;
	channel.radio->dcs.transmit_inverted = 0;
	channel.radio->txPttOut = 1;
	atomic_store_explicit(&channel.plus_applied_txmixa, URP_TX_OUTPUT_TONE,
			      memory_order_release);
	atomic_store_explicit(&channel.plus_applied_txmixb, URP_TX_OUTPUT_DISABLED,
			      memory_order_release);
	channel.radio->txState = CHAN_TXSTATE_ACTIVE;
	native_tick_then_process_and_consume(&channel);
	/* Changing only DCS polarity must rebuild the renderer encoder even when the
	 * configured code stays the same. */
	channel.radio->dcs.transmit_inverted = 1;
	native_tick_then_process_and_consume(&channel);
	channel.radio->txState = CHAN_TXSTATE_TOC;
	channel.radio->dcsTurnoffTimer = 1;
	memset(channel.usbradio_write_buf, 0, sizeof(channel.usbradio_write_buf));
	native_tick_then_process_and_consume(&channel);
	assert(urp_pcm_peak(transmit, URP_NATIVE_SAMPLES * 2U) > 0U);
	channel.radio->dcsTurnoffTimer = 0;
	/* TOC state alone is not enough to emit the DCS turnoff sequence. */
	native_tick_then_process_and_consume(&channel);
	channel.radio->txState = CHAN_TXSTATE_ACTIVE;
	memset(channel.usbradio_write_buf, 0, sizeof(channel.usbradio_write_buf));
	native_tick_then_process_and_consume(&channel);
	assert(urp_pcm_peak(transmit, URP_NATIVE_SAMPLES * 2U) > 0U);
	/* Inject DCS and final-graph failures without suppressing independent
	 * signaling routes. */
	{
		struct usbradioplus_native_graph_set *graphs =
			usbradioplus_native_graphs_acquire(&channel);

		assert(graphs);
		fail_avfilter_process_state = &graphs->dcs;
		memset(channel.usbradio_write_buf, 0, sizeof(channel.usbradio_write_buf));
		native_tick_then_process_and_consume(&channel);
		assert(urp_pcm_peak(transmit, URP_NATIVE_SAMPLES * 2U) == 0U);
		fail_avfilter_process_state = &graphs->final;
		native_tick_then_process_and_consume(&channel);
		fail_avfilter_process_state = NULL;
		usbradioplus_native_graphs_release(&channel);
	}
	/* The signaling engine holds PTT during finishing. Its normal shaping
	 * graph may still contain history, but it must not put a new DCS word on
	 * the air after the tail has completed. */
	channel.radio->txState = CHAN_TXSTATE_FINISHING;
	for (unsigned int finish_frame = 0; finish_frame < 3U; ++finish_frame) {
		memset(channel.usbradio_write_buf, 0, sizeof(channel.usbradio_write_buf));
		native_tick_then_process_and_consume(&channel);
		assert(urp_pcm_peak(transmit, URP_NATIVE_SAMPLES * 2U) == 0U);
	}
	channel.radio->txPttOut = 0;
	memset(channel.usbradio_write_buf, 0, sizeof(channel.usbradio_write_buf));
	native_tick_then_process_and_consume(&channel);
	assert(urp_pcm_peak(transmit, URP_NATIVE_SAMPLES * 2U) == 0U);
	channel.radio->dcs.enabled_transmit = 0;
	channel.radio->txPttOut = 1;
	native_tick_then_process(&channel);
	atomic_store_explicit(&channel.plus_applied_txmixa, URP_TX_OUTPUT_DISABLED,
			      memory_order_release);
	atomic_store_explicit(&channel.plus_applied_txmixb, URP_TX_OUTPUT_DISABLED,
			      memory_order_release);
	settings.profiles[0].chains[TXAGC_LOCAL].enabled = 0;
	assert(!usbradioplus_prepare_native_processing(&channel));
	native_tick_then_process(&channel);
	settings.profiles[0].chains[TXAGC_LOCAL].enabled = 1;
	assert(!usbradioplus_prepare_native_processing(&channel));
	channel.usedtmf = 1;
	channel.dsp = NULL;
	native_tick_then_process(&channel);
	channel.dsp = (struct ast_dsp *)&channel;
	channel.toneflag = 0;
	native_tick_then_process(&channel);
	channel.toneflag = 1;
	native_tick_then_process(&channel);
	channel.usedtmf = 0;
	channel.dsp = NULL;
	/* Every native FFmpeg graph is prepared before this callback begins. Make
	 * the next FFmpeg frame allocation fail and prove direct rendering does not
	 * allocate while it processes the current hardware block. */
	av_frame_alloc_calls = 0;
	fail_av_frame_alloc_call = 1;
	usbradioplus_native_tick(&channel, 1);
	assert(!av_frame_alloc_calls);
	fail_av_frame_alloc_call = 0;
	channel.echomode = 1;
	assert(!usbradioplus_prepare_native_processing(&channel));
	assert(!usbradioplus_ensure_parrot_capacity(&channel));
	native_tick_then_process(&channel);
	{
		struct usbradioplus_native_renderer_stats statistics;

		assert(!usbradioplus_native_renderer_stats_read(&channel, &statistics));
		assert(statistics.parrot_samples == URP_NATIVE_SAMPLES);
		assert(!statistics.parrot_playing);
	}
	channel.rxkeyed = 0;
	memset(channel.usbradio_write_buf, 0, sizeof(channel.usbradio_write_buf));
	native_tick_then_process(&channel);
	{
		struct usbradioplus_native_renderer_stats statistics;

		assert(!usbradioplus_native_renderer_stats_read(&channel, &statistics));
		assert(statistics.parrot_playback_frames == 1U);
		assert(!statistics.parrot_playing);
		assert(!atomic_load_explicit(&channel.echoing, memory_order_acquire));
	}
	/* A multi-frame recording remains renderer-owned until all of its rendered
	 * frames are consumed. Exercise that public state rather than constructing
	 * private parrot cursors in the callback fixture. */
	channel.rxkeyed = 1;
	native_tick_then_process(&channel);
	native_tick_then_process(&channel);
	channel.rxkeyed = 0;
	native_tick_then_process(&channel);
	{
		struct usbradioplus_native_renderer_stats statistics;

		assert(!usbradioplus_native_renderer_stats_read(&channel, &statistics));
		assert(statistics.parrot_samples == 2U * URP_NATIVE_SAMPLES);
		assert(statistics.parrot_playing);
		assert(atomic_load_explicit(&channel.echoing, memory_order_acquire));
	}
	/* The diagnostics command reads the renderer snapshot, including a live
	 * native-echo playback state, rather than peeking at callback-private data. */
	{
		struct ast_cli_entry entry = {0};
		const char *stats_args[] = {"radioplus", "native", "stats"};
		struct ast_cli_args arguments = {.fd = 1, .argc = 3, .argv = stats_args};
		struct chan_usbradio_pvt *saved_channels = usbradio_default.next;
		char *saved_active = usbradio_active;

		usbradio_default.next = &channel;
		usbradio_active = channel.name;
		assert(handle_radioplus_native_stats(&entry, 0, &arguments) == CLI_SUCCESS);
		usbradio_default.next = saved_channels;
		usbradio_active = saved_active;
	}
	native_tick_then_process(&channel);
	{
		struct usbradioplus_native_renderer_stats statistics;

		assert(!usbradioplus_native_renderer_stats_read(&channel, &statistics));
		assert(!statistics.parrot_playing);
		assert(!atomic_load_explicit(&channel.echoing, memory_order_acquire));
	}

	/* A legacy echo clear is a renderer-boundary request, not a direct reset of
	 * both ring cursors. The queued prefix is retained until the renderer handles
	 * it, then a fresh producer frame remains playable. */
	urp_sample_queue_reset(&channel.echo_queue);
	for (size_t sample = 0; sample < URP_NATIVE_SAMPLES; ++sample)
		assert(urp_sample_queue_push_sample(&channel.echo_queue, program[sample]));
	atomic_store_explicit(&channel.echoing, 1, memory_order_release);
	usbradioplus_echo_clear(&channel);
	assert(!atomic_load_explicit(&channel.echoing, memory_order_acquire));
	assert(urp_sample_queue_samples(&channel.echo_queue) == URP_NATIVE_SAMPLES);
	native_tick_then_process(&channel);
	assert(!urp_sample_queue_samples(&channel.echo_queue));
	for (size_t sample = 0; sample < URP_NATIVE_SAMPLES; ++sample)
		assert(urp_sample_queue_push_sample(&channel.echo_queue, program[sample]));
	atomic_store_explicit(&channel.echoing, 1, memory_order_release);
	native_tick_then_process(&channel);
	assert(!memcmp(channel.plus_link_native, program, sizeof(program)));
	atomic_store_explicit(&channel.echoing, 0, memory_order_release);

	/* Legacy echo has a native-rate fast path. It must copy the complete
	 * source block without involving the resampler. */
	urp_sample_queue_reset(&channel.echo_queue);
	for (size_t sample = 0; sample < URP_NATIVE_SAMPLES; ++sample)
		assert(urp_sample_queue_push_sample(&channel.echo_queue, program[sample]));
	atomic_store_explicit(&channel.echoing, 1, memory_order_release);
	native_tick_then_process(&channel);
	assert(!memcmp(channel.plus_link_native, program, sizeof(program)));
	atomic_store_explicit(&channel.echoing, 0, memory_order_release);

	/* The non-native path uses the ring's one persistent converter and resumes
	 * from its unchanged source PCM after a conversion failure. */
	settings.profiles[0].chains[TXAGC_LOCAL].rnnoise_enabled = 0;
	usbradioplus_interface_mode(&channel, 0);
	/* An empty legacy echo source is an underrun only while the controller has
	 * keyed transmit; it still resets the echo SRC in either state. */
	urp_sample_queue_reset(&channel.echo_queue);
	unsigned int underflows = channel.plus_link_queue_underflows;
	atomic_store_explicit(&channel.echoing, 1, memory_order_release);
	native_tick_then_process(&channel);
	assert(channel.plus_link_queue_underflows == underflows + 1U);
	/* A converter error and a short input-consumption result are both fail-closed
	 * echo cases. The receive downsampler is the first SRC call in each tick;
	 * the echo converter is deliberately the second. */
	urp_sample_queue_reset(&channel.echo_queue);
	for (size_t sample = 0; sample < URP_LINK_SAMPLES; ++sample)
		assert(urp_sample_queue_push_sample(&channel.echo_queue, program[sample]));
	unsigned int src_errors = channel.plus_src_errors;
	src_process_calls = 0;
	fail_src_process_call = 2;
	atomic_store_explicit(&channel.echoing, 1, memory_order_release);
	native_tick_then_process(&channel);
	fail_src_process_call = 0;
	assert(channel.plus_src_errors == src_errors + 1U);
	for (size_t sample = 0; sample < URP_NATIVE_SAMPLES; ++sample)
		assert(channel.plus_link_native[sample] == 0);

	urp_sample_queue_reset(&channel.echo_queue);
	for (size_t sample = 0; sample < URP_LINK_SAMPLES; ++sample)
		assert(urp_sample_queue_push_sample(&channel.echo_queue, program[sample]));
	src_errors = channel.plus_src_errors;
	src_process_calls = 0;
	partial_src_process_call = 2;
	native_tick_then_process(&channel);
	partial_src_process_call = 0;
	assert(channel.plus_src_errors == src_errors + 1U);
	for (size_t sample = 0; sample < URP_NATIVE_SAMPLES; ++sample)
		assert(channel.plus_link_native[sample] == 0);
	atomic_store_explicit(&channel.echoing, 0, memory_order_release);

	/* A complete 8 kHz echo block takes the normal successful branch. Force
	 * one short output to verify its deterministic zero-filled tail too. */
	urp_sample_queue_reset(&channel.echo_queue);
	for (size_t sample = 0; sample < URP_LINK_SAMPLES; ++sample)
		assert(urp_sample_queue_push_sample(&channel.echo_queue, program[sample]));
	src_errors = channel.plus_src_errors;
	src_process_calls = 0;
	atomic_store_explicit(&channel.echoing, 1, memory_order_release);
	native_tick_then_process(&channel);
	assert(src_process_calls >= 2);
	assert(channel.plus_src_errors == src_errors);

	urp_sample_queue_reset(&channel.echo_queue);
	for (size_t sample = 0; sample < URP_LINK_SAMPLES; ++sample)
		assert(urp_sample_queue_push_sample(&channel.echo_queue, program[sample]));
	src_process_calls = 0;
	partial_src_output_call = 2;
	native_tick_then_process(&channel);
	partial_src_output_call = 0;
	assert(src_process_calls >= 2);
	assert(channel.plus_src_errors == src_errors);
	assert(channel.plus_link_native[URP_NATIVE_SAMPLES - 1U] == 0);
	atomic_store_explicit(&channel.echoing, 0, memory_order_release);

	/* Converter failure details belong to the dynamically linked ring's own
	 * test suite. Here, use only its public API to verify the channel contract:
	 * queued program PCM is consumed without a false underflow, and exhaustion
	 * reports one callback-level underflow to the channel. */
	rpcr_destroy(&channel.plus_program_ring);
	assert(!rpcr_init(&channel.plus_program_ring, URP_PROGRAM_RING_SAMPLES, RPCR_SINC_BEST));
	assert(!rpcr_set_rates(&channel.plus_program_ring, URP_APP_RPT_RATE_DEFAULT,
			       URP_RATE_NATIVE));
	for (unsigned int frame = 0; frame < 6U; ++frame)
		usbradioplus_queue_program(&channel, program, URP_LINK_SAMPLES);
	{
		struct rpcr_observation before;
		struct rpcr_observation after;

		rpcr_observe(&channel.plus_program_ring, &before);
		underflows = channel.plus_link_queue_underflows;
		native_tick_then_process(&channel);
		rpcr_observe(&channel.plus_program_ring, &after);
		assert(channel.plus_link_queue_underflows == underflows);
		assert(after.available_samples < before.available_samples);
		for (unsigned int tick = 0;
		     tick < 8U && channel.plus_link_queue_underflows == underflows; ++tick)
			native_tick_then_process(&channel);
		assert(channel.plus_link_queue_underflows == underflows + 1U);
	}

	usbradioplus_dsp_destroy(&channel);
	assert(!urp_radio_destroy(channel.radio));
}

/** @brief Apply sample-rate DSP carrier gating without changing non-DSP receive sources. */
static void test_native_sample_gate(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio_config = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	static const char *const carrier_sources[] = {"dsp", "usb",	 "usbinvert", "vox",
						      "pp",  "ppinvert", "no"};
	static const enum radio_carrier_detect carrier_modes[] = {
		CD_XPMR_NOISE, CD_HID, CD_HID_INVERT, CD_XPMR_VOX, CD_PP, CD_PP_INVERT, CD_IGNORE};
	short *capture = (short *)(channel.usbradio_read_buf + AST_FRIENDLY_OFFSET);
	short *network = (short *)(channel.usbradio_read_buf_8k + AST_FRIENDLY_OFFSET);

	settings_defaults(&settings);
	strcpy(settings.profiles[0].name, "sample-gate");
	strcpy(settings.profiles[0].channel, "RadioPlus/sample-gate");
	settings.profiles[0].enabled = 0;
	memset(&settings.profiles[0].chains[TXAGC_LOCAL], 0,
	       sizeof(settings.profiles[0].chains[TXAGC_LOCAL]));
	channel.name = "sample-gate";
	channel.rxdemod = RX_AUDIO_SPEAKER;
	channel.rxkeyed = 1; /* Adapter metadata still describes the previous frame. */
	channel.duplex3 = 999;
	channel.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	channel.plus_app_rpt_rate = URP_RATE_NATIVE;
	channel.plus_app_rpt_samples = URP_NATIVE_SAMPLES;
	channel.plus_deemphasis_corner_hz = 300.0;
	channel.plus_preemphasis_corner_hz = 300.0;
	channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
	assert(channel.radio);
	assert(!usbradioplus_dsp_init(&channel));
	for (size_t i = 0; i < URP_NATIVE_SAMPLES; ++i) {
		capture[2 * i] = 1000;
		channel.radio->rxCarrierGate[i] = i < URP_NATIVE_SAMPLES / 2;
	}

	for (size_t source = 0; source < ARRAY_LEN(carrier_sources); ++source) {
		channel.rxcdtype = carrier_modes[source];
		/* The signaling engine owns the sample gate state. Native tick must
		 * consume that already-applied value rather than refresh configuration. */
		channel.radio->rxCdType = carrier_modes[source];
		assert(!usbradioplus_prepare_native_processing(&channel));
		native_tick_then_process_and_consume(&channel);
		for (size_t i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			double expected = source == 0 && i >= URP_NATIVE_SAMPLES / 2 ? 0.0 : 1000.0;
			assert(channel.plus_local_native[i] == expected);
			assert(network[i] == expected);
		}
		assert(channel.rxkeyed); /* No changes to carrier, CTCSS, or PTT qualification. */
		assert(!channel.radio->txPttIn && !channel.radio->txPttOut);
	}
	usbradioplus_dsp_destroy(&channel);
	assert(!urp_radio_destroy(channel.radio));
}

/** @brief Verify unlinked channel cleanup. */
static void test_unlinked_channel_cleanup(void)
{
	urp_radio_state configuration = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	struct chan_usbradio_pvt *channel;

	destroy_unlinked_channel(NULL);
	destroy_unlinked_channel(&usbradio_default);
	channel = calloc(1, sizeof(*channel));
	assert(channel);
	channel->name = ast_strdup("cleanup");
	mock_dsp_available = 1;
	channel->dsp = ast_dsp_new();
	channel->radio = urp_radio_create(&configuration, URP_LINK_SAMPLES);
	assert(channel->name && channel->dsp && channel->radio);
	destroy_unlinked_channel(channel);
}

/** @brief Verify store config failure and option edges. */
static void test_store_config_failure_and_option_edges(void)
{
	struct chan_usbradio_pvt saved_default = usbradio_default;
	struct chan_usbradio_pvt existing = {.radioactive = 1};
	struct chan_usbradio_pvt *created;
	char oversized_delay[32];

	memset(&usbradio_default, 0, sizeof(usbradio_default));
	usbradio_default.plus_deemphasis_corner_hz = 250.0;
	usbradio_default.plus_preemphasis_corner_hz = 250.0;
	usbradio_default.plus_app_rpt_rate = URP_RATE_LINK;
	usbradio_default.plus_app_rpt_samples = URP_LINK_SAMPLES;
	usbradio_default.rxsdtype = SD_XPMR;
	usbradio_default.txmixa = TX_OUT_VOICE;
	usbradio_default.txmixb = TX_OUT_LSD;
	usbradio_default.rxsquelchadj = 500;
	usbradio_default.rxctcssadj = 0.5;
	ast_copy_string(usbradio_default.rxctcssfreqs, "0", sizeof(usbradio_default.rxctcssfreqs));
	ast_copy_string(usbradio_default.txctcssfreqs, "0", sizeof(usbradio_default.txctcssfreqs));
	ast_copy_string(usbradio_default.txctcssdefault, "0",
			sizeof(usbradio_default.txctcssdefault));
	settings_defaults(&settings);

	ast_calloc_calls = 0;
	fail_ast_calloc_call = 1;
	assert(store_config("allocation-failure") == NULL);
	fail_ast_calloc_call = 0;
	ast_strdup_calls = 0;
	fail_ast_strdup_call = 1;
	assert(store_config("name-failure") == NULL);
	fail_ast_strdup_call = 0;

	settings_defaults(&settings);
	assert(store_config("general") == NULL);
	add_processing_override("general", "channel_enabled", "invalid");
	assert(store_config("usb") == NULL);
	settings_defaults(&settings);
	settings.profiles[0].override_count = 0;
	usbradio_default.duplex3 = DUPLEX3_LEVEL_MAX + 1;
	assert(store_config("usb") == NULL);
	usbradio_default.duplex3 = -1;
	assert(store_config("usb") == NULL);
	usbradio_default.duplex3 = 0;
	usbradio_default.plus_deemphasis_corner_hz = 0.0;
	assert(store_config("usb") == NULL);
	usbradio_default.plus_deemphasis_corner_hz = 250.0;
	usbradio_default.plus_preemphasis_corner_hz = 0.0;
	assert(store_config("usb") == NULL);
	usbradio_default.plus_preemphasis_corner_hz = 250.0;
	usbradio_default.plus_deemphasis_corner_hz = 500.1;
	assert(store_config("usb") == NULL);
	usbradio_default.plus_deemphasis_corner_hz = 250.0;
	usbradio_default.plus_preemphasis_corner_hz = 500.1;
	assert(store_config("usb") == NULL);
	usbradio_default.plus_deemphasis_corner_hz = 500.0;
	usbradio_default.plus_preemphasis_corner_hz = 500.0;

	ast_calloc_calls = 0;
	fail_ast_calloc_call = 2;
	assert(store_config("usb") == NULL);
	fail_ast_calloc_call = 0;
	fail_radio_state_allocation = 1;
	assert(store_config("usb") == NULL);
	fail_radio_state_allocation = 0;
	mock_dsp_available = 1;
	created = store_config("usb");
	assert(created);
	usbradio_default.next = created->next;
	destroy_unlinked_channel(created);

	/* Preserve the legacy defensive bounds even when a malformed raw override
	 * bypasses the normal processing-file range checker.  Store construction
	 * supplies the historic hysteresis default and clamps delay before the
	 * signaling engine receives either value. */
	settings_defaults(&settings);
	add_processing_override("receive", "noise_squelch_hysteresis", "0");
	snprintf(oversized_delay, sizeof(oversized_delay), "%d", RXSQDELAYBUFSIZE / 8);
	add_processing_override("receive", "squelch_delay_ms", oversized_delay);
	created = store_config("usb");
	assert(created);
	assert(created->rxsqhyst == 3000);
	assert(created->rxsquelchdelay == RXSQDELAYBUFSIZE / 8 - 1);
	usbradio_default.next = created->next;
	destroy_unlinked_channel(created);

	settings_defaults(&settings);
	settings.profiles[0].hardware.output_a_assignment = TX_OUT_COMPOSITE;
	settings.profiles[0].hardware.output_b_assignment = TX_OUT_VOICE;
	add_processing_override("hardware", "hardware_parallel_pin_2_assignment", "input");
	add_processing_override("hardware", "hardware_parallel_pin_3_assignment", "out1");
	add_processing_override("hardware", "hardware_parallel_pin_4_assignment", "ptt");
	add_processing_override("hardware", "hardware_parallel_pin_5_assignment", "out0");
	usbradio_default.rxsquelchdelay = RXSQDELAYBUFSIZE;
	usbradio_default.txpreemphasis = 1;
	usbradio_default.radioactive = 1;
	usbradio_default.next = &existing;
	created = store_config("usb");
	assert(created);
	/* Clean-slate [receive] defaults override an unrelated compatibility
	 * initializer, so an omitted squelch_delay_ms remains the shipped zero. */
	assert(created->rxsquelchdelay == 0);
	assert(created->radioactive && !existing.radioactive && hasout);
	usbradio_default.next = created->next;
	destroy_unlinked_channel(created);

	settings_defaults(&settings);
	settings.profiles[0].hardware.output_a_assignment = TX_OUT_VOICE;
	settings.profiles[0].hardware.output_b_assignment = TX_OUT_COMPOSITE;
	usbradio_default.rxsquelchdelay = 0;
	usbradio_default.txpreemphasis = 0;
	usbradio_default.rxsqhyst = 1;
	usbradio_default.radioactive = 0;
	created = store_config("usb");
	assert(created);
	usbradio_default.next = created->next;
	destroy_unlinked_channel(created);

	settings_defaults(&settings);
	settings.profiles[0].hardware.output_a_assignment = TX_OUT_LSD;
	settings.profiles[0].hardware.output_b_assignment = TX_OUT_LSD;
	ast_copy_string(usbradio_default.txctcssfreq, "100.0",
			sizeof(usbradio_default.txctcssfreq));
	created = store_config("usb");
	assert(created);
	usbradio_default.next = created->next;
	destroy_unlinked_channel(created);

	settings_defaults(&settings);
	settings.profiles[0].hardware.output_a_assignment = TX_OUT_VOICE;
	settings.profiles[0].hardware.output_b_assignment = TX_OUT_VOICE;
	created = store_config("usb");
	assert(created);
	usbradio_default.next = created->next;
	destroy_unlinked_channel(created);

	settings_defaults(&settings);
	settings.profiles[0].hardware.output_a_assignment = TX_OUT_OFF;
	settings.profiles[0].hardware.output_b_assignment = TX_OUT_COMPOSITE;
	created = store_config("usb");
	assert(created);
	usbradio_default.next = created->next;
	destroy_unlinked_channel(created);
	mock_dsp_available = 0;

	usbradio_default = saved_default;
	settings_defaults(&settings);
}

/** @brief Verify dsp init failures. */
static void test_dsp_init_failures(void)
{
	struct chan_usbradio_pvt channel = {.name = "test"};

	ast_calloc_calls = 0;
	fail_ast_calloc_call = 1;
	assert(usbradioplus_dsp_init(&channel) == -1);
	usbradioplus_dsp_destroy(&channel);
	ast_calloc_calls = 0;
	fail_ast_calloc_call = 2;
	assert(usbradioplus_dsp_init(&channel) == -1);
	usbradioplus_dsp_destroy(&channel);
	fail_ast_calloc_call = 0;
	rpcr_init_calls = 0;
	fail_rpcr_init_call = 1;
	assert(usbradioplus_dsp_init(&channel) == -1);
	usbradioplus_dsp_destroy(&channel);
	fail_rpcr_init_call = 0;
	fail_rpcr_set_rates = 1;
	assert(usbradioplus_dsp_init(&channel) == -1);
	usbradioplus_dsp_destroy(&channel);
	fail_rpcr_set_rates = 0;
}

/** @brief Verify direct-renderer setup failures release every partially built owner. */
static void test_native_renderer_start_failures(void)
{
	/* DSP graph setup allocates one owner first. The next allocations build the
	 * direct renderer, its persistent SRC owners, and parrot storage. Start each
	 * attempt with a fully valid channel so every failure exercises renderer
	 * cleanup rather than a preceding configuration rejection. */
	for (int failure = 2; failure <= 5; ++failure) {
		struct chan_usbradio_pvt channel = {0};
		urp_radio_state radio_config = {
			.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};

		settings_defaults(&settings);
		strcpy(settings.profiles[0].name, "renderer-start");
		strcpy(settings.profiles[0].channel, "RadioPlus/renderer-start");
		channel.name = "renderer-start";
		channel.plus_app_rpt_rate = URP_RATE_LINK;
		channel.plus_app_rpt_samples = URP_LINK_SAMPLES;
		channel.plus_deemphasis_corner_hz = 300.0;
		channel.plus_preemphasis_corner_hz = 300.0;
		channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
		assert(channel.radio);
		ast_calloc_calls = 0;
		fail_ast_calloc_call = failure;
		assert(usbradioplus_dsp_init(&channel) == -1);
		assert(!channel.plus_dsp_initialized);
		assert(!channel.plus_native_renderer);
		usbradioplus_dsp_destroy(&channel);
		assert(!urp_radio_destroy(channel.radio));
		fail_ast_calloc_call = 0;
	}

	/* The renderer owns its persistent program/echo converters. Each constructor
	 * failure must leave no started renderer or retained graph. */
	for (int failure = 1; failure <= 4; ++failure) {
		struct chan_usbradio_pvt channel = {0};
		urp_radio_state radio_config = {
			.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};

		settings_defaults(&settings);
		strcpy(settings.profiles[0].name, "renderer-src");
		strcpy(settings.profiles[0].channel, "RadioPlus/renderer-src");
		channel.name = "renderer-src";
		channel.plus_app_rpt_rate = URP_RATE_LINK;
		channel.plus_app_rpt_samples = URP_LINK_SAMPLES;
		channel.plus_deemphasis_corner_hz = 300.0;
		channel.plus_preemphasis_corner_hz = 300.0;
		channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
		assert(channel.radio);
		src_new_calls = 0;
		fail_src_new_call = failure;
		assert(usbradioplus_dsp_init(&channel) == -1);
		assert(src_new_calls >= failure);
		assert(!channel.plus_dsp_initialized);
		assert(!channel.plus_native_renderer);
		usbradioplus_dsp_destroy(&channel);
		assert(!urp_radio_destroy(channel.radio));
		fail_src_new_call = 0;
	}

	/* Renderer startup reserves both SRC workspaces before any audio callback can
	 * use them. A reservation failure must destroy the partial renderer cleanly. */
	{
		struct chan_usbradio_pvt channel = {0};
		urp_radio_state radio_config = {
			.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};

		settings_defaults(&settings);
		strcpy(settings.profiles[0].name, "renderer-reserve");
		strcpy(settings.profiles[0].channel, "RadioPlus/renderer-reserve");
		channel.name = "renderer-reserve";
		channel.plus_app_rpt_rate = URP_RATE_LINK;
		channel.plus_app_rpt_samples = URP_LINK_SAMPLES;
		channel.plus_deemphasis_corner_hz = 300.0;
		channel.plus_preemphasis_corner_hz = 300.0;
		channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
		assert(channel.radio);
		assert(!usbradioplus_dsp_init(&channel));
		usbradioplus_native_renderer_stop(&channel);
		/* Fail the first echo-up workspace allocation. */
		fail_realloc = 1;
		assert(usbradioplus_native_renderer_start(&channel) == -1);
		fail_realloc = 0;
		assert(!channel.plus_native_renderer);
		/* echo_up reserves its input and output first. Fail the next reserve,
		 * which is the downsampler input, to exercise partial-renderer cleanup. */
		realloc_calls = 0;
		fail_realloc_call = 3;
		assert(usbradioplus_native_renderer_start(&channel) == -1);
		assert(realloc_calls == 3);
		fail_realloc_call = 0;
		assert(!channel.plus_native_renderer);
		usbradioplus_dsp_destroy(&channel);
		assert(!urp_radio_destroy(channel.radio));
	}
}

/** Inject a backend device-reservation failure. */
static int advanced_request_failure;
/** Number of native interface selections. */
static unsigned int advanced_configured;

/** @brief Reserve a fixture device through the app_rpt-facing backend.
 * @param type Backend technology name.
 * @param cap Backend capabilities.
 * @param assignedids Optional channel identities.
 * @param requestor Optional requesting channel.
 * @param data Device name.
 * @param cause Receives a fixture failure cause.
 * @return Fixture channel or null.
 */
static struct ast_channel *advanced_backend_request(const char *type, struct ast_format_cap *cap,
						    const struct ast_assigned_ids *assignedids,
						    const struct ast_channel *requestor,
						    const char *data, int *cause)
{
	assert(!strcmp(type, "test-backend") && !cap && !assignedids && !requestor);
	assert(!strcmp(data, "test"));
	*cause = 42;
	return advanced_request_failure ? NULL : (struct ast_channel *)(uintptr_t)1;
}

/** @brief Record native-mode selection before startup.
 * @param channel Reserved fixture channel.
 */
static void advanced_configure(struct ast_channel *channel)
{
	assert(channel == (struct ast_channel *)(uintptr_t)1);
	advanced_configured++;
}

/** @brief Check call forwarding.
 * @param channel Reserved fixture channel.
 * @param destination Device name.
 * @param timeout Call timeout.
 * @return Distinct forwarded status.
 */
static int advanced_backend_call(struct ast_channel *channel, const char *destination, int timeout)
{
	assert(channel && !strcmp(destination, "test") && timeout == 7);
	return 23;
}

/** @brief Check hangup forwarding.
 * @param channel Reserved fixture channel.
 * @return Distinct forwarded status.
 */
static int advanced_backend_hangup(struct ast_channel *channel)
{
	assert(channel);
	return 24;
}

/** @brief Check read forwarding.
 * @param channel Reserved fixture channel.
 * @return Fixture frame.
 */
static struct ast_frame *advanced_backend_read(struct ast_channel *channel)
{
	assert(channel);
	return &ast_null_frame;
}

/** @brief Check write forwarding.
 * @param channel Reserved fixture channel.
 * @param frame Fixture frame.
 * @return Distinct forwarded status.
 */
static int advanced_backend_write(struct ast_channel *channel, struct ast_frame *frame)
{
	assert(channel && frame == &ast_null_frame);
	return 25;
}

/** @brief Check indication forwarding.
 * @param channel Reserved fixture channel.
 * @param condition PTT indication.
 * @param data Optional payload.
 * @param length Payload bytes.
 * @return Distinct forwarded status.
 */
static int advanced_backend_indicate(struct ast_channel *channel, int condition, const void *data,
				     size_t length)
{
	assert(channel && condition == AST_CONTROL_RADIO_KEY && !data && !length);
	return 26;
}

/** @brief Verify native queue pacing through the persistent clock-recovery converter. */
static void test_advanced_native_clock(void)
{
	struct chan_usbradio_pvt channel = {.name = "test",
					    .plus_hardware_applied = 1,
					    .plus_deemphasis_corner_hz = 300.0,
					    .plus_preemphasis_corner_hz = 300.0};
	urp_radio_state configuration = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	settings_defaults(&settings);
	ast_copy_string(settings.profiles[0].name, "test", sizeof(settings.profiles[0].name));
	ast_copy_string(settings.profiles[0].channel, "RadioPlus/test",
			sizeof(settings.profiles[0].channel));
	settings.profiles[0].enabled = 0;
	test_channel_private = &channel;
	usbradioplus_configure_advanced(NULL);
	channel.radio = urp_radio_create(&configuration, 160);
	assert(channel.radio && usbradioplus_dsp_init(&channel) == 0);
	usbradioplus_configure_advanced(NULL);
	assert(channel.radio->radioDuplex);
	channel.hasusb = 1;
	usbradioplus_configure_advanced(NULL);
	channel.hasusb = 0;
	/* The module reports a lower-layer rate-setup failure and safely continues;
	 * the next successful selection restores the shared ring configuration. */
	fail_rpcr_set_rates = 1;
	usbradioplus_interface_mode(&channel, 1);
	fail_rpcr_set_rates = 0;
	usbradioplus_interface_mode(&channel, 1);
	assert(channel.plus_app_rpt_rate == 48000 && channel.plus_app_rpt_samples == 960);
	assert(channel.plus_program_target_samples ==
	       (URP_RATE_NATIVE * URP_PROGRAM_RING_TARGET_MS + 999U) / 1000U);
	assert(channel.plus_program_target_samples < channel.plus_program_ring.capacity);
	short program[URP_NATIVE_SAMPLES];
	for (size_t i = 0; i < URP_NATIVE_SAMPLES; ++i)
		program[i] = 123;
	channel.rxkeyed = 1;
	atomic_store_explicit(&channel.txkeyed, 1, memory_order_release);
	channel.duplex3 = 999;
	channel.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	for (unsigned int frame = 0;
	     frame < channel.plus_program_target_samples / URP_NATIVE_SAMPLES + 1U; ++frame)
		usbradioplus_queue_program(&channel, program, URP_NATIVE_SAMPLES);
	assert(rpcr_available(&channel.plus_program_ring) >= channel.plus_program_target_samples);
	channel.plus_link_queue_underflows = 0;
	/* Native-rate playout consumes directly without startup priming. */
	for (unsigned int tick = 0; tick < 4U; ++tick)
		native_tick_then_process(&channel);
	assert(rpcr_available(&channel.plus_program_ring) <
	       channel.plus_program_target_samples + URP_NATIVE_SAMPLES);
	assert(channel.plus_link_queue_underflows == 0);
	/* Continuous playout drains source PCM until a genuine shortfall; it never
	 * holds a protected startup or recovery reserve. */
	for (unsigned int tick = 0; tick < 8U && !channel.plus_link_queue_underflows; ++tick)
		native_tick_then_process(&channel);
	assert(channel.plus_link_queue_underflows == 1);
	size_t program_capacity = channel.plus_program_ring.capacity;
	/* Direct playout renders immediately, conceals the missing tail, and records
	 * one true output shortfall. */
	rpcr_destroy(&channel.plus_program_ring);
	assert(rpcr_init(&channel.plus_program_ring, program_capacity, RPCR_SINC_BEST) == 0);
	assert(rpcr_set_rates(&channel.plus_program_ring, URP_RATE_NATIVE, URP_RATE_NATIVE) == 0);
	channel.plus_program_target_samples = 1;
	usbradioplus_queue_program(&channel, program, URP_LINK_SAMPLES);
	assert(!usbradioplus_prepare_native_processing(&channel));
	native_tick_then_process(&channel);
	assert(channel.plus_link_queue_underflows == 2);
	/* Remaining direct source PCM must not be classified as an underrun. */
	rpcr_destroy(&channel.plus_program_ring);
	assert(rpcr_init(&channel.plus_program_ring, program_capacity, RPCR_SINC_BEST) == 0);
	assert(rpcr_set_rates(&channel.plus_program_ring, URP_RATE_NATIVE, URP_RATE_NATIVE) == 0);
	channel.plus_program_target_samples = 1;
	usbradioplus_queue_program(&channel, program, URP_NATIVE_SAMPLES);
	usbradioplus_queue_program(&channel, program, URP_NATIVE_SAMPLES);
	assert(!usbradioplus_prepare_native_processing(&channel));
	native_tick_then_process(&channel);
	assert(channel.plus_link_queue_underflows == 2);
	atomic_store_explicit(&channel.txkeyed, 0, memory_order_release);
	native_tick_then_process(&channel);
	assert(channel.plus_link_queue_underflows == 2);
	usbradioplus_interface_mode(&channel, 0);
	assert(!channel.plus_advanced && channel.plus_app_rpt_rate == 8000 &&
	       channel.plus_app_rpt_samples == 160);
	usbradioplus_queue_program(&channel, program, 160);
	assert(rpcr_available(&channel.plus_program_ring) > 0);
	usbradioplus_dsp_destroy(&channel);
	urp_radio_destroy(channel.radio);
	test_channel_private = NULL;
}

/** @brief Verify adapter reservation, forwarding, and registration failure cleanup. */
static void test_advanced_adapter(void)
{
	const struct ast_channel_tech backend = {
		.type = "test-backend",
		.requester = advanced_backend_request,
		.call = advanced_backend_call,
		.hangup = advanced_backend_hangup,
		.read = advanced_backend_read,
		.write = advanced_backend_write,
		.indicate = advanced_backend_indicate,
	};
	usbradioplus_advanced_unregister();
	assert(usbradioplus_advanced_register(&backend, 44100, advanced_configure) == -1);
	fail_format_cap_alloc = 1;
	assert(usbradioplus_advanced_register(&backend, 48000, advanced_configure) == -1);
	fail_format_cap_alloc = 0;
	format_append_result = -1;
	assert(usbradioplus_advanced_register(&backend, 48000, advanced_configure) == -1);
	format_append_result = 0;
	channel_register_result = -1;
	assert(usbradioplus_advanced_register(&backend, 48000, advanced_configure) == -1);
	channel_register_result = 0;
	assert(usbradioplus_advanced_register(&backend, 48000, advanced_configure) == 0);
	int cause = 0;
	format_compatible = 0;
	assert(!advanced_technology->requester("RadioPlusAdvanced", NULL, NULL, NULL, "test",
					       &cause));
	format_compatible = 1;
	advanced_request_failure = 1;
	assert(!advanced_technology->requester("RadioPlusAdvanced", NULL, NULL, NULL, "test",
					       &cause));
	assert(cause == 42 && !advanced_configured);
	advanced_request_failure = 0;
	set_read_result = -1;
	int before = hangup_calls;
	assert(!advanced_technology->requester("RadioPlusAdvanced", NULL, NULL, NULL, "test",
					       &cause));
	set_read_result = 0;
	set_write_result = -1;
	assert(!advanced_technology->requester("RadioPlusAdvanced", NULL, NULL, NULL, "test",
					       &cause));
	set_write_result = 0;
	assert(hangup_calls == before + 2);
	struct ast_channel *channel = advanced_technology->requester("RadioPlusAdvanced", NULL,
								     NULL, NULL, "test", &cause);
	assert(channel && advanced_configured == 3);
	assert(advanced_technology->call(channel, "test", 7) == 23);
	assert(advanced_technology->read(channel) == &ast_null_frame);
	assert(advanced_technology->write(channel, &ast_null_frame) == 25);
	assert(advanced_technology->indicate(channel, AST_CONTROL_RADIO_KEY, NULL, 0) == 26);
	assert(advanced_technology->hangup(channel) == 24);
	usbradioplus_advanced_unregister();
	usbradioplus_advanced_unregister();
}

/** @brief Verify direct-renderer guard paths preserve callback invariants. */
static void test_native_renderer_guard_paths(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio_config = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	struct usbradioplus_native_renderer_stats statistics;
	struct audiostatistics tx_statistics;
	struct chan_usbradio_pvt no_renderer = {0};

	settings_defaults(&settings);
	strcpy(settings.profiles[0].name, "renderer-guards");
	strcpy(settings.profiles[0].channel, "RadioPlus/renderer-guards");
	settings.profiles[0].enabled = 0;
	channel.name = "renderer-guards";
	channel.plus_app_rpt_rate = URP_RATE_LINK;
	channel.plus_app_rpt_samples = URP_LINK_SAMPLES;
	channel.plus_deemphasis_corner_hz = 300.0;
	channel.plus_preemphasis_corner_hz = 300.0;
	channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
	assert(channel.radio);
	/* Public controls remain harmless before setup. A missing renderer emits
	 * silence and publishes only the signaling-owned PTT decision. */
	atomic_init(&no_renderer.plus_hardware_ptt_request, 0);
	usbradioplus_native_renderer_stats_reset(&no_renderer);
	usbradioplus_native_renderer_clear_parrot(&no_renderer);
	usbradioplus_native_renderer_clear_legacy_echo(&no_renderer);
	usbradioplus_native_tick(&no_renderer, 1);
	assert(usbradioplus_native_renderer_stats_read(&no_renderer, &statistics) == -1);
	assert(usbradioplus_native_renderer_tx_audio_stats_read(&no_renderer, &tx_statistics) ==
	       -1);
	assert(!usbradioplus_dsp_init(&channel));
	usbradioplus_interface_mode(&channel, 0);

	/* The direct callback uses only bounded snapshots: retry exhaustion returns
	 * failure rather than waiting for a diagnostics reader or control writer. */
	usbradioplus_native_renderer_test_hardware_snapshot_race(&channel);
	assert(!usbradioplus_native_renderer_stats_read(&channel, &statistics));
	assert(!usbradioplus_native_renderer_tx_audio_stats_read(&channel, &tx_statistics));
	usbradioplus_native_renderer_test_statistics_publish_busy(&channel);
	assert(usbradioplus_native_renderer_test_statistics_retry(&channel) == -1);
	assert(usbradioplus_native_renderer_test_tx_audio_statistics_retry(&channel) == -1);

	usbradioplus_dsp_destroy(&channel);
	assert(usbradioplus_native_renderer_stats_read(&channel, &statistics) == -1);
	assert(usbradioplus_native_renderer_tx_audio_stats_read(&channel, &tx_statistics) == -1);
	assert(!urp_radio_destroy(channel.radio));
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{

#define RUN_TEST(function)                                                                         \
	do {                                                                                       \
		puts(#function);                                                                   \
		function();                                                                        \
	} while (0)
#ifdef URP_TEST_MODERN
	RUN_TEST(test_modern_device_policy_helpers);
	RUN_TEST(test_modern_channel_callbacks);
	RUN_TEST(test_modern_hid_worker_baseline);
	RUN_TEST(test_modern_parallel_ptt_release);
	RUN_TEST(test_modern_hid_worker_retries);
	RUN_TEST(test_modern_audio_worker_baseline);
	RUN_TEST(test_modern_module_lifecycle_baseline);
#endif
	RUN_TEST(test_clean_slate_signaling_defaults);
	RUN_TEST(test_option_decoders);
	RUN_TEST(test_channel_callbacks);
	RUN_TEST(test_text_controls);
	RUN_TEST(test_console_keying);
	RUN_TEST(test_channel_selection_helpers);
	RUN_TEST(test_cli_handlers);
	RUN_TEST(test_tune_flash_sequences);
	RUN_TEST(test_radio_tune_dispatch);
	RUN_TEST(test_menu_adjustment_helpers);
	RUN_TEST(test_menu_support_dispatch);
	RUN_TEST(test_tuning_displays);
	RUN_TEST(test_receive_calibration_helpers);
	RUN_TEST(test_config_update_and_radio_programming);
	RUN_TEST(test_hardware_handoff_snapshots);
	RUN_TEST(test_processing_config_overrides);
	RUN_TEST(test_complete_processing_config_overrides);
	RUN_TEST(test_complete_processing_config_override_rejections);
	RUN_TEST(test_processing_override_parse_edges);
	RUN_TEST(test_shared_config_loading);
	RUN_TEST(test_effective_processing_settings);
	RUN_TEST(test_numeric_helpers);
	RUN_TEST(test_shared_hardware_layouts);
	RUN_TEST(test_shared_control_helpers);
	RUN_TEST(test_shared_receive_signaling_helpers);
	RUN_TEST(test_shared_eeprom_wait);
	RUN_TEST(test_continuous_soundcard_output);
	RUN_TEST(test_tx_playout_hold);
	RUN_TEST(test_oss_tune_write_paths);
#ifndef URP_TEST_MODERN
	RUN_TEST(test_oss_audio_helpers);
	RUN_TEST(test_oss_channel_write_and_call);
	RUN_TEST(test_oss_channel_hangup);
	RUN_TEST(test_oss_parallel_pulser);
	RUN_TEST(test_oss_hid_worker_device_retry);
	RUN_TEST(test_oss_hid_attach_failures);
	RUN_TEST(test_oss_hid_worker_attach);
	advanced_hid_mode = 1;
	RUN_TEST(test_oss_hid_worker_attach);
	advanced_hid_mode = 0;
	ast_set_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	module_debug_level = 10;
	RUN_TEST(test_oss_hid_worker_attach);
	ast_clear_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	module_debug_level = 0;
	RUN_TEST(test_oss_hid_worker_first_radio_construction);
	advanced_hid_mode = 1;
	RUN_TEST(test_oss_hid_worker_first_radio_construction);
	advanced_hid_mode = 2;
	RUN_TEST(test_oss_hid_worker_first_radio_construction);
	advanced_hid_mode = 0;
	RUN_TEST(test_oss_channel_creation_and_request);
	RUN_TEST(test_oss_channel_read_guards);
	RUN_TEST(test_oss_complete_read_frame);
	ast_set_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	module_debug_level = 10;
	RUN_TEST(test_oss_complete_read_frame);
	ast_clear_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	module_debug_level = 0;
	RUN_TEST(test_oss_module_lifecycle_guards);
#endif
	RUN_TEST(test_native_fifo_and_squelch_copy);
	RUN_TEST(test_parrot_transitions);
	RUN_TEST(test_program_ring_and_parrot_storage);
	RUN_TEST(test_dsp_init_failures);
	RUN_TEST(test_native_renderer_start_failures);
	RUN_TEST(test_program_ring_native_tick);
	RUN_TEST(test_native_renderer_transmit_admission);
	RUN_TEST(test_native_tick_voice_graph_ownership);
	RUN_TEST(test_native_renderer_guard_paths);
	RUN_TEST(test_native_graph_slot_deferred_reclaim);
	RUN_TEST(test_native_graph_transaction_paths);
	RUN_TEST(test_radio_access_reconfigure_exclusion);
	RUN_TEST(test_native_tick_processing_edges);
	RUN_TEST(test_native_sample_gate);
	RUN_TEST(test_unlinked_channel_cleanup);
	RUN_TEST(test_store_config_failure_and_option_edges);
	RUN_TEST(test_advanced_adapter);
	RUN_TEST(test_advanced_native_clock);
	RUN_TEST(test_signaling_override_rejections);
	RUN_TEST(test_selected_signaling_requirements);
	RUN_TEST(test_independent_signaling_directions);
	RUN_TEST(test_signaling_override_commit_and_tuning_save);
	RUN_TEST(test_signaling_reload_preflight_transaction);
	RUN_TEST(test_signaling_reload_commit_transaction);
	RUN_TEST(test_signaling_reload_second_pass_failures);
	RUN_TEST(test_signaling_parser_helper_edges);
	RUN_TEST(test_processing_signaling_resolution_failure_paths);
	RUN_TEST(test_processing_config_missing_option_paths);
	RUN_TEST(test_radio_config_parser_failure_paths);
#undef RUN_TEST
	puts("channel core tests passed");
	return 0;
}

/** @def URP_CHANNEL_UNIT_TEST
 * @brief URP CHANNEL UNIT TEST selection for this isolated test harness.
 */
/** @def AST_MODULE_SELF_SYM
 * @brief AST MODULE SELF SYM selection for this isolated test harness.
 */
/** @def AST_MODULE
 * @brief AST MODULE selection for this isolated test harness.
 */
/** @def EXERCISE_HANDLER
 * @brief EXERCISE HANDLER selection for this isolated test harness.
 */
/** @def SET_APPLIED
 * @brief SET APPLIED selection for this isolated test harness.
 */
/** @def RUN_TEST
 * @brief RUN TEST selection for this isolated test harness.
 */

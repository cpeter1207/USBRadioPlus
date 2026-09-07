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
#include <samplerate.h>
#ifdef URP_TEST_MODERN
#include <libusb-1.0/libusb.h>
#include <sys/soundcard.h>
#include <usb.h>
#endif

struct usb_device;
struct usb_dev_handle;

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
/** Controls injected src process call failure for this test. */
static int fail_src_process_call;
/** Controls injected src new call failure for this test. */
static int fail_src_new_call;
/** Recorded src new calls for assertions. */
static int src_new_calls;
/** Recorded src process calls for assertions. */
static int src_process_calls;
/** Harness partial src process call used to script and verify host behavior. */
static int partial_src_process_call;
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
		memset(info, 0, sizeof(*info));
		info->fragstotal = mock_oss_fragment_total;
		info->fragments = mock_oss_fragments;
		info->fragsize = 3840;
		return 0;
	}
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
	(void)descriptor;
	if (mock_oss_io) {
		errno = mock_read_errno;
		if (mock_read_result > 0) {
			size_t bytes = (size_t)mock_read_result;
			memset(buffer, 0, bytes < count ? bytes : count);
		}
		return mock_read_result;
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
	(void)variable;
	(void)value;
	(void)match;
	(void)object;
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
	(void)value;
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
	return fail_realloc ? NULL : realloc(pointer, size);
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
static PaError modern_read_result;
static PaError modern_write_result;
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
	stream->output_channels = 2;
	return modern_open_result;
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
	ast_radio_pa_stop(&radio.pa);

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
		modern_write_result = paNoError;
		assert(soundcard_writeframe(&radio, output) ==
		       AST_RADIO_PA_48K_STEREO_SAMPLES * (int)sizeof(short));
		assert(modern_last_write == silence_buf);
		radio_state.txPttIn = 1;
		modern_write_result = paOutputUnderflowed;
		assert(soundcard_writeframe(&radio, output) > 0);
		assert(modern_last_write == output);
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
	ftxcapraw = tmpfile();
	assert(ftxcapraw);
	radio.txcapraw = 0;
	assert(usbradio_write(channel, &frame) == 0);
	radio.txcapraw = 1;
	assert(usbradio_write(channel, &frame) == 0);
	fclose(ftxcapraw);
	ftxcapraw = NULL;
	radio.txcapraw = 0;
	assert(radio.plus_program_queue.count > 0);

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
	radio.plus_emphasis_corner_hz = 300.0;
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
	radio.radio->txPttOut = 1;
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
	constructed.plus_emphasis_corner_hz = 300.0;
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
	radio.plus_emphasis_corner_hz = 300.0;
	radio.echoq.q_forw = radio.echoq.q_back = &radio.echoq;
	assert(usbradioplus_dsp_init(&radio) == 0);
	modern_read_result = paNoError;
	modern_read_sample = 0;
	modern_write_result = paNoError;
	modern_write_available = AST_RADIO_PA_FRAMES_PER_BUFFER;
	run_modern_audio_iteration(&radio);
	assert(radio.stopaudiothread && !radio.audio_thread_ready && !radio.pa.active);

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
	radio.owner = (struct ast_channel *)(uintptr_t)1;
	radio.rxcdtype = CD_HID;
	strcpy(settings.profiles[0].hardware.cos_assignment, "usb");
	radio.rxsdtype = SD_HID;
	radio.rxhidsq = 1;
	radio.rxhidctcss = 1;
	radio.radioduplex = 1;
	radio.rxctcssdecode = 1;
	strcpy(radio.rxctcssfreq, "100.0");
	radio.duplex3 = 999;
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
	run_modern_audio_iteration(&radio);
	assert(radio.rxkeyed && radio.lastrx);

	radio.rxhidsq = 0;
	run_modern_audio_iteration(&radio);
	assert(!radio.rxkeyed && !radio.lastrx);

	/* A nonzero, current HID timestamp is healthy. */
	radio.lasthidtime = 1234;
	run_modern_audio_iteration(&radio);
	radio.lasthidtime = 0;

	radio.rxcdtype = CD_HID_INVERT;
	strcpy(settings.profiles[0].hardware.cos_assignment, "usbinvert");
	radio.rxhidsq = 0;
	radio.rxsdtype = SD_HID_INVERT;
	radio.rxhidctcss = 0;
	run_modern_audio_iteration(&radio);
	radio.radio->rxExtCarrierDetect = 1;
	run_modern_audio_iteration(&radio);
	radio.rxcdtype = CD_HID;
	strcpy(settings.profiles[0].hardware.cos_assignment, "usb");
	radio.rxhidsq = 1;
	radio.rxsdtype = SD_PP;
	radio.rxppctcss = 1;
	run_modern_audio_iteration(&radio);
	radio.rxsdtype = SD_PP_INVERT;
	radio.rxppctcss = 0;
	run_modern_audio_iteration(&radio);

	radio.txoffdelay = 1;
	radio.txkeyed = 1;
	run_modern_audio_iteration(&radio);
	radio.txkeyed = 0;
	radio.txoffcnt = MS_TO_FRAMES(TX_OFF_DELAY_MAX);
	run_modern_audio_iteration(&radio);
	radio.txoffdelay = 0;

	radio.echomode = 1;
	radio.echomax = 2;
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_HID;
	radio.rxhidsq = radio.rxhidctcss = 1;
	radio.rxkeyed = radio.lastrx = 0;
	run_modern_audio_iteration(&radio);
	/* A second keyed frame traverses an existing queued echo frame. */
	run_modern_audio_iteration(&radio);
	radio.rxhidsq = 0;
	radio.rxkeyed = 0;
	run_modern_audio_iteration(&radio);

	radio.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	radio.rxhidsq = radio.rxhidctcss = 1;
	radio.rxkeyed = radio.lastrx = 0;
	run_modern_audio_iteration(&radio);
	radio.rxhidsq = 0;
	run_modern_audio_iteration(&radio);
	radio.echomode = 0;
	run_modern_audio_iteration(&radio);

	radio.rxctcssoverride = 1;
	radio.rxhidsq = 1;
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
	radio.rxsdtype = SD_HID;
	radio.radio->b.ctcssRxEnable = 1;
	radio.radio->rxCtcss->decode = CTCSS_NULL;
	run_modern_audio_iteration(&radio);
	radio.radio->b.ctcssRxEnable = 0;
	radio.rxcdtype = CD_IGNORE;
	strcpy(settings.profiles[0].hardware.cos_assignment, "ignore");
	radio.rxsdtype = SD_HID;
	run_modern_audio_iteration(&radio);
	radio.rxsdtype = SD_IGNORE;
	run_modern_audio_iteration(&radio);
	radio.rxsdtype = SD_HID;
	run_modern_audio_iteration(&radio);
	radio.rxcdtype = CD_HID;
	strcpy(settings.profiles[0].hardware.cos_assignment, "usb");
	radio.rxsdtype = SD_HID;

	mock_audio_clipping = 1;
	radio.clipledgpio = 0;
	run_modern_audio_iteration(&radio);
	radio.clipledgpio = 1;
	radio.hid_gpio_pulsetimer[0] = 0;
	run_modern_audio_iteration(&radio);
	assert(radio.hid_gpio_pulsetimer[0] == CLIP_LED_HOLD_TIME_MS);
	mock_audio_clipping = 0;
	radio.clipledgpio = 0;

	radio.radio->b.txCtcssReady = 1;
	radio.sendvoter = 1;
	radio.count_rssi_update = 1;
	radio.rxhidctcss = 1;
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
	run_modern_audio_iteration(&radio);
	radio.rxhidsq = 1;
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
	radio.radio->txPttOut = 1;
	radio.radioduplex = 0;
	radio.rxkeyed = radio.lastrx = 0;
	run_modern_audio_iteration(&radio);
	radio.radio->txPttOut = 0;
	radio.radioduplex = 1;

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
	radio.duplex3 = 0;
	radio.rxkeyed = 0;
	radio.lastrx = 1;
	radio.rxhidsq = 0;
	run_modern_audio_iteration(&radio);
	radio.rxkeyed = 1;
	radio.lastrx = 0;
	radio.rxhidsq = radio.rxhidctcss = 1;
	run_modern_audio_iteration(&radio);
	radio.owner = (struct ast_channel *)(uintptr_t)1;
	channel_state = AST_STATE_DOWN;
	run_modern_audio_iteration(&radio);
	channel_state = AST_STATE_UP;
	radio.rxctcssdecode = 0;
	radio.rxkeyed = 0;
	radio.lastrx = 0;
	radio.rxhidsq = radio.rxhidctcss = 1;
	run_modern_audio_iteration(&radio);

	frxcapraw = tmpfile();
	frxcaptrace = tmpfile();
	ftxcaptrace = tmpfile();
	assert(frxcapraw && frxcaptrace && ftxcaptrace);
	radio.rxcapraw = radio.rxcap2 = radio.txcap2 = radio.radioactive = 1;
	run_modern_audio_iteration(&radio);
	radio.rxcap2 = 0;
	run_modern_audio_iteration(&radio);
	radio.rxcap2 = 1;
	radio.radioactive = 0;
	run_modern_audio_iteration(&radio);
	fclose(frxcapraw);
	fclose(frxcaptrace);
	fclose(ftxcaptrace);
	frxcapraw = frxcaptrace = ftxcaptrace = NULL;
	radio.rxcapraw = radio.rxcap2 = radio.txcap2 = radio.radioactive = 0;
	/* Each capture guard also tolerates an enabled flag with no open file. */
	radio.rxcapraw = radio.rxcap2 = radio.txcap2 = radio.radioactive = 1;
	run_modern_audio_iteration(&radio);
	radio.rxcapraw = radio.rxcap2 = radio.txcap2 = radio.radioactive = 0;

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
	radio->radio->txPttOut = 1;
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
	radio.plus_emphasis_corner_hz = 300.0;
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
	constructed.plus_emphasis_corner_hz = 300.0;
	fail_radio_state_allocation = 1;
	run_modern_hid_retry(&constructed, 1);
	fail_radio_state_allocation = 0;

	{
		struct ast_variable ignored_option = {.name = "unsupported", .value = "4999"};
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
	radio.stophid = 0;
	modern_stop_hid_target = &radio;
	assert(hidthread(&radio) == NULL);
	modern_stop_hid_target = NULL;
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
		frxcapraw = tmpfile();
		frxcaptrace = tmpfile();
		frxoutraw = tmpfile();
		ftxcapraw = tmpfile();
		ftxcaptrace = tmpfile();
		ftxoutraw = tmpfile();
		assert(frxcapraw && frxcaptrace && frxoutraw && ftxcapraw && ftxcaptrace &&
		       ftxoutraw);
		no_radio.pttkick[0] = no_radio.pttkick[1] = -1;
		no_radio.hidthread = no_radio.audiothread = AST_PTHREADT_NULL;
		radio->next = &no_radio;
	}
	assert(unload_module() == 0);
	assert(!frxcapraw && !frxcaptrace && !frxoutraw && !ftxcapraw && !ftxcaptrace &&
	       !ftxoutraw);
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
	store_txtoctype(&radio, "phase");
	assert(radio.txtoctype == TOC_PHASE);
	store_txtoctype(&radio, "notone");
	assert(radio.txtoctype == TOC_NOTONE);
	store_txtoctype(&radio, "invalid");
	assert(radio.txtoctype == TOC_NOTONE);
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
	template.pRxCodeSrc = "100.0";
	template.pTxCodeSrc = "100.0";
	template.pTxCodeDefault = "100.0";
	radio.radio = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(radio.radio);
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
	assert(handle_radioplus_native_stats(&entry, 0, &stats3) == CLI_SUCCESS);
	radio.plus_parrot_playing = 1;
	assert(handle_radioplus_native_stats(&entry, 0, &stats3) == CLI_SUCCESS);
	radio.plus_parrot_playing = 0;
	assert(handle_radioplus_native_stats(&entry, 0, &stats4) == CLI_SUCCESS);
	stats_args[3] = "invalid";
	assert(handle_radioplus_native_stats(&entry, 0, &stats4) == CLI_SHOWUSAGE);
	usbradio_active = "missing";
	assert(handle_radioplus_native_stats(&entry, 0, &stats3) == CLI_FAILURE);
#undef EXERCISE_HANDLER
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
	assert(call_radio_tune(&radio, 3, "rxtracecap", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 3, "rxtracecap", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 3, "txtracecap", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 3, "txtracecap", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 3, "rxcap", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 3, "rxcap", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 3, "txcap", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 3, "txcap", NULL) == RESULT_SUCCESS);
	frxoutraw = tmpfile();
	ftxoutraw = tmpfile();
	assert(frxoutraw && ftxoutraw);
	assert(call_radio_tune(&radio, 3, "nocap", NULL) == RESULT_SUCCESS);
	assert(call_radio_tune(&radio, 3, "nocap", NULL) == RESULT_SUCCESS);
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
	assert(ctcss_adjust == (200 * M_Q8) / AUDIO_ADJUSTMENT);
	radio.txmixa = TX_OUT_LSD;
	_menu_txtone(1, &radio, "201");
	assert(radio.txmixaset == 201);
	radio.txmixa = TX_OUT_OFF;
	radio.txmixb = TX_OUT_LSD;
	_menu_txtone(1, &radio, "202");
	assert(radio.txmixbset == 202);
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
	struct chan_usbradio_pvt no_radio = {0};
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

	settings.profiles[0].hardware.rx_ctcss_frequencies_configured = 1;
	settings.profiles[0].hardware.tx_ctcss_frequencies_configured = 1;
	strcpy(settings.profiles[0].hardware.rx_ctcss_frequencies, "67.0");
	strcpy(settings.profiles[0].hardware.tx_ctcss_frequencies, "88.5");
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

	parallel_write_calls = 0;
	haspp = 2;
	pp_val = 0;
	radio.rxfreq = 146520000;
	radio.txfreq = 146520000;
	radio.remoted = 0;
	usbradioplus_program_radio(&radio);
	assert(parallel_write_calls > 0);
	radio.radio->txPttOut = 1;
	usbradioplus_program_radio(&radio);
	usbradioplus_program_radio(&no_radio);
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
	settings.profiles[0].hardware.rx_ctcss_frequencies_configured = 1;
	settings.profiles[0].hardware.tx_ctcss_frequencies_configured = 1;
	strcpy(settings.profiles[0].hardware.rx_ctcss_frequencies, "67.0");
	strcpy(settings.profiles[0].hardware.tx_ctcss_frequencies, "71.9");
	refresh_processing_hardware(&radio);
	assert(!strcmp(radio.plus_applied_rxctcssfreqs, "67.0"));
	assert(!strcmp(radio.plus_applied_txctcssfreqs, "71.9"));
	radio.remoted = 1;
	strcpy(settings.profiles[0].hardware.rx_ctcss_frequencies, "74.4");
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
	settings.profiles[0].hardware.rx_ctcss_frequencies_configured = 1;
	settings.profiles[0].hardware.tx_ctcss_frequencies_configured = 1;
	strcpy(settings.profiles[0].hardware.rx_ctcss_frequencies, "103.5");
	strcpy(settings.profiles[0].hardware.tx_ctcss_frequencies, "123.0");
	strcpy(radio.plus_applied_rxctcssfreqs, "103.5");
	strcpy(radio.plus_applied_txctcssfreqs, "100.0");
	refresh_processing_hardware(&radio);
	settings.profiles[0].hardware.rx_ctcss_frequencies_configured = 0;
	settings.profiles[0].hardware.tx_ctcss_frequencies_configured = 0;
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

/** @brief Verify processing config overrides. */
static void test_processing_config_overrides(void)
{
	struct chan_usbradio_pvt radio = {0};
	size_t index;
	static const int boolean_hardware[] = {3, 6, 7, 16, 19, 24, 25, 26, 27, 28, 32, 33};

	settings_defaults(&settings);
	for (index = 0; index < ARRAY_LEN(hardware_override_options); ++index) {
		const char *value = "1";
		size_t boolean_index;
		if (index == 0)
			value = "usb-test";
		else if (index == 1)
			value = "serial-test";
		else if (index == 8)
			value = "flat";
		else if (index == 9)
			value = "dsp";
		else if (index == 21)
			value = "100.0";
		else if (index == 23)
			value = "phase";
		else if (index == 38)
			value = "user-key";
		else if (index >= 43 && index <= 50)
			value = "in";
		else if (index == 51)
			value = "/dev/parport0";
		else if (index >= 53 && index <= 64)
			value = "out";
		else if (index == 65)
			value = "300.0";
		for (boolean_index = 0; boolean_index < ARRAY_LEN(boolean_hardware);
		     ++boolean_index)
			if (index == (size_t)boolean_hardware[boolean_index])
				value = "yes";
		add_processing_override("hardware", hardware_override_options[index], value);
	}
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
	assert(fabs(radio.plus_emphasis_corner_hz - 300.0) < 0.001);
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
	add_processing_override("hardware", "hardware_rx_on_delay_frames", "999999");
	add_processing_override("hardware", "hardware_tx_off_delay_frames", "999999");
	add_processing_override("duplex", duplex_override_options[2], "hardware");
	assert(apply_processing_config_overrides(&radio, "usb") == 0);
	assert(radio.rxondelay == MS_TO_FRAMES(RX_ON_DELAY_MAX));
	assert(radio.txoffdelay == MS_TO_FRAMES(TX_OFF_DELAY_MAX));
	assert(radio.duplex3mode == DUPLEX3_MODE_HARDWARE);

	settings_defaults(&settings);
	for (index = 0; index < ARRAY_LEN(boolean_hardware); ++index)
		add_processing_override("hardware",
					hardware_override_options[boolean_hardware[index]], "no");
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
	add_processing_override("hardware", "hardware_rx_ctcss_level", "nan");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	settings_defaults(&settings);
	add_processing_override("duplex", duplex_override_options[2], "invalid");
	assert(apply_processing_config_overrides(&radio, "usb") == -1);
	settings_defaults(&settings);
	add_processing_override("hardware", "hardware_emphasis_corner_hz", "invalid");
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
}

/** @brief Verify processing override parse edges. */
static void test_processing_override_parse_edges(void)
{
	struct chan_usbradio_pvt radio = {0};
	size_t index;
	static const int integer_hardware[] = {2,  4,  5,  10, 11, 12, 13, 14, 15, 17, 20, 22,
					       29, 30, 31, 34, 35, 36, 37, 39, 40, 41, 42};
	static const int boolean_hardware[] = {3, 6, 7, 16, 19, 24, 25, 26, 27, 28, 32, 33};
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
	for (index = 0; index < 2; ++index) {
		static const char *const floating_names[] = {"hardware_rx_ctcss_level",
							     "hardware_emphasis_corner_hz"};
		static const char *const floating_values[] = {"bad", "1x", "nan"};
		for (size_t malformed = 0; malformed < ARRAY_LEN(floating_values); ++malformed) {
			settings_defaults(&settings);
			add_processing_override("hardware", floating_names[index],
						floating_values[malformed]);
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
	assert(effective_rxcdtype(&radio) == CD_XPMR_NOISE);

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

	settings.profiles[0].hardware.cos_assignment_configured = 1;
	strcpy(settings.profiles[0].hardware.cos_assignment, "usb");
	assert(effective_rxcdtype(&radio) == CD_HID);
	strcpy(settings.profiles[0].hardware.cos_assignment, "usbinvert");
	assert(effective_rxcdtype(&radio) == CD_HID_INVERT);
	strcpy(settings.profiles[0].hardware.cos_assignment, "dsp");
	assert(effective_rxcdtype(&radio) == CD_XPMR_NOISE);
	strcpy(settings.profiles[0].hardware.cos_assignment, "vox");
	assert(effective_rxcdtype(&radio) == CD_XPMR_VOX);
	strcpy(settings.profiles[0].hardware.cos_assignment, "pp");
	assert(effective_rxcdtype(&radio) == CD_PP);
	strcpy(settings.profiles[0].hardware.cos_assignment, "ppinvert");
	assert(effective_rxcdtype(&radio) == CD_PP_INVERT);
	strcpy(settings.profiles[0].hardware.cos_assignment, "no");
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
	int32_t ctcss_adjust = 0;

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
	radio.txmixa = TX_OUT_LSD;
	assert(set_txctcss_level(&radio) == 0);
	assert(radio.txmixaset == 200);
	radio.txmixa = TX_OUT_OFF;
	radio.txmixb = TX_OUT_LSD;
	assert(set_txctcss_level(&radio) == 0);
	assert(radio.txmixbset == 200);
	radio.txmixb = TX_OUT_OFF;
	radio_state.ptxCtcssAdjust = NULL;
	assert(set_txctcss_level(&radio) == 0);
	radio_state.ptxCtcssAdjust = &ctcss_adjust;
	assert(set_txctcss_level(&radio) == 0);
	assert(ctcss_adjust == (200 * M_Q8) / AUDIO_ADJUSTMENT);
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
#ifdef URP_TEST_MODERN
	radio.pa.active = 1;
	modern_write_result = paNoError;
#else
	radio.sounddev = 7;
	radio.total_blocks = 8;
	radio.queuesize = 4;
	mock_oss_io = 1;
	mock_ioctl_failure = ULONG_MAX;
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
	assert(mock_sound_write_calls == 7);
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
	test_channel_private = &radio;
	assert(usbradio_write(channel, &frame) == 0);
	radio.hasusb = 1;
	ftxcapraw = tmpfile();
	assert(ftxcapraw);
	radio.txcapraw = 1;
	assert(usbradio_write(channel, &frame) == 0);
	radio.txcapraw = 0;
	assert(usbradio_write(channel, &frame) == 0);
	fclose(ftxcapraw);
	ftxcapraw = NULL;
	radio.echoing = 1;
	assert(usbradio_write(channel, &frame) == 0);
	radio.plus_advanced = 1;
	assert(usbradio_write(channel, &frame) == 0);
	radio.plus_advanced = 0;
	radio.echoing = 0;
	assert(usbradio_write(channel, &frame) == 0);
	radio.txkeyed = 1;
	assert(usbradio_write(channel, &frame) == 0);
	assert(radio.plus_program_queue.count > 0);
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
	urp_radio_state radio_state = {0};
	pthread_t thread;

	radio.name = "test";
	radio.radio = &radio_state;
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
	radio.plus_emphasis_corner_hz = 300.0;
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
	radio.radio->txPttOut = 1;
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
	 * inverted PTT release and a failed kick-pipe read in the same interval. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	radio.eepromctl = 1;
	mock_eeprom_result = 0;
	mock_eeprom_valid_magic = 0;
	radio.invertptt = 1;
	radio.lasttx = 1;
	radio.radio->txPttOut = 0;
	mock_poll_revents = POLLIN;
	mock_read_result = -1;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!radio.eepromctl && radio.invertptt);

	/* A readable kick byte requests the unkeyed state without forcing HID recovery. */
	radio.stophid = 0;
	mock_hid_input_calls = 0;
	mock_read_result = 1;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!radio.lasttx);
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
	radio.radio->txPttOut = 0;
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
	radio.name = "first-radio";
	radio.plus_advanced = advanced_hid_mode == 1;
	radio.radioduplex = advanced_hid_mode == 2;
	strcpy(radio.devstr, "usb-test");
	radio.sounddev = -1;
	radio.pttkick[0] = radio.pttkick[1] = -1;
	radio.plus_app_rpt_rate = URP_RATE_LINK;
	radio.plus_app_rpt_samples = URP_LINK_SAMPLES;
	radio.plus_emphasis_corner_hz = 250.0;
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
	radio.stophid = 0;
	assert(pthread_create(&thread, NULL, hidthread, &radio) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(radio.radio && radio.hasusb && radio.radioactive);

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
	radio.echoq.q_forw = radio.echoq.q_back = &radio.echoq;
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
	unsigned int writes_before = mock_sound_write_calls;

	mock_read_result = (ssize_t)(sizeof(radio->usbradio_read_buf) - radio->readpos);
	frame = usbradio_read(channel);
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
	radio.plus_emphasis_corner_hz = 300.0;
	radio.readpos = AST_FRIENDLY_OFFSET;
	radio.echoq.q_forw = radio.echoq.q_back = &radio.echoq;
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
	mock_write_result = -2;
	mock_read_errno = 0;
	channel_state = AST_STATE_UP;
	radio.clipledgpio = 1;
	mock_audio_clipping = 1;
	frxcapraw = tmpfile();
	frxcaptrace = tmpfile();
	ftxcaptrace = tmpfile();
	assert(frxcapraw);
	radio.rxcapraw = 1;
	radio.rxcap2 = 1;
	radio.txcap2 = 1;
	frame = oss_read_complete(&radio, channel);
	fclose(frxcapraw);
	fclose(frxcaptrace);
	fclose(ftxcaptrace);
	frxcapraw = NULL;
	frxcaptrace = NULL;
	ftxcaptrace = NULL;
	assert(frame == &radio.read_f && frame->frametype == AST_FRAME_VOICE);
	assert(frame->samples == URP_LINK_SAMPLES);
	assert(radio.hid_gpio_pulsetimer[0] == CLIP_LED_HOLD_TIME_MS);
	radio.hid_gpio_pulsetimer[0] = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	strcpy(settings.profiles[0].hardware.cos_assignment, "ignore");
	radio.rxsdtype = SD_HID;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	strcpy(settings.profiles[0].hardware.cos_assignment, "usb");
	assert(radio.hid_gpio_pulsetimer[0] == 1);
	radio.clipledgpio = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	ftxcaptrace = tmpfile();
	assert(ftxcaptrace);
	radio.txcap2 = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	fclose(ftxcaptrace);
	ftxcaptrace = NULL;
	frxcaptrace = tmpfile();
	assert(frxcaptrace);
	radio.rxcap2 = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.rxcap2 = 1;
	radio.radioactive = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.radioactive = 1;
	fclose(frxcaptrace);
	frxcaptrace = NULL;
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
	strcpy(settings.profiles[0].hardware.cos_assignment, "usb");
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
	strcpy(settings.profiles[0].hardware.cos_assignment, "usbinvert");
	radio.rxhidsq = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.rxkeyed);
	radio.rxcdtype = CD_PP;
	strcpy(settings.profiles[0].hardware.cos_assignment, "pp");
	radio.rxppsq = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.rxcdtype = CD_PP_INVERT;
	strcpy(settings.profiles[0].hardware.cos_assignment, "ppinvert");
	radio.rxppsq = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.rxcdtype = CD_HID_INVERT;
	strcpy(settings.profiles[0].hardware.cos_assignment, "usbinvert");
	radio.rxhidsq = 1;
	radio.radio->rxExtCarrierDetect = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(!radio.radio->rxExtCarrierDetect);
	radio.rxcdtype = CD_HID;
	strcpy(settings.profiles[0].hardware.cos_assignment, "usb");
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
	assert(radio.txoffcnt == MS_TO_FRAMES(TX_OFF_DELAY_MAX));
	radio.txoffdelay = 0;
	ast_clear_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	file_debug_level = 0;
	option_debug = 10;

	/* Legacy 8 kHz echo mode clears, records, and plays its queue. */
	{
		struct usbecho *echo = calloc(1, sizeof(*echo));
		assert(echo);
		insque((struct qelem *)echo, radio.echoq.q_back);
		radio.echomode = 0;
		assert(oss_read_complete(&radio, channel) == &radio.read_f);
		assert(radio.echoq.q_forw == &radio.echoq);
		echo = calloc(1, sizeof(*echo));
		assert(echo);
		insque((struct qelem *)echo, radio.echoq.q_back);
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
	ast_calloc_calls = 0;
	fail_ast_calloc_call = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	fail_ast_calloc_call = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.echoq.q_forw != &radio.echoq);
	radio.echoing = 0;
	radio.rxhidsq = 1;
	radio.radioduplex = 1;
	radio.echomax = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	assert(radio.echoq.q_forw != &radio.echoq);
	radio.echoing = 1;
	radio.rxcdtype = CD_IGNORE;
	radio.rxsdtype = SD_IGNORE;
	strcpy(settings.profiles[0].hardware.cos_assignment, "ignore");
	radio.rxkeyed = 1;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	radio.echomode = 0;
	radio.rxhidsq = 0;
	assert(oss_read_complete(&radio, channel) == &radio.read_f);
	strcpy(settings.profiles[0].hardware.cos_assignment, "usb");

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
	assert(!radio.echoing && radio.echoq.q_forw == &radio.echoq);
	radio.duplex3mode = DUPLEX3_MODE_HARDWARE;
	radio.duplex3 = 999;
	radio.radioduplex = 0;
	radio.radio->txPttOut = 1;
	radio.rxcdtype = CD_HID;
	radio.rxsdtype = SD_IGNORE;
	strcpy(settings.profiles[0].hardware.cos_assignment, "usb");
	radio.rxkeyed = radio.lastrx = 0;
	radio.rxhidsq = 1;
	assert(oss_read_complete(&radio, channel)->frametype == AST_FRAME_VOICE);
	radio.rxhidsq = 0;
	assert(oss_read_complete(&radio, channel)->frametype == AST_FRAME_VOICE);
	dsp_result_type = -1;
	radio.usedtmf = radio.echomode = 0;
	radio.dsp = NULL;
	usbradioplus_interface_mode(&radio, 0);

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
	frxcapraw = tmpfile();
	frxcaptrace = tmpfile();
	frxoutraw = tmpfile();
	ftxcapraw = tmpfile();
	ftxcaptrace = tmpfile();
	ftxoutraw = tmpfile();
	assert(frxcapraw && frxcaptrace && frxoutraw && ftxcapraw && ftxcaptrace && ftxoutraw);
	mock_oss_io = 1;
	assert(unload_module() == -1);
	assert(!frxcapraw && !frxcaptrace && !frxoutraw && !ftxcapraw && !ftxcaptrace &&
	       !ftxoutraw);
	assert(active_owner.sounddev == -1);
	mock_oss_io = 0;
	active_owner.owner = NULL;
	active_owner.dsp = NULL;
	usbradio_default.next = NULL;
	test_config_category = NULL;
	test_config_variables = NULL;
	test_config_load_result = NULL;
	separate_processing_config_result = 0;
}
#endif

/** @brief Verify native fifo and squelch copy. */
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

/** @brief Verify parrot transitions. */
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

/** @brief Verify program queue and parrot storage. */
static void test_program_queue_and_parrot_storage(void)
{
	struct chan_usbradio_pvt radio = {0};
	struct chan_usbradio_pvt seeded = {0};
	short samples[200];
	size_t i;

	radio.plus_app_rpt_samples = 160;
	radio.plus_app_rpt_rate = URP_RATE_LINK;
	for (i = 0; i < ARRAY_LEN(samples); ++i)
		samples[i] = (short)i;
	usbradioplus_queue_program(&radio, samples, ARRAY_LEN(samples));
	assert(radio.plus_program_queue.count ==
	       (PLUS_LINK_NATIVE_TARGET_SAMPLES + URP_NATIVE_SAMPLES - 1U) / URP_NATIVE_SAMPLES +
		       1U);
	assert(radio.plus_program_queue.high_water == radio.plus_program_queue.count);
	assert(radio.plus_program_queue.frames[radio.plus_program_queue.head][0] == 0);
	assert(radio.plus_program_queue
		       .frames[(radio.plus_program_queue.tail + URP_PROGRAM_QUEUE_FRAMES - 1) %
			       URP_PROGRAM_QUEUE_FRAMES][159] == 159);
	seeded.plus_app_rpt_samples = 160;
	seeded.plus_native_fifo.count = 1;
	usbradioplus_queue_program(&seeded, samples, 1);
	seeded.plus_native_fifo.count = 0;
	seeded.plus_program_queue.count = 1;
	seeded.plus_program_queue.head = 0;
	seeded.plus_program_queue.tail = 1;
	usbradioplus_queue_program(&seeded, samples, 1);
	radio.plus_native_fifo.count = 1;
	radio.plus_program_queue.count = 0;

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
	assert(usbradioplus_ensure_parrot_capacity(&radio) == 0);
	free(radio.plus_parrot);
	radio.plus_parrot = NULL;
	radio.plus_parrot_capacity = 0;
	fail_realloc = 1;
	assert(usbradioplus_ensure_parrot_capacity(&radio) == -1);
	fail_realloc = 0;
}

/** @brief Verify native tick baseline. */
static void test_native_tick_baseline(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio_config = {0};
	short native_program[URP_NATIVE_SAMPLES] = {0};
	short *capture = (short *)(channel.usbradio_read_buf + AST_FRIENDLY_OFFSET);
	size_t i;

	settings_defaults(&settings);
	ast_copy_string(settings.profiles[0].name, "test", sizeof(settings.profiles[0].name));
	ast_copy_string(settings.profiles[0].channel, "RadioPlus/test",
			sizeof(settings.profiles[0].channel));
	settings.profiles[0].enabled = 0;
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
	/* This rendering harness supplies an already open detector for every sample. */
	memset(channel.radio->rxCarrierGate, 1, URP_NATIVE_SAMPLES);
	assert(usbradioplus_dsp_init(&channel) == 0);
	for (i = 0; i < URP_NATIVE_SAMPLES; i++) {
		capture[i * 2] = (short)(1000.0 * sin(2.0 * M_PI * i / 48.0));
		capture[i * 2 + 1] = 0;
	}
	option_debug = 5;
	usbradioplus_native_tick(&channel);
	assert(strstr(tx_trace_message, "event=render frame=0"));
	option_debug = 0;
	assert(channel.plus_native_frames == 1);
	assert(channel.plus_adc_peak_dbfs > -40.0);
	assert(channel.plus_app_rpt_samples == 160);

	ast_set_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);
	module_debug_level = 5;
	usbradioplus_native_tick(&channel);
	assert(strstr(tx_trace_message, "event=render frame=1"));
	assert(channel.plus_native_frames == 2);
	module_debug_level = 0;
	file_debug_level = 5;
	usbradioplus_native_tick(&channel);
	assert(strstr(tx_trace_message, "event=render frame=2"));
	file_debug_level = 0;
	assert(channel.plus_native_frames == 3);

	channel.plus_app_rpt_rate = URP_RATE_NATIVE;
	channel.plus_app_rpt_samples = URP_NATIVE_SAMPLES;
	for (i = 0; i < ARRAY_LEN(native_program); ++i)
		native_program[i] = (short)i;
	for (i = 0; i < 3; ++i)
		usbradioplus_queue_program(&channel, native_program, ARRAY_LEN(native_program));
	usbradioplus_native_tick(&channel);
	assert(channel.plus_native_fifo.primed);
	assert(channel.plus_native_fifo.count ==
	       ((URP_FIFO_TARGET_NORMAL + URP_NATIVE_SAMPLES - 1U) / URP_NATIVE_SAMPLES - 1U) *
		       URP_NATIVE_SAMPLES);
	assert(channel.plus_native_frames == 4);
	assert(strstr(tx_trace_message, "event=render frame=2"));
	ast_clear_flag64(&ast_options, AST_OPT_FLAG_DEBUG_MODULE);

	urp_native_fifo_reset(&channel.plus_native_fifo);
	memset(&channel.plus_program_queue, 0, sizeof(channel.plus_program_queue));
	channel.plus_native_fifo.primed = 1;
	channel.txkeyed = 1;
	usbradioplus_native_tick(&channel);
	assert(!channel.plus_native_fifo.primed);
	assert(channel.plus_link_queue_underflows == 1);
	assert(!urp_native_fifo_push(&channel.plus_native_fifo, native_program,
				     ARRAY_LEN(native_program)));
	channel.plus_native_fifo.primed = 1;
	usbradioplus_native_tick(&channel);
	assert(channel.plus_native_fifo.stable_blocks == 1);

	settings.profiles[0].enabled = 1;
	settings.profiles[0].chains[TXAGC_LOCAL].enabled = 1;
	settings.profiles[0].chains[TXAGC_LOCAL].rnnoise_enabled = 1;
	settings.profiles[0].chains[TXAGC_LOCAL].ctcss_filter_configured = 1;
	settings.profiles[0].chains[TXAGC_LOCAL].agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	settings.profiles[0].chains[TXAGC_LOCAL].agc.ctcss_notch_width_hz = 5.0;
	ast_copy_string(channel.rxctcssfreq, "100.0", sizeof(channel.rxctcssfreq));
	channel.txpreemphasis = 1;
	channel.txkeyed = 0;
	channel.rxkeyed = 1;
	channel.duplex3 = 999;
	channel.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	channel.usedtmf = 0;
	usbradioplus_native_tick(&channel);
	settings.profiles[0].chains[TXAGC_LOCAL].rnnoise_enabled = 0;
	for (i = 0; i < 4; ++i)
		usbradioplus_native_tick(&channel);
	assert(isfinite(channel.plus_local_tx_max_peak_dbfs));
	assert(isfinite(channel.plus_preemphasis_input_max_peak_dbfs));
	/* Pre-emphasis never enables, disables, or changes the explicit final limiter. */
	for (int emphasis = 0; emphasis < 2; ++emphasis) {
		for (int limiter = 0; limiter < 2; ++limiter) {
			channel.txpreemphasis = emphasis;
			settings.profiles[0]
				.chains[TXAGC_VOICE_TELEMETRY]
				.agc.lookahead_limiter_enabled = limiter;
			usbradioplus_native_tick(&channel);
			assert(channel.plus_final_avfilter.config.preemphasis_enabled == emphasis);
			assert(channel.plus_final_avfilter.config.lookahead_limiter_enabled ==
			       limiter);
		}
	}
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.lookahead_limiter_enabled = 0;
	channel.txpreemphasis = 1;

	channel.plus_app_rpt_rate = URP_RATE_LINK;
	channel.plus_app_rpt_samples = URP_LINK_SAMPLES;
	urp_native_fifo_reset(&channel.plus_native_fifo);
	memset(&channel.plus_program_queue, 0, sizeof(channel.plus_program_queue));
	for (i = 0; i < 4; ++i)
		usbradioplus_queue_program(&channel, native_program, URP_LINK_SAMPLES);
	usbradioplus_native_tick(&channel);
	assert(channel.plus_native_fifo.primed);

	channel.echomode = 1;
	channel.usedtmf = 1;
	channel.toneflag = 1;
	channel.dsp = (struct ast_dsp *)&channel;
	assert(usbradioplus_ensure_parrot_capacity(&channel) == 0);
	usbradioplus_native_tick(&channel);
	assert(channel.plus_parrot_count == URP_NATIVE_SAMPLES);
	channel.rxkeyed = 0;
	usbradioplus_parrot_rx_transition(&channel, 1);
	assert(channel.plus_parrot_playing);
	usbradioplus_native_tick(&channel);
	assert(channel.plus_parrot_playback_frames == 1);
	assert(!channel.plus_parrot_playing && !channel.echoing);

	channel.plus_test_tone_enabled = 1;
	usbradioplus_native_tick(&channel);
	assert(channel.plus_test_tone_phase > 0.0);
	channel.plus_test_tone_enabled = 0;
	usbradioplus_native_tick(&channel);
	assert(channel.plus_test_tone_phase == 0.0);

	/* Each native processing stage must recover safely when an external DSP
	 * dependency rejects one frame.  The wrappers affect this test translation
	 * unit only and invoke the real libraries on every nonselected call. */
	channel.rxkeyed = 1;
	for (int failed_call = 1; failed_call <= 7; failed_call += 2) {
		av_frame_alloc_calls = 0;
		fail_av_frame_alloc_call = failed_call;
		usbradioplus_native_tick(&channel);
		assert(av_frame_alloc_calls >= failed_call);
	}
	fail_av_frame_alloc_call = 0;

	settings.profiles[0].chains[TXAGC_LOCAL].rnnoise_enabled = 1;
	src_process_calls = 0;
	fail_src_process_call = 1;
	usbradioplus_native_tick(&channel);
	assert(src_process_calls >= 1);
	settings.profiles[0].chains[TXAGC_LOCAL].rnnoise_enabled = 0;
	fail_src_process_call = 0;

	channel.plus_app_rpt_rate = URP_RATE_LINK;
	channel.plus_app_rpt_samples = URP_LINK_SAMPLES;
	src_process_calls = 0;
	fail_src_process_call = 1;
	unsigned int errors = channel.plus_src_errors;
	usbradioplus_native_tick(&channel);
	assert(channel.plus_src_errors == errors + 1);

	urp_native_fifo_reset(&channel.plus_native_fifo);
	memset(&channel.plus_program_queue, 0, sizeof(channel.plus_program_queue));
	for (i = 0; i < 4; ++i)
		usbradioplus_queue_program(&channel, native_program, URP_LINK_SAMPLES);
	src_process_calls = 0;
	fail_src_process_call = 2;
	errors = channel.plus_src_errors;
	usbradioplus_native_tick(&channel);
	assert(channel.plus_src_errors == errors + 1);
	fail_src_process_call = 0;

	urp_native_fifo_reset(&channel.plus_native_fifo);
	memset(&channel.plus_program_queue, 0, sizeof(channel.plus_program_queue));
	for (i = 0; i < 4; ++i)
		usbradioplus_queue_program(&channel, native_program, URP_LINK_SAMPLES);
	src_process_calls = 0;
	partial_src_process_call = 2;
	errors = channel.plus_src_errors;
	usbradioplus_native_tick(&channel);
	assert(channel.plus_src_errors == errors + 1);
	partial_src_process_call = 0;

	/* Exercise the remaining independent policy choices without relying on
	 * attached hardware. */
	channel.rxsquelchdelay = 1;
	settings.profiles[0].chains[TXAGC_LOCAL].input_gain_configured = 0;
	settings.profiles[0].chains[TXAGC_LOCAL].ctcss_filter_configured = 1;
	settings.profiles[0].chains[TXAGC_LOCAL].agc.ctcss_filter_mode =
		TXAGC_CTCSS_FILTER_HIGHPASS;
	channel.radio->txCtcssEnabled = 1;
	channel.radio->b.txCtcssOff = 1;
	channel.rxkeyed = 1;
	channel.duplex3 = 0;
	channel.duplex3mode = DUPLEX3_MODE_HARDWARE;
	usbradioplus_native_tick(&channel);

	channel.radio->b.txCtcssOff = 0;
	channel.rxkeyed = 0;
	channel.duplex3 = 999;
	usbradioplus_native_tick(&channel);
	channel.rxkeyed = 1;
	usbradioplus_native_tick(&channel);
	channel.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	channel.usedtmf = 1;
	channel.dsp = NULL;
	usbradioplus_native_tick(&channel);
	channel.dsp = (struct ast_dsp *)&channel;
	channel.toneflag = 0;
	usbradioplus_native_tick(&channel);

	channel.plus_native_fifo.primed = 1;
	channel.plus_native_fifo.count = 1;
	channel.txkeyed = 0;
	memset(&channel.plus_program_queue, 0, sizeof(channel.plus_program_queue));
	errors = channel.plus_link_queue_underflows;
	usbradioplus_native_tick(&channel);
	assert(channel.plus_link_queue_underflows == errors);

	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].splatter_filter_configured = 1;
	channel.txpreemphasis = 0;
	usbradioplus_native_tick(&channel);

	settings.profiles[0].chains[TXAGC_LOCAL].input_gain_configured = 1;
	settings.profiles[0].chains[TXAGC_LOCAL].ctcss_filter_configured = 0;
	usbradioplus_native_tick(&channel);

	channel.plus_parrot_count = 2 * URP_NATIVE_SAMPLES;
	channel.plus_parrot_play = 0;
	channel.plus_parrot_playing = 1;
	usbradioplus_native_tick(&channel);
	assert(channel.plus_parrot_playing);
	channel.plus_parrot_playing = 0;
	channel.plus_parrot_state.playing = 0;
	channel.rxkeyed = 1;
	channel.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	channel.echomode = 1;
	double *saved_parrot = channel.plus_parrot;
	channel.plus_parrot = NULL;
	usbradioplus_native_tick(&channel);
	channel.plus_parrot = saved_parrot;
	channel.echomode = 0;
	usbradioplus_native_tick(&channel);

	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].splatter_filter_configured = 0;
	channel.txhpf = 1;
	usbradioplus_native_tick(&channel);
	channel.txlpf = 1;
	usbradioplus_native_tick(&channel);
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.output_highpass_hz = 0.0;
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.output_lowpass_hz = 1000.0;
	usbradioplus_native_tick(&channel);
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.output_highpass_hz = 500.0;
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.output_lowpass_hz = 0.0;
	usbradioplus_native_tick(&channel);
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.output_highpass_hz = 5000.0;
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.output_lowpass_hz = 1000.0;
	usbradioplus_native_tick(&channel);
	usbradioplus_dsp_destroy(&channel);
	urp_radio_destroy(channel.radio);
}
/** @brief Preserve a single native-rate program frame through startup and underrun recovery.
 * @param target_samples Initial adaptive FIFO target in native samples.
 */
static void check_native_fifo_short_burst(unsigned int target_samples)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio_config = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	short program[URP_NATIVE_SAMPLES];

	settings_defaults(&settings);
	strcpy(settings.profiles[0].name, "fifo-burst");
	strcpy(settings.profiles[0].channel, "RadioPlus/fifo-burst");
	settings.profiles[0].enabled = 0;
	channel.name = "fifo-burst";
	channel.plus_app_rpt_rate = URP_RATE_NATIVE;
	channel.plus_app_rpt_samples = URP_NATIVE_SAMPLES;
	channel.plus_emphasis_corner_hz = 300.0;
	channel.plus_native_fifo.target_samples = target_samples;
	channel.txkeyed = 1;
	channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
	assert(channel.radio);
	assert(!usbradioplus_dsp_init(&channel));
	for (size_t i = 0; i < ARRAY_LEN(program); ++i)
		program[i] = 1000;

	for (unsigned int burst = 0; burst < 2; ++burst) {
		unsigned int frames =
			(target_samples + URP_NATIVE_SAMPLES - 1U) / URP_NATIVE_SAMPLES;
		usbradioplus_queue_program(&channel, program, ARRAY_LEN(program));
		assert(channel.plus_program_queue.count == frames);
		assert(!channel.plus_link_queue_overflows);
		for (unsigned int frame = 0; frame < frames; ++frame) {
			/* Recovery blends its preceding concealment into the first millisecond. */
			size_t first_sample = burst && !frame ? URP_NATIVE_SAMPLES / 20U : 0;
			src_process_calls = 0;
			usbradioplus_native_tick(&channel);
			assert(!src_process_calls && !channel.plus_link_src_pending);
			assert(channel.plus_native_fifo.primed);
			assert(channel.plus_native_fifo.target_samples == target_samples);
			assert(channel.plus_native_fifo.target_samples >=
			       110U * (URP_RATE_NATIVE / 1000U));
			assert(channel.plus_native_fifo.count ==
			       (frames - frame - 1U) * URP_NATIVE_SAMPLES);
			for (size_t i = first_sample; i < ARRAY_LEN(program); ++i)
				assert(channel.plus_link_native[i] ==
				       (frame + 1U == frames ? program[i] : 0));
		}
		assert(!channel.plus_program_queue.count);
		assert(channel.plus_link_queue_underflows == burst);
		/* A keyed empty tick raises the target and resets startup buffering. */
		usbradioplus_native_tick(&channel);
		assert(!channel.plus_native_fifo.primed && !channel.plus_native_fifo.count);
		assert(channel.plus_link_queue_underflows == burst + 1U);
		target_samples = target_samples < URP_FIFO_TARGET_MAX
					 ? target_samples + URP_FIFO_TARGET_STEP
					 : URP_FIFO_TARGET_MAX;
		assert(channel.plus_native_fifo.target_samples == target_samples);
		assert(!channel.plus_link_queue_overflows);
	}
	usbradioplus_dsp_destroy(&channel);
	assert(!urp_radio_destroy(channel.radio));
}

/** @brief Check one-frame bursts at every boundary of the adaptive latency policy. */
static void test_native_fifo_short_bursts(void)
{
	check_native_fifo_short_burst(URP_FIFO_TARGET_MIN);
	check_native_fifo_short_burst(URP_FIFO_TARGET_NORMAL);
	check_native_fifo_short_burst(URP_FIFO_TARGET_MAX);
}

/** @brief Initialize a channel with the production best-quality sinc converters.
 * @param channel Zero-initialized channel to prepare.
 * @param target_samples Initial adaptive FIFO target in native samples.
 * @param input_rate Link-side sample rate in samples per second.
 * @param keyed Nonzero keeps the transmitter keyed while the program drains.
 */
static void initialize_src_burst_channel(struct chan_usbradio_pvt *channel,
					 unsigned int target_samples, unsigned int input_rate,
					 int keyed)
{
	urp_radio_state radio_config = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};

	settings_defaults(&settings);
	strcpy(settings.profiles[0].name, "src-burst");
	strcpy(settings.profiles[0].channel, "RadioPlus/src-burst");
	settings.profiles[0].enabled = 0;
	channel->name = "src-burst";
	channel->plus_app_rpt_rate = input_rate;
	channel->plus_app_rpt_samples = input_rate / 50U;
	channel->plus_emphasis_corner_hz = 300.0;
	channel->plus_native_fifo.target_samples = target_samples;
	channel->txkeyed = keyed;
	channel->radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
	assert(channel->radio);
	/* The channel initializer selects the production SRC_SINC_BEST_QUALITY converter. */
	assert(!usbradioplus_dsp_init(channel));
}

/** @brief Keep local dynamics and RNNoise continuous unless RX CPU saving permits a pause. */
static void test_local_processing_cpu_saver(void)
{
	const struct {
		int saver;
		int keyed;
		int runs;
	} cases[] = {{0, 0, 1}, {0, 1, 1}, {1, 0, 0}, {1, 1, 1}};

	for (unsigned int enabled = 0; enabled < 2; ++enabled) {
		for (unsigned int rnnoise = 0; rnnoise < 2; ++rnnoise) {
			for (size_t scenario = 0; scenario < ARRAY_LEN(cases); ++scenario) {
				struct chan_usbradio_pvt channel = {0};
				short *capture =
					(short *)(channel.usbradio_read_buf + AST_FRIENDLY_OFFSET);
				unsigned long long dynamics_before, filter_before, after;
				uint64_t rnnoise_before, startup_before;
				int runs = enabled && cases[scenario].runs;

				initialize_src_burst_channel(&channel, URP_FIFO_TARGET_NORMAL,
							     URP_RATE_NATIVE, 0);
				settings.profiles[0].enabled = 1;
				settings.profiles[0].chains[TXAGC_LOCAL].enabled = enabled;
				settings.profiles[0].chains[TXAGC_LOCAL].rnnoise_enabled = rnnoise;
				strcpy(settings.profiles[0].hardware.cos_assignment, "usb");
				channel.rxcpusaver = cases[scenario].saver;
				channel.rxkeyed = 1;
				for (size_t i = 0; i < URP_NATIVE_SAMPLES; ++i)
					capture[2 * i] =
						(short)(3000.0 * sin(2.0 * M_PI * i / 48.0));
				for (unsigned int tick = 0; tick < 4; ++tick)
					usbradioplus_native_tick(&channel);
				dynamics_before = channel.plus_local_avfilter.input_samples;
				filter_before = channel.plus_rx_filter.input_samples;
				rnnoise_before = channel.plus_local_rnnoise.rnnoise_frames;
				startup_before = channel.plus_local_rnnoise.startup_samples;
				assert(dynamics_before == (enabled ? 4U * URP_NATIVE_SAMPLES : 0));
				channel.rxkeyed = cases[scenario].keyed;
				for (unsigned int tick = 0; tick < 3; ++tick)
					usbradioplus_native_tick(&channel);
				after = channel.plus_local_avfilter.input_samples;
				assert(after ==
				       dynamics_before + (runs ? 3U * URP_NATIVE_SAMPLES : 0));
				assert(channel.plus_rx_filter.input_samples ==
				       filter_before + 3U * URP_NATIVE_SAMPLES);
				assert(!channel.plus_local_avfilter.failed);
				assert(!channel.plus_local_rnnoise.errors);
				if (runs && rnnoise) {
					assert(channel.plus_local_rnnoise.active);
					assert(channel.plus_local_rnnoise.rnnoise_frames >
					       rnnoise_before);
					assert(channel.plus_local_rnnoise.startup_samples ==
					       startup_before);
				} else {
					assert(!channel.plus_local_rnnoise.active);
					assert(channel.plus_local_rnnoise.rnnoise_frames ==
					       rnnoise_before);
				}
				/* Reopening receive resumes the optional stages when enabled. */
				channel.rxkeyed = 1;
				usbradioplus_native_tick(&channel);
				assert(channel.plus_local_avfilter.input_samples ==
				       after + (enabled ? URP_NATIVE_SAMPLES : 0));
				assert(channel.plus_local_rnnoise.active ==
				       (int)(enabled && rnnoise));
				usbradioplus_dsp_destroy(&channel);
				assert(!urp_radio_destroy(channel.radio));
			}
		}
	}
}

/** @brief Render one link-rate tone frame through real SRC startup and recovery.
 * @param target_samples Initial adaptive FIFO target in native samples.
 * @param input_rate Link-side sample rate in samples per second.
 * @param keyed Nonzero keeps the transmitter keyed while the program drains.
 */
static void check_resampled_fifo_short_burst(unsigned int target_samples, unsigned int input_rate,
					     int keyed)
{
	struct chan_usbradio_pvt channel = {0};
	short program[URP_NATIVE_SAMPLES];
	size_t program_samples = input_rate / 50U;
	double input_energy = 0.0;

	initialize_src_burst_channel(&channel, target_samples, input_rate, keyed);
	for (size_t i = 0; i < program_samples; ++i) {
		double frequency = i < 3U * program_samples / 4U ? 1000.0 : 2000.0;
		program[i] = (short)(5000.0 * sin(2.0 * M_PI * frequency * i / input_rate));
		input_energy += (double)program[i] * program[i];
	}

	for (unsigned int burst = 0; burst < 2; ++burst) {
		unsigned int max_ticks =
			(target_samples + URP_NATIVE_SAMPLES - 1U) / URP_NATIVE_SAMPLES + 3U;
		double output_energy = 0.0;
		double tail_sine = 0.0, tail_cosine = 0.0;
		unsigned int tick;

		/* No later program frame is available to satisfy the sinc filter's lookahead. */
		usbradioplus_queue_program(&channel, program, program_samples);
		assert(channel.plus_program_queue.count <= URP_PROGRAM_QUEUE_FRAMES);
		for (tick = 0; tick < max_ticks; ++tick) {
			usbradioplus_native_tick(&channel);
			if (!tick)
				assert(channel.plus_native_fifo.primed);
			assert(channel.txkeyed == keyed);
			assert(!channel.plus_link_queue_overflows && !channel.plus_src_errors);
			assert(channel.plus_native_fifo.target_samples >= URP_FIFO_TARGET_MIN);
			assert(channel.plus_native_fifo.target_samples <= URP_FIFO_TARGET_MAX);
			for (size_t i = 0; i < URP_NATIVE_SAMPLES; ++i) {
				double phase = 2.0 * M_PI * 2000.0 *
					       (tick * URP_NATIVE_SAMPLES + i) / URP_RATE_NATIVE;
				output_energy += (double)channel.plus_link_native[i] *
						 channel.plus_link_native[i];
				tail_sine += channel.plus_link_native[i] * sin(phase);
				tail_cosine += channel.plus_link_native[i] * cos(phase);
			}
			if (!channel.plus_native_fifo.primed)
				break;
		}
		assert(tick < max_ticks);
		assert(!channel.plus_program_queue.count && !channel.plus_native_fifo.count);
		assert(!channel.plus_link_src_pending);
		/* Preserve most of the tone, allowing sinc ringing and the final concealment fade.
		 */
		assert(output_energy >=
		       0.70 * input_energy * ((double)URP_RATE_NATIVE / input_rate));
		/* The final 5 ms marker must escape the SRC, not just the initial voice energy. */
		assert(hypot(tail_sine, tail_cosine) > 0.70 * 5000.0 * URP_NATIVE_SAMPLES / 8.0);
		assert(channel.plus_link_queue_underflows == (keyed ? burst + 1U : 0));
		if (keyed && target_samples < URP_FIFO_TARGET_MAX)
			target_samples += URP_FIFO_TARGET_STEP;
		assert(channel.plus_native_fifo.target_samples == target_samples);
	}
	usbradioplus_dsp_destroy(&channel);
	assert(!urp_radio_destroy(channel.radio));
}

/** @brief Preserve isolated link-rate bursts at the minimum, normal, and maximum FIFO targets. */
static void test_resampled_fifo_short_bursts(void)
{
	check_resampled_fifo_short_burst(URP_FIFO_TARGET_MIN, URP_RATE_LINK, 1);
	check_resampled_fifo_short_burst(URP_FIFO_TARGET_NORMAL, URP_RATE_LINK, 1);
	check_resampled_fifo_short_burst(URP_FIFO_TARGET_MAX, URP_RATE_LINK, 1);
	check_resampled_fifo_short_burst(URP_FIFO_TARGET_NORMAL, URP_RATE_LINK, 0);
	check_resampled_fifo_short_burst(URP_FIFO_TARGET_MIN, 16000, 1);
	check_resampled_fifo_short_burst(URP_FIFO_TARGET_MAX, 16000, 1);
	check_resampled_fifo_short_burst(URP_FIFO_TARGET_MIN, 24000, 1);
	check_resampled_fifo_short_burst(URP_FIFO_TARGET_MAX, 24000, 1);
}

/** @brief Compare real rendered samples and retained stream state across key transitions.
 * @param channel Channel whose PTT state changes while samples continue arriving.
 * @param reference Continuously keyed channel receiving the identical input stream.
 */
static void assert_resampled_stream_matches(const struct chan_usbradio_pvt *channel,
					    const struct chan_usbradio_pvt *reference)
{
	assert(!memcmp(channel->plus_link_native, reference->plus_link_native,
		       sizeof(channel->plus_link_native)));
	assert(!memcmp(channel->usbradio_write_buf, reference->usbradio_write_buf,
		       sizeof(channel->usbradio_write_buf)));
	assert(channel->plus_native_fifo.head == reference->plus_native_fifo.head);
	assert(channel->plus_native_fifo.count == reference->plus_native_fifo.count);
	assert(channel->plus_native_fifo.primed == reference->plus_native_fifo.primed);
	assert(channel->plus_native_fifo.have_history == reference->plus_native_fifo.have_history);
	assert(channel->plus_native_fifo.concealing == reference->plus_native_fifo.concealing);
	assert(channel->plus_native_fifo.target_samples ==
	       reference->plus_native_fifo.target_samples);
	assert(!memcmp(channel->plus_native_fifo.history, reference->plus_native_fifo.history,
		       sizeof(channel->plus_native_fifo.history)));
	assert(channel->plus_link_src_pending == reference->plus_link_src_pending);
	assert(channel->plus_link_clock.correction == reference->plus_link_clock.correction);
	assert(channel->plus_link_clock.filtered_error ==
	       reference->plus_link_clock.filtered_error);
	assert(channel->plus_link_clock.integral_error ==
	       reference->plus_link_clock.integral_error);
	assert(!channel->plus_link_queue_underflows && !channel->plus_link_queue_overflows);
	assert(!channel->plus_src_errors);
}

/** @brief Preserve admitted audio, sinc history, and clock recovery across arbitrary PTT edges. */
static void test_resampled_fifo_continuity_and_rekey(void)
{
	struct chan_usbradio_pvt channel = {0};
	struct chan_usbradio_pvt reference = {0};
	struct ast_channel *owner = (struct ast_channel *)(uintptr_t)1;
	short program[URP_LINK_SAMPLES];
	short silence[URP_LINK_SAMPLES] = {0};
	struct ast_frame voice = {
		.frametype = AST_FRAME_VOICE, .data.ptr = program, .datalen = sizeof(program)};
	const int keyed[] = {0, 0, 1, 1, 0, 1, 0, 0};
	unsigned int stable_blocks = 0;
	double expected_energy = 0.0;

	initialize_src_burst_channel(&channel, URP_FIFO_TARGET_NORMAL, URP_RATE_LINK, 0);
	initialize_src_burst_channel(&reference, URP_FIFO_TARGET_NORMAL, URP_RATE_LINK, 1);
	channel.hasusb = 1;
#ifdef URP_TEST_MODERN
	channel.audio_thread_ready = 1;
#else
	channel.sounddev = 7;
#endif
	test_channel_private = &channel;
	for (size_t i = 0; i < ARRAY_LEN(program); ++i) {
		program[i] = (short)(5000.0 * sin(2.0 * M_PI * 1000.0 * i / URP_RATE_LINK));
		expected_energy +=
			(double)program[i] * program[i] * ((double)URP_RATE_NATIVE / URP_RATE_LINK);
	}
	for (unsigned int frame = 0; frame < 24; ++frame) {
		double energy = 0.0;
		unsigned int queued = channel.plus_program_queue.count;

		channel.txkeyed = keyed[frame % ARRAY_LEN(keyed)];
		/* Idle silence and unkeyed voice both belong to the same input stream. */
		voice.data.ptr = frame >= 16 && frame < 20 ? silence : program;
		channel.echoing = 1;
		assert(!usbradio_write(owner, &voice));
		assert(channel.plus_program_queue.count == queued);
		channel.echoing = 0;
		assert(!usbradio_write(owner, &voice));
		assert(channel.plus_program_queue.count > queued);
		usbradioplus_queue_program(&reference, voice.data.ptr, ARRAY_LEN(program));
		usbradioplus_native_tick(&channel);
		usbradioplus_native_tick(&reference);
		assert_resampled_stream_matches(&channel, &reference);
		assert(channel.plus_native_fifo.primed && channel.plus_link_src_pending);
		stable_blocks += channel.txkeyed;
		assert(channel.plus_native_fifo.stable_blocks == stable_blocks);
		for (size_t i = 0; i < URP_NATIVE_SAMPLES; ++i)
			energy += (double)channel.plus_link_native[i] * channel.plus_link_native[i];
		/* Once startup silence passes, padding must not introduce gaps between frames. */
		if (frame >= 8 && frame < 16)
			assert(energy > 0.90 * expected_energy && energy < 1.10 * expected_energy);
	}

	/* Key changes also preserve buffered audio when no new frame arrives that tick. */
	for (unsigned int tick = 0; tick < 3; ++tick) {
		channel.txkeyed = tick % 2;
		usbradioplus_native_tick(&channel);
		usbradioplus_native_tick(&reference);
		assert_resampled_stream_matches(&channel, &reference);
		assert(channel.plus_native_fifo.primed && channel.plus_link_src_pending);
		stable_blocks += channel.txkeyed;
		assert(channel.plus_native_fifo.stable_blocks == stable_blocks);
	}
	test_channel_private = NULL;
	usbradioplus_dsp_destroy(&channel);
	usbradioplus_dsp_destroy(&reference);
	assert(!urp_radio_destroy(channel.radio));
	assert(!urp_radio_destroy(reference.radio));
}

/** @brief Stop padding after either a rejected or partially consumed synthetic tail frame. */
static void test_resampled_fifo_tail_failures(void)
{
	for (unsigned int failure = 0; failure < 2; ++failure) {
		struct chan_usbradio_pvt channel = {0};
		short program[URP_LINK_SAMPLES] = {0};

		initialize_src_burst_channel(&channel, URP_FIFO_TARGET_NORMAL, URP_RATE_LINK, 1);
		usbradioplus_queue_program(&channel, program, ARRAY_LEN(program));
		usbradioplus_native_tick(&channel);
		assert(channel.plus_native_fifo.primed);
		for (unsigned int tick = 0; tick < URP_PROGRAM_QUEUE_FRAMES &&
					    channel.plus_native_fifo.count >= URP_NATIVE_SAMPLES;
		     ++tick)
			usbradioplus_native_tick(&channel);
		assert(channel.plus_native_fifo.count < URP_NATIVE_SAMPLES);
		assert(channel.plus_link_src_pending && !channel.plus_program_queue.count);
		src_process_calls = 0;
		if (failure)
			partial_src_process_call = 2;
		else
			fail_src_process_call = 2;
		usbradioplus_native_tick(&channel);
		assert(src_process_calls == 2); /* Receive downsampling, then the synthetic tail. */
		assert(channel.plus_src_errors == 1 && channel.plus_link_queue_underflows == 1);
		assert(!channel.plus_link_src_pending && !channel.plus_native_fifo.primed);
		assert(!channel.plus_native_fifo.count && !channel.plus_link_queue_overflows);
		assert(channel.plus_native_fifo.target_samples ==
		       URP_FIFO_TARGET_NORMAL + URP_FIFO_TARGET_STEP);
		fail_src_process_call = 0;
		partial_src_process_call = 0;
		src_process_calls = 0;
		usbradioplus_native_tick(&channel);
		assert(src_process_calls == 1);
		assert(channel.plus_src_errors == 1 && channel.plus_link_queue_underflows == 1);
		assert(!channel.plus_link_src_pending && channel.txkeyed);
		usbradioplus_dsp_destroy(&channel);
		assert(!urp_radio_destroy(channel.radio));
	}
}

/** @brief Apply sample-rate DSP carrier gating without changing non-DSP receive sources. */
static void test_native_sample_gate(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio_config = {
		.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	static const char *const carrier_sources[] = {"dsp", "usb",	 "usbinvert", "vox",
						      "pp",  "ppinvert", "no"};
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
	channel.plus_emphasis_corner_hz = 300.0;
	channel.radio = urp_radio_create(&radio_config, URP_LINK_SAMPLES);
	assert(channel.radio);
	assert(!usbradioplus_dsp_init(&channel));
	for (size_t i = 0; i < URP_NATIVE_SAMPLES; ++i) {
		capture[2 * i] = 1000;
		channel.radio->rxCarrierGate[i] = i < URP_NATIVE_SAMPLES / 2;
	}

	for (size_t source = 0; source < ARRAY_LEN(carrier_sources); ++source) {
		strcpy(settings.profiles[0].hardware.cos_assignment, carrier_sources[source]);
		usbradioplus_native_tick(&channel);
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

	memset(&usbradio_default, 0, sizeof(usbradio_default));
	usbradio_default.plus_emphasis_corner_hz = 250.0;
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
	usbradio_default.plus_emphasis_corner_hz = 0.0;
	assert(store_config("usb") == NULL);
	usbradio_default.plus_emphasis_corner_hz = 300.0;
	assert(store_config("usb") == NULL);
	usbradio_default.plus_emphasis_corner_hz = 250.0;

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
	assert(created->rxsquelchdelay == RXSQDELAYBUFSIZE / 8 - 1);
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

/** @brief Verify native queue pacing without SRC, sidetone duplication, or clock correction. */
static void test_advanced_native_clock(void)
{
	struct chan_usbradio_pvt channel = {
		.name = "test", .plus_hardware_applied = 1, .plus_emphasis_corner_hz = 300.0};
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
	assert(channel.plus_app_rpt_rate == 48000 && channel.plus_app_rpt_samples == 960);
	short program[URP_NATIVE_SAMPLES];
	for (size_t i = 0; i < URP_NATIVE_SAMPLES; ++i)
		program[i] = 123;
	channel.rxkeyed = channel.txkeyed = 1;
	channel.duplex3 = 999;
	channel.duplex3mode = DUPLEX3_MODE_SOFTWARE;
	channel.plus_parrot_playing = 1;
	usbradioplus_queue_program(&channel, program, URP_NATIVE_SAMPLES);
	assert(channel.plus_program_queue.count == 1);
	src_process_calls = 0;
	usbradioplus_native_tick(&channel);
	assert(!channel.plus_program_queue.count && !channel.plus_native_fifo.count);
	assert(channel.plus_link_native[0] == 123 && channel.plus_link_native[959] == 123);
	assert(src_process_calls == 0 && channel.plus_parrot_playing && !channel.plus_parrot_play);
	assert(channel.plus_link_queue_underflows == 0);
	usbradioplus_native_tick(&channel);
	assert(channel.plus_link_queue_underflows == 1);
	channel.txkeyed = 0;
	usbradioplus_native_tick(&channel);
	assert(channel.plus_link_queue_underflows == 1);
	usbradioplus_interface_mode(&channel, 0);
	assert(!channel.plus_advanced && channel.plus_app_rpt_rate == 8000 &&
	       channel.plus_app_rpt_samples == 160);
	usbradioplus_queue_program(&channel, program, 160);
	assert(channel.plus_program_queue.count > 1);
	channel.plus_parrot_playing = 0;
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
	RUN_TEST(test_modern_hid_worker_retries);
	RUN_TEST(test_modern_audio_worker_baseline);
	RUN_TEST(test_modern_module_lifecycle_baseline);
#endif
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
	RUN_TEST(test_processing_config_overrides);
	RUN_TEST(test_processing_override_parse_edges);
	RUN_TEST(test_shared_config_loading);
	RUN_TEST(test_effective_processing_settings);
	RUN_TEST(test_numeric_helpers);
	RUN_TEST(test_shared_hardware_layouts);
	RUN_TEST(test_shared_control_helpers);
	RUN_TEST(test_shared_receive_signaling_helpers);
	RUN_TEST(test_shared_eeprom_wait);
	RUN_TEST(test_continuous_soundcard_output);
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
	RUN_TEST(test_program_queue_and_parrot_storage);
	RUN_TEST(test_dsp_init_failures);
	RUN_TEST(test_native_tick_baseline);
	RUN_TEST(test_local_processing_cpu_saver);
	RUN_TEST(test_native_fifo_short_bursts);
	RUN_TEST(test_resampled_fifo_short_bursts);
	RUN_TEST(test_resampled_fifo_continuity_and_rekey);
	RUN_TEST(test_resampled_fifo_tail_failures);
	RUN_TEST(test_native_sample_gate);
	RUN_TEST(test_unlinked_channel_cleanup);
	RUN_TEST(test_store_config_failure_and_option_edges);
	RUN_TEST(test_advanced_adapter);
	RUN_TEST(test_advanced_native_clock);
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

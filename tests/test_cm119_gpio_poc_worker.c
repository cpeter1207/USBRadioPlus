/**
 * \file test_cm119_gpio_poc_worker.c
 * \brief No-hardware lifecycle tests for the shared CM119 GPIO POC worker.
 */

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "usbradioplus_cm119_gpio_poc_worker.h"

/** \brief Deterministic worker outcome exercised by one fake callback table. */
enum worker_test_case {
	WORKER_TEST_VALIDATE_FAILURE,
	WORKER_TEST_START_FAILURE,
	WORKER_TEST_INITIAL_SERVICE_FAILURE,
	WORKER_TEST_WAKE_FAILURE,
	WORKER_TEST_STOP_REQUEST,
	WORKER_TEST_POLL_FAILURE,
	WORKER_TEST_DRAIN_FAILURE,
	WORKER_TEST_SERVICE_FAILURE,
	WORKER_TEST_TIMEOUT,
	WORKER_TEST_WAKE_SUCCESS,
	WORKER_TEST_OFFLINE,
	WORKER_TEST_RETRY,
};

/** \brief Fake adapter state and a compact callback-order trace. */
struct worker_test_state {
	enum worker_test_case test_case;
	char trace[32];
	size_t trace_length;
	int stop;
	int online;
	unsigned int reports;
	unsigned int services;
	enum usbradioplus_cm119_gpio_poc_worker_event last_event;
};

/** \brief Active fake worker used by deterministic system-call replacements. */
static struct worker_test_state *active_worker;

/** \brief Return the scripted wake result without touching a physical descriptor. */
int poll(struct pollfd *descriptors, nfds_t count, int timeout)
{
	const int ready = active_worker->test_case != WORKER_TEST_TIMEOUT;
	assert(count == 1U && timeout == 5);
	if (active_worker->test_case == WORKER_TEST_POLL_FAILURE) {
		errno = EINTR;
		return -1;
	}
	descriptors[0].revents = ready ? POLLIN : 0;
	return ready;
}

/** \brief Observe retry backoff without sleeping in a unit test. */
int usleep(useconds_t microseconds)
{
	assert(microseconds == 500000U);
	assert(active_worker->test_case == WORKER_TEST_RETRY);
	active_worker->stop = 1;
	return 0;
}

/** \brief Record one lifecycle callback in the fake adapter trace. */
static void worker_test_trace(struct worker_test_state *state, char event)
{
	assert(state->trace_length + 1U < sizeof(state->trace));
	state->trace[state->trace_length++] = event;
	state->trace[state->trace_length] = '\0';
}

/** \brief Return whether the fake adapter has requested shutdown. */
static int worker_test_stop_requested(void *opaque)
{
	return ((struct worker_test_state *)opaque)->stop;
}

/** \brief Return whether the fake adapter reports an online device. */
static int worker_test_online(void *opaque)
{
	return ((struct worker_test_state *)opaque)->online;
}

/** \brief Clear fake published state before worker validation. */
static void worker_test_clear(void *opaque)
{
	worker_test_trace(opaque, 'C');
}

/** \brief Fail validation only in the validation-failure scenario. */
static int worker_test_validate(void *opaque)
{
	struct worker_test_state *state = opaque;

	worker_test_trace(state, 'V');
	return state->test_case == WORKER_TEST_VALIDATE_FAILURE;
}

/** \brief Start one fake ownership attempt. */
static int worker_test_start(void *opaque)
{
	struct worker_test_state *state = opaque;

	worker_test_trace(state, 'A');
	return state->test_case == WORKER_TEST_START_FAILURE ||
	       state->test_case == WORKER_TEST_RETRY;
}

/** \brief Service fake HID state and trigger the selected deterministic exit. */
static int worker_test_service(void *opaque)
{
	struct worker_test_state *state = opaque;

	worker_test_trace(state, 'S');
	state->services++;
	if (state->test_case == WORKER_TEST_INITIAL_SERVICE_FAILURE)
		return -1;
	if (state->test_case == WORKER_TEST_STOP_REQUEST)
		state->stop = 1;
	if (state->services > 1U) {
		if (state->test_case == WORKER_TEST_SERVICE_FAILURE)
			return -2;
		state->stop = 1;
	}
	return 0;
}

/** \brief Mark the fake hardware online after its initial service pass. */
static void worker_test_mark_online(void *opaque)
{
	struct worker_test_state *state = opaque;

	worker_test_trace(state, 'O');
	state->online = state->test_case != WORKER_TEST_OFFLINE;
}

/** \brief Return an invalid wake descriptor in the deterministic wake-failure test. */
static int worker_test_wake_fd(void *opaque)
{
	worker_test_trace(opaque, 'F');
	return ((struct worker_test_state *)opaque)->test_case == WORKER_TEST_WAKE_FAILURE ? -1 : 7;
}

/** \brief Unused because the fake wake descriptor fails before a poll. */
static int worker_test_drain(void *opaque)
{
	worker_test_trace(opaque, 'D');
	return ((struct worker_test_state *)opaque)->test_case == WORKER_TEST_DRAIN_FAILURE;
}

/** \brief Record ordered shutdown and prevent another retry. */
static void worker_test_stop(void *opaque)
{
	struct worker_test_state *state = opaque;

	worker_test_trace(state, 'T');
	state->stop = state->test_case != WORKER_TEST_RETRY;
}

/** \brief Record identity release after the fake attempt is stopped. */
static void worker_test_release(void *opaque)
{
	worker_test_trace(opaque, 'R');
}

/** \brief Record the common worker failure classification. */
static void worker_test_report(void *opaque, enum usbradioplus_cm119_gpio_poc_worker_event event,
			       int detail)
{
	struct worker_test_state *state = opaque;

	(void)detail;
	worker_test_trace(state, 'E');
	state->reports++;
	state->last_event = event;
	if (state->test_case == WORKER_TEST_POLL_FAILURE)
		state->stop = 1;
}

/** \brief Build one complete fake callback table. */
static struct usbradioplus_cm119_gpio_poc_worker_ops
worker_test_ops(struct worker_test_state *state)
{
	return (struct usbradioplus_cm119_gpio_poc_worker_ops){
		.struct_size = sizeof(struct usbradioplus_cm119_gpio_poc_worker_ops),
		.opaque = state,
		.stop_requested = worker_test_stop_requested,
		.online = worker_test_online,
		.clear_published_state = worker_test_clear,
		.validate = worker_test_validate,
		.start_attempt = worker_test_start,
		.service = worker_test_service,
		.mark_online = worker_test_mark_online,
		.wake_read_fd = worker_test_wake_fd,
		.drain_wake = worker_test_drain,
		.stop_attempt = worker_test_stop,
		.release_identity = worker_test_release,
		.report = worker_test_report,
	};
}

/** \brief Run one deterministic scenario and verify its exact lifecycle order. */
static void worker_test_run(enum worker_test_case test_case, const char *expected_trace,
			    unsigned int expected_reports,
			    enum usbradioplus_cm119_gpio_poc_worker_event expected_event)
{
	struct worker_test_state state = {.test_case = test_case};
	struct usbradioplus_cm119_gpio_poc_worker_ops ops = worker_test_ops(&state);

	active_worker = &state;
	assert(usbradioplus_cm119_gpio_poc_worker_run(&ops) == NULL);
	assert(!strcmp(state.trace, expected_trace));
	assert(state.reports == expected_reports);
	if (expected_reports)
		assert(state.last_event == expected_event);
}

/** \brief Verify validation failure has no partially-started cleanup. */
static void test_validation_failure(void)
{
	worker_test_run(WORKER_TEST_VALIDATE_FAILURE, "CVE", 1U,
			USBRADIOPLUS_CM119_GPIO_POC_WORKER_VALIDATE_FAILED);
}

/** \brief Verify a failed start stops before it releases the reservation. */
static void test_start_failure_cleanup(void)
{
	worker_test_run(WORKER_TEST_START_FAILURE, "CVAETR", 1U,
			USBRADIOPLUS_CM119_GPIO_POC_WORKER_START_FAILED);
}

/** \brief Verify a failed initial service follows the same cleanup ordering. */
static void test_initial_service_failure_cleanup(void)
{
	worker_test_run(WORKER_TEST_INITIAL_SERVICE_FAILURE, "CVASETR", 1U,
			USBRADIOPLUS_CM119_GPIO_POC_WORKER_INITIAL_SERVICE_FAILED);
}

/** \brief Verify an unusable wake descriptor retires a live attempt safely. */
static void test_wake_failure_cleanup(void)
{
	worker_test_run(WORKER_TEST_WAKE_FAILURE, "CVASOFETR", 1U,
			USBRADIOPLUS_CM119_GPIO_POC_WORKER_WAKE_FAILED);
}

/** \brief Verify a stop observed after initial service skips polling and retries. */
static void test_stop_request_cleanup(void)
{
	worker_test_run(WORKER_TEST_STOP_REQUEST, "CVASOTR", 0U,
			USBRADIOPLUS_CM119_GPIO_POC_WORKER_VALIDATE_FAILED);
}

/** \brief Reject each incomplete callback table before invoking any callback. */
static void test_incomplete_ops(void)
{
	struct worker_test_state state = {0};
	struct usbradioplus_cm119_gpio_poc_worker_ops ops = worker_test_ops(&state);
	assert(usbradioplus_cm119_gpio_poc_worker_run(NULL) == NULL);
	ops.struct_size = 0U;
	assert(usbradioplus_cm119_gpio_poc_worker_run(&ops) == NULL);
#define CHECK_MISSING_CALLBACK(member)                                                             \
	do {                                                                                       \
		ops = worker_test_ops(&state);                                                     \
		ops.member = NULL;                                                                 \
		assert(usbradioplus_cm119_gpio_poc_worker_run(&ops) == NULL);                      \
	} while (0)
	CHECK_MISSING_CALLBACK(stop_requested);
	CHECK_MISSING_CALLBACK(online);
	CHECK_MISSING_CALLBACK(clear_published_state);
	CHECK_MISSING_CALLBACK(validate);
	CHECK_MISSING_CALLBACK(start_attempt);
	CHECK_MISSING_CALLBACK(service);
	CHECK_MISSING_CALLBACK(mark_online);
	CHECK_MISSING_CALLBACK(wake_read_fd);
	CHECK_MISSING_CALLBACK(drain_wake);
	CHECK_MISSING_CALLBACK(stop_attempt);
	CHECK_MISSING_CALLBACK(release_identity);
	CHECK_MISSING_CALLBACK(report);
#undef CHECK_MISSING_CALLBACK
	assert(state.trace_length == 0U);
}

/** \brief Exercise shared-worker sequencing without a CM119 or Asterisk process. */
int main(void)
{
	test_validation_failure();
	test_start_failure_cleanup();
	test_initial_service_failure_cleanup();
	test_wake_failure_cleanup();
	test_stop_request_cleanup();
	test_incomplete_ops();
	worker_test_run(WORKER_TEST_POLL_FAILURE, "CVASOFETR", 1U,
			USBRADIOPLUS_CM119_GPIO_POC_WORKER_POLL_FAILED);
	worker_test_run(WORKER_TEST_DRAIN_FAILURE, "CVASOFDETR", 1U,
			USBRADIOPLUS_CM119_GPIO_POC_WORKER_WAKE_FAILED);
	worker_test_run(WORKER_TEST_SERVICE_FAILURE, "CVASOFDSETR", 1U,
			USBRADIOPLUS_CM119_GPIO_POC_WORKER_SERVICE_FAILED);
	worker_test_run(WORKER_TEST_TIMEOUT, "CVASOFSTR", 0U,
			USBRADIOPLUS_CM119_GPIO_POC_WORKER_SERVICE_FAILED);
	worker_test_run(WORKER_TEST_WAKE_SUCCESS, "CVASOFDSTR", 0U,
			USBRADIOPLUS_CM119_GPIO_POC_WORKER_SERVICE_FAILED);
	worker_test_run(WORKER_TEST_OFFLINE, "CVASOTR", 0U,
			USBRADIOPLUS_CM119_GPIO_POC_WORKER_SERVICE_FAILED);
	worker_test_run(WORKER_TEST_RETRY, "CVAETR", 1U,
			USBRADIOPLUS_CM119_GPIO_POC_WORKER_START_FAILED);
	return 0;
}

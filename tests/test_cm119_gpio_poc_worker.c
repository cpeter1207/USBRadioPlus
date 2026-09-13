/**
 * \file test_cm119_gpio_poc_worker.c
 * \brief No-hardware lifecycle tests for the shared CM119 GPIO POC worker.
 */

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "usbradioplus_cm119_gpio_poc_worker.h"

/** \brief Deterministic worker outcome exercised by one fake callback table. */
enum worker_test_case {
	WORKER_TEST_VALIDATE_FAILURE,
	WORKER_TEST_START_FAILURE,
	WORKER_TEST_INITIAL_SERVICE_FAILURE,
	WORKER_TEST_WAKE_FAILURE,
	WORKER_TEST_STOP_REQUEST,
};

/** \brief Fake adapter state and a compact callback-order trace. */
struct worker_test_state {
	enum worker_test_case test_case;
	char trace[32];
	size_t trace_length;
	int stop;
	int online;
	unsigned int reports;
	enum usbradioplus_cm119_gpio_poc_worker_event last_event;
};

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
	return state->test_case == WORKER_TEST_START_FAILURE;
}

/** \brief Service fake HID state and trigger the selected deterministic exit. */
static int worker_test_service(void *opaque)
{
	struct worker_test_state *state = opaque;

	worker_test_trace(state, 'S');
	if (state->test_case == WORKER_TEST_INITIAL_SERVICE_FAILURE)
		return -1;
	if (state->test_case == WORKER_TEST_STOP_REQUEST)
		state->stop = 1;
	return 0;
}

/** \brief Mark the fake hardware online after its initial service pass. */
static void worker_test_mark_online(void *opaque)
{
	struct worker_test_state *state = opaque;

	worker_test_trace(state, 'O');
	state->online = 1;
}

/** \brief Return an invalid wake descriptor in the deterministic wake-failure test. */
static int worker_test_wake_fd(void *opaque)
{
	worker_test_trace(opaque, 'F');
	return -1;
}

/** \brief Unused because the fake wake descriptor fails before a poll. */
static int worker_test_drain(void *opaque)
{
	worker_test_trace(opaque, 'D');
	return 0;
}

/** \brief Record ordered shutdown and prevent another retry. */
static void worker_test_stop(void *opaque)
{
	struct worker_test_state *state = opaque;

	worker_test_trace(state, 'T');
	state->stop = 1;
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

/** \brief Exercise shared-worker sequencing without a CM119 or Asterisk process. */
int main(void)
{
	test_validation_failure();
	test_start_failure_cleanup();
	test_initial_service_failure_cleanup();
	test_wake_failure_cleanup();
	test_stop_request_cleanup();
	return 0;
}

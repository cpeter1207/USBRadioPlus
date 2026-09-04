#include "../src/usbradioplus_tune_internal.h"

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int flat_configuration = 1;
static int response_result;
static int command_result;
static int response_calls;
static int line_result;
static const char *line_failure_command;
static const char *device_list_override;
static const char *setup_override;
static const char *options_override;
static const char *active_override;
static const char *duplex_mode_response;
static const char *response_failure_command;
static int copy_failure_call;
static int copy_calls;
static int wait_results[4];
static int wait_errnos[4];
static size_t wait_result_count;
static size_t wait_result_index;
static int read_results[4];
static int read_errnos[4];
static size_t read_result_count;
static size_t read_result_index;

static int fail_pipe(int pipefd[2])
{
	(void)pipefd;
	errno = EMFILE;
	return -1;
}

static int fail_nonblocking(int fd)
{
	(void)fd;
	errno = EIO;
	return -1;
}

static int fail_open_null(void)
{
	errno = EMFILE;
	return -1;
}

static pid_t fail_fork(void)
{
	errno = EAGAIN;
	return -1;
}

static int selective_copy_fd(int oldfd, int newfd)
{
	copy_calls++;
	if (copy_calls == copy_failure_call) {
		errno = EBADF;
		return -1;
	}
	return dup2(oldfd, newfd);
}

static int scripted_waitfds(int fd1, int fd2, int ms)
{
	if (wait_result_index < wait_result_count) {
		int result = wait_results[wait_result_index];
		errno = wait_errnos[wait_result_index++];
		return result;
	}
	return waitfds_real(fd1, fd2, ms);
}

static ssize_t scripted_read(int fd, void *buffer, size_t count)
{
	if (read_result_index < read_result_count) {
		int result = read_results[read_result_index];
		errno = read_errnos[read_result_index++];
		return result;
	}
	return read(fd, buffer, count);
}

static void set_wait_script(int first_result, int first_errno)
{
	wait_results[0] = first_result;
	wait_errnos[0] = first_errno;
	wait_result_count = 1;
	wait_result_index = 0;
	waitfds = scripted_waitfds;
}

static void set_read_script(int first_result, int first_errno)
{
	read_results[0] = first_result;
	read_errnos[0] = first_errno;
	read_result_count = 1;
	read_result_index = 0;
	read_bytes = scripted_read;
}

static int fake_astgetline(char *command, char *result, int maximum)
{
	const char *value = "";
	if (line_result)
		return line_result;
	if (line_failure_command && strstr(command, line_failure_command))
		return 1;

	if (strstr(command, "0+9")) {
		value = setup_override ? setup_override
			: flat_configuration
				? "1,1,0,0,0,1,1,10,20,1,0,2,1,4,500,1.0,600,700,800,900,100,100,"
				  "100"
				: "0,0,1,0,0,0,0,0,0,0,0,1,0,0,400,0.5,300,400,500,600,50,50,50";
	} else if (strstr(command, "menu-support 0")) {
		value = options_override ? options_override : "1,1,0,0,0,1,1,10,20,1,0,2,1,4";
	} else if (strstr(command, "menu-support 1")) {
		value = device_list_override ? device_list_override : "beta,alpha";
	} else if (strstr(command, "menu-support 3")) {
		value = device_list_override ? device_list_override : "delta,charlie";
	} else if (strstr(command, "menu-support L")) {
		value = "TX soft limiting setpoint currently set to: 12000";
	} else if (strstr(command, "menu-support D")) {
		value = "Duplex 3 level currently set to: 500";
	} else if (strstr(command, "menu-support M")) {
		value = duplex_mode_response ? duplex_mode_response
					     : "Duplex 3 mode currently set to: software";
	} else if (strstr(command, "active test")) {
		value = active_override ? active_override
					: "Active (command) USB Radio device set to test";
	}
	snprintf(result, (size_t)maximum + 1, "%s", value);
	return 0;
}

static int fake_astgetresp(char *command)
{
	response_calls++;
	if (response_failure_command && strstr(command, response_failure_command))
		return 1;
	return response_result;
}

static int fake_run_command(const char *command)
{
	assert(strcmp(command, "/usr/sbin/usbradioplus-processing-tune") == 0);
	return command_result;
}

static void use_input(const char *text)
{
	int input[2];
	ssize_t length = (ssize_t)strlen(text);

	assert(pipe(input) == 0);
	assert(write(input[1], text, (size_t)length) == length);
	assert(close(input[1]) == 0);
	assert(dup2(input[0], STDIN_FILENO) == STDIN_FILENO);
	assert(close(input[0]) == 0);
	clearerr(stdin);
}

static void test_explode_string(void)
{
	char empty[] = "";
	char plain[] = "charlie,alpha,bravo";
	char quoted[] = "one,\"two,three\",four";
	char limited[] = "one,two,three";
	char *parts[8];

	assert(explode_string(plain, parts, 0, ',', 0) == 0);
	assert(explode_string(empty, parts, 8, ',', 0) == 0);
	assert(explode_string(plain, parts, 8, ',', 0) == 3);
	qsort(parts, 3, sizeof(*parts), qcompar);
	assert(strcmp(parts[0], "alpha") == 0);
	assert(strcmp(parts[1], "bravo") == 0);
	assert(strcmp(parts[2], "charlie") == 0);
	assert(explode_string(quoted, parts, 8, ',', '"') == 3);
	assert(strcmp(parts[1], "two,three") == 0);
	assert(explode_string(limited, parts, 2, ',', 0) == 1);
}

static void test_integer_parser(void)
{
	int value = -1;

	assert(parse_integer(NULL, 0, 9, &value) == -1);
	assert(parse_integer("", 0, 9, &value) == -1);
	assert(parse_integer("999999999999999999999999", 0, 9, &value) == -1);
	assert(parse_integer("1x", 0, 9, &value) == -1);
	assert(parse_integer("-1", 0, 9, &value) == -1);
	assert(parse_integer("10", 0, 9, &value) == -1);
	assert(parse_integer("7", 0, 9, &value) == 0);
	assert(value == 7);
	{
		char empty[] = "";
		char line[] = "value\n";
		char plain[] = "value";
		strip_newline(empty);
		strip_newline(line);
		strip_newline(plain);
		assert(strcmp(line, "value") == 0);
		assert(strcmp(plain, "value") == 0);
	}
}

static void test_file_descriptor_helpers(void)
{
	int input[2];
	int other[2];
	char buffer[8];

	assert(pipe(input) == 0);
	assert(waitfds(input[0], -1, 0) == 0);
	assert(write(input[1], "a\nb", 3) == 3);
	assert(waitfds(input[0], -1, 10) == input[0] + 1);
	assert(getstrfd(input[0], buffer, sizeof(buffer) - 1) == 1);
	assert(strcmp(buffer, "a") == 0);
	assert(getcharfd(input[0]) == 'b');
	assert(close(input[1]) == 0);
	assert(getcharfd(input[0]) == -1);
	assert(close(input[0]) == 0);

	/* fd2 may legitimately be standard input, whose descriptor is zero. */
	use_input("x");
	assert(pipe(input) == 0);
	assert(waitfds_real(input[0], STDIN_FILENO, 10) == STDIN_FILENO + 1);
	assert(close(input[0]) == 0);
	assert(close(input[1]) == 0);
	assert(pipe(input) == 0);
	assert(pipe(other) == 0);
	assert(write(other[1], "q", 1) == 1);
	assert(waitfds_real(input[0], other[0], 10) == other[0] + 1);
	assert(close(input[0]) == 0);
	assert(close(input[1]) == 0);
	assert(close(other[0]) == 0);
	assert(close(other[1]) == 0);
	assert(pipe(input) == 0);
	assert(write(input[1], "q", 1) == 1);
	assert(waitfds_real(input[0], input[1], 10) == input[0] + 1);
	assert(close(input[0]) == 0);
	assert(close(input[1]) == 0);

	assert(pipe(input) == 0);
	assert(write(input[1], "abc\n", 4) == 4);
	assert(getstrfd(input[0], NULL, 0) == 3);
	assert(close(input[0]) == 0);
	assert(close(input[1]) == 0);
	assert(pipe(input) == 0);
	assert(write(input[1], "xy", 2) == 2);
	assert(getstrfd(input[0], buffer, 1) == 1);
	assert(close(input[0]) == 0);
	assert(close(input[1]) == 0);
}

static void test_interrupted_io(void)
{
	int input[2];
	char buffer[8];

	assert(pipe(input) == 0);
	assert(write(input[1], "x\n", 2) == 2);
	set_wait_script(-1, EINTR);
	assert(getstrfd(input[0], buffer, sizeof(buffer) - 1) == 1);
	waitfds = waitfds_real;
	set_wait_script(-1, EIO);
	assert(getstrfd(input[0], buffer, sizeof(buffer) - 1) == 0);
	waitfds = waitfds_real;
	assert(write(input[1], "y\n", 2) == 2);
	set_read_script(-1, EINTR);
	assert(getstrfd(input[0], buffer, sizeof(buffer) - 1) == 1);
	read_bytes = read;
	assert(write(input[1], "z\n", 2) == 2);
	set_read_script(-1, EIO);
	assert(getstrfd(input[0], buffer, sizeof(buffer) - 1) == 0);
	read_bytes = read;
	assert(close(input[0]) == 0);
	assert(close(input[1]) == 0);
}

static void test_value_prompts(void)
{
	static const char *const values[] = {"first", "second"};

	use_input("2\n");
	assert(menu_select_value("test", values, 2, 0) == 2);
	use_input("\n");
	assert(menu_select_value("test", values, 2, 0) == 0);
	use_input("9\n");
	assert(menu_select_value("test", values, 2, 0) == 0);
	use_input("7\n");
	assert(menu_get_integer("test", 4, 1, 9) == 7);
	use_input("\n");
	assert(menu_get_integer("test", 4, 1, 9) == 4);
	use_input("bad\n");
	assert(menu_get_integer("test", 4, 1, 9) == 4);
	use_input("10\n");
	assert(menu_get_integer("test", 4, 1, 9) == 4);
}

static int run_tuner(const char *input, int argc, char **argv)
{
	use_input(input);
	optind = 1;
	response_result = 0;
	response_calls = 0;
	line_result = 0;
	line_failure_command = NULL;
	device_list_override = NULL;
	setup_override = NULL;
	options_override = NULL;
	active_override = NULL;
	duplex_mode_response = NULL;
	response_failure_command = NULL;
	astgetline = fake_astgetline;
	astgetresp = fake_astgetresp;
	run_command = fake_run_command;
	return usbradioplus_tune_main(argc, argv);
}

static void reset_fakes(const char *input)
{
	use_input(input);
	response_result = 0;
	response_calls = 0;
	line_result = 0;
	line_failure_command = NULL;
	device_list_override = NULL;
	setup_override = NULL;
	options_override = NULL;
	active_override = NULL;
	duplex_mode_response = NULL;
	response_failure_command = NULL;
	astgetline = fake_astgetline;
	astgetresp = fake_astgetresp;
	run_command = fake_run_command;
}

static void test_menu_input_edges(void)
{
	reset_fakes("");
	response_result = 1;
	menu_selectusb();
	reset_fakes("");
	menu_selectusb();
	reset_fakes("x\n");
	menu_selectusb();
	reset_fakes("0\n");
	menu_selectusb();
	reset_fakes("9\n");
	menu_selectusb();
	reset_fakes("");
	device_list_override = "";
	menu_selectusb();

	reset_fakes("");
	response_result = 1;
	menu_swapusb();
	reset_fakes("");
	menu_swapusb();
	reset_fakes("x\n");
	menu_swapusb();
	reset_fakes("0\n");
	menu_swapusb();
	reset_fakes("");
	device_list_override = "";
	menu_swapusb();

	reset_fakes("");
	menu_rxvoice();
	reset_fakes("bad\n\n");
	menu_rxvoice();
	reset_fakes("");
	menu_rxsquelch();
	reset_fakes("\n");
	menu_rxsquelch();
	reset_fakes("bad\n");
	menu_rxsquelch();
	reset_fakes("");
	response_result = 1;
	menu_rxsquelch();

	reset_fakes("");
	response_result = 1;
	menu_txvoice(0);
	reset_fakes("");
	menu_txvoice(1);
	reset_fakes("");
	menu_txvoice(0);
	reset_fakes("\n");
	menu_txvoice(1);
	reset_fakes("\n");
	menu_txvoice(0);
	reset_fakes("bad\n");
	menu_txvoice(0);
	reset_fakes("500\n");
	menu_txvoice(0);
	reset_fakes("");
	menu_auxvoice();
	reset_fakes("");
	response_result = 1;
	menu_auxvoice();
	reset_fakes("\n");
	menu_auxvoice();
	reset_fakes("bad\n");
	menu_auxvoice();
	reset_fakes("");
	menu_txtone(1);
	reset_fakes("");
	response_result = 1;
	menu_txtone(0);
	reset_fakes("");
	menu_txtone(0);
	reset_fakes("\n");
	menu_txtone(1);
	reset_fakes("\n");
	menu_txtone(0);
	reset_fakes("bad\n");
	menu_txtone(0);
	reset_fakes("500\n");
	menu_txtone(0);

	reset_fakes("");
	assert(menu_select_value("test", mixer_type, 5, 0) == 0);
	reset_fakes("");
	assert(menu_get_delay("delay", "q", 5) == 5);
	reset_fakes("\n");
	assert(menu_get_delay("delay", "q", 5) == 5);
	reset_fakes("bad\n");
	assert(menu_get_delay("delay", "q", 5) == 5);
	reset_fakes("123\n");
	assert(menu_get_delay("delay", "q", 5) == 123);
	reset_fakes("");
	response_result = 1;
	assert(menu_get_delay("delay", "q", 5) == 5);
	reset_fakes("");
	assert(menu_get_integer("test", 4, 1, 9) == 4);

	reset_fakes("");
	response_result = 1;
	menu_rxvoice();
	reset_fakes("500\n");
	response_failure_command = "menu-support c";
	menu_rxvoice();
	reset_fakes("500\n");
	response_failure_command = "menu-support c500";
	menu_rxvoice();
	reset_fakes("500");
	menu_rxvoice();
	reset_fakes("500");
	menu_rxsquelch();
	reset_fakes("500");
	menu_txvoice(1);
	reset_fakes("500");
	menu_auxvoice();
	reset_fakes("500");
	menu_txtone(1);
}

static void test_launcher_statuses(void)
{
	reset_fakes("");
	command_result = -1;
	errno = EIO;
	launch_processing_tune();
	command_result = 1 << 8;
	launch_processing_tune();
	command_result = SIGTERM;
	launch_processing_tune();
	ourhandler(SIGCHLD);
}

static void test_real_asterisk_command_transport(void)
{
	char output[128];
	int input[2];

	asterisk_binary = "/bin/echo";
	assert(astgetline_real("test command", output, sizeof(output) - 1) == 0);
	assert(strcmp(output, "-rx test command") == 0);
	assert(astgetline_real("test command", NULL, 0) == 0);
	assert(astgetline_real("test command", output, 0) == 0);
	assert(pipe(input) == 0);
	assert(dup2(input[0], STDIN_FILENO) == STDIN_FILENO);
	assert(close(input[0]) == 0);
	clearerr(stdin);
	assert(astgetresp_real("test response") == 0);
	assert(close(input[1]) == 0);
	asterisk_binary = "/usr/sbin/asterisk";
}

static void test_response_wait_paths(void)
{
	int input[2];

	asterisk_binary = "/bin/echo";
	set_wait_script(-1, EIO);
	assert(astgetresp_real("error") == -1);
	waitfds = waitfds_real;
	ourhandler(SIGCHLD);

	wait_results[0] = -1;
	wait_errnos[0] = EINTR;
	wait_results[1] = -1;
	wait_errnos[1] = EIO;
	wait_result_count = 2;
	wait_result_index = 0;
	waitfds = scripted_waitfds;
	assert(astgetresp_real("interrupt") == -1);
	waitfds = waitfds_real;
	ourhandler(SIGCHLD);

	wait_results[0] = 0;
	wait_errnos[0] = 0;
	wait_results[1] = -1;
	wait_errnos[1] = EIO;
	wait_result_count = 2;
	wait_result_index = 0;
	waitfds = scripted_waitfds;
	assert(astgetresp_real("timeout") == -1);
	waitfds = waitfds_real;
	ourhandler(SIGCHLD);

	assert(pipe(input) == 0);
	assert(write(input[1], "\n", 1) == 1);
	assert(dup2(input[0], STDIN_FILENO) == STDIN_FILENO);
	assert(close(input[0]) == 0);
	clearerr(stdin);
	set_wait_script(STDIN_FILENO + 1, 0);
	assert(astgetresp_real("console") == 0);
	waitfds = waitfds_real;
	assert(close(input[1]) == 0);
	ourhandler(SIGCHLD);
	asterisk_binary = "/usr/sbin/asterisk";
}

static void restore_transport(void)
{
	make_pipe = pipe;
	set_nonblocking = set_nonblocking_real;
	open_null_device = open_null_real;
	make_child = fork;
	copy_fd = dup2;
}

static void test_transport_failures(void)
{
	char output[8];
	int fd;

	make_pipe = fail_pipe;
	assert(doastcmd("test") == -1);
	assert(astgetline_real("test", output, sizeof(output) - 1) == -1);
	assert(astgetresp_real("test") == -1);
	restore_transport();

	set_nonblocking = fail_nonblocking;
	assert(doastcmd("test") == -1);
	restore_transport();
	open_null_device = fail_open_null;
	assert(doastcmd("test") == -1);
	restore_transport();
	make_child = fail_fork;
	assert(doastcmd("test") == -1);
	restore_transport();

	for (copy_failure_call = 1; copy_failure_call <= 3; copy_failure_call++) {
		copy_calls = 0;
		copy_fd = selective_copy_fd;
		fd = doastcmd("test");
		assert(fd >= 0);
		assert(getstrfd(fd, output, sizeof(output) - 1) == 0);
		assert(close(fd) == 0);
		ourhandler(SIGCHLD);
	}
	restore_transport();
	asterisk_binary = "/does/not/exist";
	fd = doastcmd("test");
	assert(fd >= 0);
	assert(getstrfd(fd, output, sizeof(output) - 1) == 0);
	assert(close(fd) == 0);
	ourhandler(SIGCHLD);
	asterisk_binary = "/usr/sbin/asterisk";
}

typedef void (*child_test)(void);

static void expect_child_exit(child_test test, int expected)
{
	void (*previous_handler)(int);
	int status;
	pid_t child;

	/* The utility's asynchronous reaper must not collect this test-owned child
	 * before the synchronous wait below. */
	previous_handler = signal(SIGCHLD, SIG_DFL);
	assert(previous_handler != SIG_ERR);
	child = fork();

	assert(child >= 0);
	if (!child) {
		test();
		exit(EXIT_SUCCESS);
	}
	assert(waitpid(child, &status, 0) == child);
	assert(signal(SIGCHLD, previous_handler) != SIG_ERR);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == expected);
}

static void child_bad_meter(void)
{
	char *args[] = {"tune", "--meter", "invalid", NULL};
	(void)run_tuner("", 3, args);
}

static void child_bad_option(void)
{
	char *args[] = {"tune", "--bad-option", NULL};
	(void)run_tuner("", 2, args);
}

static void child_device_transport_failure(void)
{
	char *args[] = {"tune", "-n", "test", NULL};
	reset_fakes("");
	line_result = -1;
	(void)usbradioplus_tune_main(3, args);
}

static void child_device_rejected(void)
{
	char *args[] = {"tune", "-n", "test", NULL};
	reset_fakes("");
	active_override = "No such device";
	(void)usbradioplus_tune_main(3, args);
}

static void child_setup_failure(void)
{
	char *args[] = {"tune", NULL};
	reset_fakes("");
	line_result = -1;
	(void)usbradioplus_tune_main(1, args);
}

static void child_bad_setup(void)
{
	char *args[] = {"tune", NULL};
	reset_fakes("");
	setup_override = "invalid";
	(void)usbradioplus_tune_main(1, args);
}

static void child_select_device_failure(void)
{
	reset_fakes("");
	line_result = -1;
	menu_selectusb();
}

static void child_swap_device_failure(void)
{
	reset_fakes("");
	line_result = -1;
	menu_swapusb();
}

static const char *child_input;
static const char *child_failure_command;
static const char *child_setup_value;
static const char *child_options_value;

static void child_main_response_failure(void)
{
	char *args[] = {"tune", NULL};
	reset_fakes(child_input);
	setup_override = child_setup_value;
	response_failure_command = child_failure_command;
	(void)usbradioplus_tune_main(1, args);
}

static void child_options_response_failure(void)
{
	reset_fakes(child_input);
	options_override = child_options_value;
	response_failure_command = child_failure_command;
	options_menu();
}

static void expect_main_response_failure(const char *input, const char *command, const char *setup)
{
	child_input = input;
	child_failure_command = command;
	child_setup_value = setup;
	expect_child_exit(child_main_response_failure, 255);
}

static void expect_options_response_failure(const char *input, const char *command,
					    const char *options)
{
	child_input = input;
	child_failure_command = command;
	child_options_value = options;
	expect_child_exit(child_options_response_failure, 255);
}

static void test_terminating_errors(void)
{
	expect_child_exit(child_bad_meter, EXIT_FAILURE);
	expect_child_exit(child_bad_option, EXIT_FAILURE);
	expect_child_exit(child_device_transport_failure, EXIT_FAILURE);
	expect_child_exit(child_device_rejected, EXIT_FAILURE);
	expect_child_exit(child_setup_failure, 255);
	expect_child_exit(child_bad_setup, 255);
	expect_child_exit(child_select_device_failure, 255);
	expect_child_exit(child_swap_device_failure, 255);
	expect_main_response_failure("2\n", "menu-support a", NULL);
	expect_main_response_failure("4\n", "menu-support d", NULL);
	expect_main_response_failure("9\n", "menu-support i", NULL);
	expect_main_response_failure("E\n", "menu-support k1", NULL);
	expect_main_response_failure(
		"E\n", "menu-support k0",
		"1,1,1,0,0,1,1,10,20,1,0,2,1,4,500,1.0,600,700,800,900,100,100,100");
	expect_main_response_failure("F\n", "menu-support l", NULL);
	expect_main_response_failure("P\n", "menu-support 2", NULL);
	expect_main_response_failure("W\n", "menu-support j", NULL);
	expect_options_response_failure("4\n", "menu-support s0", NULL);
	expect_options_response_failure("4\n", "menu-support s1", "1,1,0,0,0,1,1,10,20,0,0,2,1,4");
	expect_options_response_failure("5\n", "menu-support t1", NULL);
	expect_options_response_failure("5\n", "menu-support t0", "1,1,0,0,0,1,1,10,20,1,1,2,1,4");
}

static void test_meter_and_device_modes(void)
{
	char *device_args[] = {"tune", "-n", "test", "--meter", "all", NULL};
	char *rx_args[] = {"tune", "--meter", "rx", NULL};
	char *tx_args[] = {"tune", "--meter", "tx", NULL};
	char *status_args[] = {"tune", "--meter", "status", NULL};

	assert(run_tuner("", 5, device_args) == EXIT_SUCCESS);
	assert(run_tuner("", 3, rx_args) == EXIT_SUCCESS);
	assert(run_tuner("", 3, tx_args) == EXIT_SUCCESS);
	assert(run_tuner("", 3, status_args) == EXIT_SUCCESS);
}

static void test_full_menu(void)
{
	char *args[] = {"tune", NULL};
	const char *input = "1\n1\n2\n3\n500\n\n4\n5\n500\nT\n6\n500\n7\n500\n8\n500\n9\nE\nF\n"
			    "G\n2\nH\n2\nP\nO\n1\n2\n2\n123\n3\n321\n4\n5\n6\n2\n7\n3\nL\n"
			    "11000\nD\n700\nM\n1\n0\nC\nR\nS\n1\nV\nW\nX\n?\n0\n";

	flat_configuration = 1;
	command_result = 0;
	assert(run_tuner(input, 1, args) == EXIT_SUCCESS);
	assert(response_calls > 20);
}

static void test_inapplicable_menu_items(void)
{
	char *args[] = {"tune", NULL};

	flat_configuration = 0;
	assert(run_tuner("2\n4\n5\n8\n9\n0\n", 1, args) == EXIT_SUCCESS);
	assert(run_tuner("", 1, args) == EXIT_SUCCESS);
	assert(run_tuner("long\n0\n", 1, args) == EXIT_SUCCESS);
	reset_fakes("");
	response_failure_command = "active";
	optind = 1;
	assert(usbradioplus_tune_main(1, args) == EXIT_SUCCESS);
}

static void test_options_menu_edges(void)
{
	reset_fakes("");
	options_menu();
	reset_fakes("long\n0\n");
	options_menu();
	reset_fakes("?\n0\n");
	options_menu();
	reset_fakes("");
	line_result = 1;
	options_menu();
	reset_fakes("");
	options_override = "invalid";
	options_menu();
	reset_fakes("1\n\n6\n\n7\n\nL\n\nD\n\nM\n\n0\n");
	options_menu();
	reset_fakes("0\n");
	line_failure_command = "menu-support L";
	options_menu();
	reset_fakes("0\n");
	line_failure_command = "menu-support D";
	options_menu();
	reset_fakes("0\n");
	line_failure_command = "menu-support M";
	options_menu();
	reset_fakes("0\n");
	options_override = "1,1,0,0,0,1,1,10,20,1,0,2,1,4";
	line_failure_command = "never";
	options_menu();
	reset_fakes("4\n5\n0\n");
	options_override = "1,1,0,0,0,1,1,10,20,0,1,2,1,4";
	options_menu();
	reset_fakes("0\n");
	duplex_mode_response = "invalid";
	options_menu();
}

static void test_alternate_output_assignments(void)
{
	char *args[] = {"tune", NULL};

	flat_configuration = 1;
	reset_fakes("0\n");
	setup_override = "1,1,0,0,0,1,1,10,20,1,0,2,0,1,500,1.0,600,700,800,900,100,100,100";
	optind = 1;
	assert(usbradioplus_tune_main(1, args) == EXIT_SUCCESS);
	reset_fakes("0\n");
	setup_override = "1,1,0,0,0,1,1,10,20,1,0,2,4,0,500,1.0,600,700,800,900,100,100,100";
	optind = 1;
	assert(usbradioplus_tune_main(1, args) == EXIT_SUCCESS);
	reset_fakes("0\n");
	setup_override = "1,1,0,0,0,1,1,10,20,1,0,2,0,4,500,1.0,600,700,800,900,100,100,100";
	optind = 1;
	assert(usbradioplus_tune_main(1, args) == EXIT_SUCCESS);
	reset_fakes("0\n");
	setup_override = "1,1,0,0,0,1,1,10,20,1,0,2,3,0,500,1.0,600,700,800,900,100,100,100";
	optind = 1;
	assert(usbradioplus_tune_main(1, args) == EXIT_SUCCESS);
	reset_fakes("0\n");
	setup_override = "1,1,0,0,0,1,1,10,20,1,0,2,0,3,500,1.0,600,700,800,900,100,100,100";
	optind = 1;
	assert(usbradioplus_tune_main(1, args) == EXIT_SUCCESS);
}

static void test_empty_device_and_cancelled_selections(void)
{
	char *device_args[] = {"tune", "-n", "", NULL};
	char *args[] = {"tune", NULL};

	assert(run_tuner("0\n", 3, device_args) == EXIT_SUCCESS);
	assert(run_tuner("G\n\nH\n\n0\n", 1, args) == EXIT_SUCCESS);
	assert(run_tuner("T\nT\n0\n", 1, args) == EXIT_SUCCESS);
	reset_fakes("E\n0\n");
	setup_override = "1,1,1,0,0,1,1,10,20,1,0,2,1,4,500,1.0,600,700,800,900,100,100,100";
	optind = 1;
	assert(usbradioplus_tune_main(1, args) == EXIT_SUCCESS);
}

int main(void)
{
	test_explode_string();
	test_integer_parser();
	test_file_descriptor_helpers();
	test_interrupted_io();
	test_value_prompts();
	test_menu_input_edges();
	test_launcher_statuses();
	test_real_asterisk_command_transport();
	test_response_wait_paths();
	test_transport_failures();
	test_terminating_errors();
	test_meter_and_device_modes();
	test_full_menu();
	test_inapplicable_menu_items();
	test_options_menu_edges();
	test_alternate_output_assignments();
	test_empty_device_and_cancelled_selections();
	puts("tune core tests passed");
	return 0;
}

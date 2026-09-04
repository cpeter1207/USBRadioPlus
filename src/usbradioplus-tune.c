/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2010, Jim Dixon, WB6NIL
 *
 * Jim Dixon <jim@lambdatel.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*
 *
 * USBRadioPlus tune menu program
 *
 * This program communicates with Asterisk by sending commands to retrieve and set values
 * for the usbradio channel driver.
 *
 * The following 'menu-support' commands are used:
 *
 * susb tune menusupport X - where X is one of the following:
 *		0 - get current settings
 *		1 - get node names that are configured in usbradioplus.conf
 *		2 - print parameters
 *		3 - get node names that are configured in usbradioplus.conf, except current device
 *		a - receive rx level
 *		b - receiver tune display
 *		c - receive level
 *		d - receive ctcss level
 *		e - squelch level
 *		f - voice level
 *		g - aux level
 *		h - transmit a test tone
 *		i - tune receive level
 *		j - save current settings for the selected node
 *		k - change echo mode
 *		l - generate test tone
 *		o - change carrier from
 *		p - change ctcss from
 *		q - change rx on delay
 *		r - change tx off delay
 *		s - change tx pre limiting
 *		t - change tx limiting only
 *		u - change rx demodulation
 *		v - view cos, ctcss and ptt status
 *		w - change tx mixer a
 *		x - change tx mixer b
 *		y - receive audio statistics display
 *		D - change duplex 3 repeat level
 *		M - change duplex 3 implementation
 *
 * Most of these commands take optional parameters to set values.
 *
 */
#include "asterisk.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <getopt.h>

#include "asterisk/utils.h"
#include "usbradioplus_tune_internal.h"

#ifdef URP_TUNE_TESTING
#define TUNE_PRIVATE
#else
#define TUNE_PRIVATE static
#endif

/*! \brief type of signal detection used for carrier (cd) or ctcss (sd) */
static const char *const cd_signal_type[] = {"no",	  "dsp", "vox",	    "usb",
					     "usbinvert", "pp",	 "ppinvert"};
static const char *const sd_signal_type[] = {"no", "usb", "usbinvert", "dsp", "pp", "ppinvert"};

/*! \brief demodulation type */
static const char *const demodulation_type[] = {"no", "speaker", "flat"};

/*! \brief mixer type */
TUNE_PRIVATE const char *const mixer_type[] = {"no", "voice", "tone", "composite", "auxvoice"};
static const char *const duplex3_mode_type[] = {"hardware", "software"};

TUNE_PRIVATE int astgetline_real(char *cmd, char *str, int max);
TUNE_PRIVATE int astgetresp_real(char *cmd);
TUNE_PRIVATE asterisk_line_reader astgetline = astgetline_real;
TUNE_PRIVATE asterisk_response_reader astgetresp = astgetresp_real;
TUNE_PRIVATE command_runner run_command = system;
TUNE_PRIVATE const char *asterisk_binary = "/usr/sbin/asterisk";

TUNE_PRIVATE int set_nonblocking_real(int fd)
{
	return fcntl(fd, F_SETFL, O_NONBLOCK);
}

TUNE_PRIVATE int open_null_real(void)
{
	return open("/dev/null", O_RDWR);
}

TUNE_PRIVATE int (*make_pipe)(int pipefd[2]) = pipe;
TUNE_PRIVATE int (*set_nonblocking)(int fd) = set_nonblocking_real;
TUNE_PRIVATE int (*open_null_device)(void) = open_null_real;
TUNE_PRIVATE pid_t (*make_child)(void) = fork;
TUNE_PRIVATE int (*copy_fd)(int oldfd, int newfd) = dup2;
TUNE_PRIVATE int waitfds_real(int fd1, int fd2, int ms);
TUNE_PRIVATE int (*waitfds)(int fd1, int fd2, int ms) = waitfds_real;
TUNE_PRIVATE ssize_t (*read_bytes)(int fd, void *buffer, size_t count) = read;

/*! \brief command prefix for Asterisk - simpleusb channel driver access */
#define COMMAND_PREFIX "radioplus "

/*!
 * \brief Signal handler
 * \param sig		Signal to watch.
 */
TUNE_PRIVATE void ourhandler(int sig)
{
	int i;

	signal(sig, ourhandler);
	while (waitpid(-1, &i, WNOHANG) > 0) {
		;
	}
}

TUNE_PRIVATE void launch_processing_tune(void)
{
	int status;

	/* system() waits for this child, so temporarily stop the general child
	 * reaper from collecting it first. */
	signal(SIGCHLD, SIG_DFL);
	status = run_command("/usr/sbin/usbradioplus-processing-tune");
	signal(SIGCHLD, ourhandler);
	if (status == -1) {
		fprintf(stderr, "Unable to start usbradioplus-processing-tune: %s\n",
			strerror(errno));
	} else if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr,
			"usbradioplus-processing-tune exited without completing normally\n");
	}
}

/*!
 * \brief Compare for qsort - sorts strings.
 * \param a			Pointer to first string.
 * \param b			Pointer to second string.
 * \retval 0		If equal.
 * \retval -1		If less than.
 * \retval 1		If greater than.
 */
TUNE_PRIVATE int qcompar(const void *a, const void *b)
{
	char **sa = (char **)a, **sb = (char **)b;
	return (strcmp(*sa, *sb));
}

/*!
 * \brief Break up a delimited string into a table of substrings
 *
 * \note This modifies the string str, be sure to save an intact copy if you need it later.
 *
 * \param str		Pointer to string to process (it will be modified).
 * \param strp		Pointer to a list of substrings created, NULL will be placed at the end of
 * the list. \param limit		Maximum number of substrings to process. \param delim
 * Specified delimiter
 * \param quote		User specified quote for escaping a substring. Set to zero to escape
 * nothing.
 *
 * \retval 			Returns number of substrings found.
 */
TUNE_PRIVATE int explode_string(char *str, char *strp[], size_t limit, char delim, char quote)
{
	int i, inquo;

	if (!limit) {
		return 0;
	}
	inquo = 0;
	i = 0;
	strp[i++] = str;

	if (!*str) {
		strp[0] = 0;
		return 0;
	}
	for (; *str && ((size_t)i + 1 < limit); str++) {
		if (quote) {
			if (*str == quote) {
				if (inquo) {
					*str = 0;
					inquo = 0;
				} else {
					strp[i - 1] = str + 1;
					inquo = 1;
				}
			}
		}
		if ((*str == delim) && (!inquo)) {
			*str = 0;
			strp[i++] = str + 1;
		}
	}
	strp[i] = 0;
	return i;
}

TUNE_PRIVATE int parse_integer(const char *text, int minimum, int maximum, int *value)
{
	char *end;
	long parsed;

	if (!text || !*text)
		return -1;
	errno = 0;
	parsed = strtol(text, &end, 10);
	if (errno || *end || parsed < minimum || parsed > maximum)
		return -1;
	*value = (int)parsed;
	return 0;
}

TUNE_PRIVATE void strip_newline(char *text)
{
	size_t length = strlen(text);

	if (length && text[length - 1] == '\n')
		text[length - 1] = '\0';
}

/*!
 * \brief Execute an asterisk command.
 *
 * Opens a pipe and executes 'asterisk -rx cmd'
 *
 * \param cmd		Pointer to command to execute.
 * \returns			Pipe FD or -1 on failure.
 */
TUNE_PRIVATE int doastcmd(char *cmd)
{
	int pfd[2], pid, nullfd;

	if (make_pipe(pfd) == -1) {
		perror("Error: cannot open pipe");
		return -1;
	}
	if (set_nonblocking(pfd[0]) == -1) {
		perror("Error: cannot set pipe to NONBLOCK");
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}

	nullfd = open_null_device();
	if (nullfd == -1) {
		perror("Error: cannot open /dev/null");
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}

	pid = make_child();
	if (pid == -1) {
		perror("Error: cannot fork");
		close(nullfd);
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid) { /* if this is us (the parent) */
		close(nullfd);
		close(pfd[1]);
		return pfd[0];
	}
	close(pfd[0]);

	if (copy_fd(nullfd, fileno(stdin)) == -1) {
		perror("Error: cannot dup2() stdin");
		exit(0);
	}
	if (copy_fd(pfd[1], fileno(stdout)) == -1) {
		perror("Error: cannot dup2() stdout");
		exit(0);
	}
	if (copy_fd(pfd[1], fileno(stderr)) == -1) {
		perror("Error: cannot dup2() stderr");
		exit(0);
	}
	/* Execute the asterisk command */
	close(nullfd);
	close(pfd[1]);
	execl(asterisk_binary, "asterisk", "-rx", cmd, NULL);

	exit(0);
}

/*!
 * \brief Wait on one or two fd's.
 *
 * Check to see if fd1 or fd2, if specified, is ready to read.
 * returns -1 if error, 0 if nothing ready, or ready fd + 1
 *
 * awkward, but needed to support having an fd of 0, which
 * is likely, since that's most likely stdin
 *
 * specify fd2 as -1 if not used
 *
 * \param fd1		First fd to poll.
 * \param fd2		Second fd to poll.  Specify -1 if not used.
 * \param ms		Milliseconds to wait.
 * \returns			-1 on error, 0 if nothing ready, or fd+1.
 */
TUNE_PRIVATE int waitfds_real(int fd1, int fd2, int ms)
{
	fd_set fds;
	struct timeval tv;
	int i, r;

	FD_ZERO(&fds);
	FD_SET(fd1, &fds);

	if (fd2 >= 0) {
		FD_SET(fd2, &fds);
	}
	tv = ast_tv(0, ms * 1000);

	i = fd1;
	if (fd2 > fd1) {
		i = fd2;
	}

	r = select(i + 1, &fds, NULL, NULL, &tv);
	if (r < 1) {
		return r;
	}
	if (FD_ISSET(fd1, &fds)) {
		return (fd1 + 1);
	}
	/* select() reported one of the two requested descriptors, and fd1 was not it. */
	return fd2 + 1;
}

/*!
 * \brief Wait for a character.
 *
 * \param fd		fd to read.
 * \returns			-1 nothing read, or character read.
 */
TUNE_PRIVATE int getcharfd(int fd)
{
	char c;

	if (read_bytes(fd, &c, 1) != 1) {
		return -1;
	}
	return c;
}

/*!
 * \brief Wait for string of characters.
 *
 * \param fd		fd to read.
 * \param str		Pointer to string buffer.
 * \param max		Maximum number of characters to read.
 * \returns			Number of characters read.
 */
TUNE_PRIVATE int getstrfd(int fd, char *str, int max)
{
	int i, j;
	char c;

	for (i = 0; (i < max) || (!max); i++) {
		do {
			j = waitfds(fd, -1, 100);
			if (j == -1) {
				if (errno != EINTR) {
					break;
				}
				j = 0;
			}
		} while (!j);
		if (j == -1) {
			break;
		}

		do {
			j = (int)read_bytes(fd, &c, 1);
		} while (j == -1 && errno == EINTR);
		if (j == 0) {
			break;
		}
		if (j == -1) {
			break;
		}
		if (c == '\n') {
			break;
		}
		if (str) {
			str[i] = c;
		}
	}
	if (str) {
		str[i] = 0;
	}
	return i;
}

/*!
 * \brief Get one line of data from Asterisk.
 *
 *	Send a command to asterisk and get the response.
 *
 * \param cmd		Pointer to command to send to asterisk.
 * \param str		Pointer to string buffer.
 * \param max		Size of string buffer.
 * \returns			-1 on error, 0 if successful, 1 if nothing was returned.
 */
TUNE_PRIVATE int astgetline_real(char *cmd, char *str, int max)
{
	int fd, rv;

	if (str && max > 0) {
		str[0] = '\0';
	}

	/* Send the command to Asterisk */
	fd = doastcmd(cmd);
	if (fd == -1) {
		perror("Error getting data from Asterisk");
		return -1;
	}

	rv = getstrfd(fd, str, max);
	close(fd);

	return rv > 0 ? 0 : 1;
}

/*!
 * \brief Get a response from Asterisk and send to stdout.
 *
 *	Send a command to asterisk and output the response.
 *
 * \param cmd		Pointer to command to send to asterisk.
 * \returns			-1 on error, 0 if successful.
 */
TUNE_PRIVATE int astgetresp_real(char *cmd)
{
	int i, fd;
	char str[256];

	/* Send the command to Asterisk */
	fd = doastcmd(cmd);
	if (fd == -1) {
		perror("Error getting response from Asterisk");
		return -1;
	}

	/* Wait and process the response */
	for (;;) {
		int w;

		w = waitfds(fileno(stdin), fd, 100);
		if (w == -1) {
			if (errno == EINTR) {
				continue;
			}
			perror("Error processing response from Asterisk");
			close(fd);
			return -1;
		}
		if (!w) {
			continue;
		}

		/* if it's our console */
		if (w == (fileno(stdin) + 1)) {
			getstrfd(fileno(stdin), str, sizeof(str) - 1);
			break;
		}

		/* waitfds() can only identify stdin or the Asterisk descriptor here. */
		i = getcharfd(fd);
		if (i == -1) {
			break;
		}
		putchar(i);
		fflush(stdout);
	}
	close(fd);
	return 0;
}

/*!
 * \brief Menu option to select the usb device.
 */
TUNE_PRIVATE void menu_selectusb(void)
{
	int i, n, x;
	char str[100], buf[256], *strs[100];

	printf("\n");

	/* print selected USB device */
	if (astgetresp(COMMAND_PREFIX "active")) {
		return;
	}

	/* get device list from Asterisk */
	if (astgetline(COMMAND_PREFIX "tune menu-support 1", buf, sizeof(buf) - 1) < 0) {
		exit(255);
	}
	n = explode_string(buf, strs, ARRAY_LEN(strs), ',', 0);
	if (n < 1) {
		fprintf(stderr, "No USB devices found\n");
		return;
	}
	qsort((void *)strs, n, sizeof(char *), qcompar);

	printf("Please select from the following USB devices:\n");
	for (x = 0; x < n; x++) {
		printf("%d) Device [%s]\n", x + 1, strs[x]);
	}

	printf("0) Back\n");
	printf("Enter your selection: ");
	if (fgets(str, sizeof(str) - 1, stdin) == NULL) {
		printf("USB device not changed\n");
		return;
	}
	strip_newline(str);
	if (parse_integer(str, 0, n, &i)) {
		printf("Entry Error, USB device not changed\n");
		return;
	}
	if (i < 1) {
		printf("USB device not changed\n");
		return;
	}
	snprintf(str, sizeof(str), COMMAND_PREFIX "active %s", strs[i - 1]);
	astgetresp(str);
}

/*!
 * \brief Menu option to swap the usb device.
 */
TUNE_PRIVATE void menu_swapusb(void)
{
	int i, n, x;
	char str[100], buf[256], *strs[100];

	printf("\n");

	/* print selected USB device */
	if (astgetresp(COMMAND_PREFIX "active")) {
		return;
	}

	/* get device list from Asterisk */
	if (astgetline(COMMAND_PREFIX "tune menu-support 3", buf, sizeof(buf) - 1) < 0) {
		exit(255);
	}
	n = explode_string(buf, strs, ARRAY_LEN(strs), ',', 0);
	if (n < 1) {
		fprintf(stderr, "No additional USB devices found\n");
		return;
	}
	qsort((void *)strs, n, sizeof(char *), qcompar);

	printf("Please select from the following USB devices:\n");
	for (x = 0; x < n; x++) {
		printf("%d) Device [%s]\n", x + 1, strs[x]);
	}
	printf("0) Exit Selection\n");
	printf("Enter make your selection now: ");
	if (fgets(str, sizeof(str) - 1, stdin) == NULL) {
		printf("USB device not changed\n");
		return;
	}
	strip_newline(str);
	if (parse_integer(str, 0, n, &i)) {
		printf("Entry Error, USB device not swapped\n");
		return;
	}
	if (i < 1) {
		printf("USB device not swapped\n");
		return;
	}
	snprintf(str, sizeof(str), COMMAND_PREFIX "tune swap %s", strs[i - 1]);
	astgetresp(str);
}

/*!
 * \brief Menu option to set rxvoice level.
 */
TUNE_PRIVATE void menu_rxvoice(void)
{
	int i;
	char str[100];

	for (;;) {
		printf("Live RX level display. Press Enter to continue to the setting prompt.\n");
		if (astgetresp(COMMAND_PREFIX "tune menu-support b")) {
			break;
		}
		if (astgetresp(COMMAND_PREFIX "tune menu-support c")) {
			break;
		}

		printf("Enter a new RX voice level (0-999), or press Enter to keep the current "
		       "value: ");
		if (fgets(str, sizeof(str) - 1, stdin) == NULL) {
			printf("Rx voice setting not changed\n");
			return;
		}
		strip_newline(str);
		if (!str[0]) {
			printf("Rx voice setting not changed\n");
			return;
		}
		if (parse_integer(str, 0, 999, &i)) {
			printf("Entry Error, Rx voice setting not changed\n");
			continue;
		}
		snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support c%d", i);
		if (astgetresp(str)) {
			break;
		}
	}
}

/*!
 * \brief Menu option to set rxsquelch level.
 */
TUNE_PRIVATE void menu_rxsquelch(void)
{
	char str[100];
	int i;

	if (astgetresp(COMMAND_PREFIX "tune menu-support e")) {
		return;
	}

	printf("Enter a new RX squelch level (0-999), or press Enter to keep the current value: ");
	if (fgets(str, sizeof(str) - 1, stdin) == NULL) {
		printf("Rx Squelch Level setting not changed\n");
		return;
	}
	strip_newline(str);
	if (!str[0]) {
		printf("Rx Squelch Level setting not changed\n");
		return;
	}
	if (parse_integer(str, 0, 999, &i)) {
		printf("Entry Error, Rx Squelch Level setting not changed\n");
		return;
	}
	snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support e%d", i);
	astgetresp(str);
}

/*!
 * \brief Menu option to set txvoice level.
 * \param keying	Boolean to indicate if we are currently keying.
 */
TUNE_PRIVATE void menu_txvoice(int keying)
{
	char str[100];
	int i;

	if (astgetresp(COMMAND_PREFIX "tune menu-support f")) {
		return;
	}

	printf("Enter a new TX voice level (0-999), or press Enter to keep the current value: ");
	if (fgets(str, sizeof(str) - 1, stdin) == NULL) {
		printf("Tx Voice Level setting not changed\n");
		if (keying) {
			astgetresp(COMMAND_PREFIX "tune menu-support fK");
		}
		return;
	}
	strip_newline(str);
	if (!str[0]) {
		printf("Tx Voice Level setting not changed\n");
		if (keying) {
			astgetresp(COMMAND_PREFIX "tune menu-support fK");
		}
		return;
	}
	if (parse_integer(str, 0, 999, &i)) {
		printf("Entry Error, Tx Voice Level setting not changed\n");
		return;
	}
	if (keying) {
		snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support fK%d", i);
	} else {
		snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support f%d", i);
	}
	astgetresp(str);
}

/*!
 * \brief Menu option to set auxvoice level.
 */
TUNE_PRIVATE void menu_auxvoice(void)
{
	char str[100];
	int i;

	if (astgetresp(COMMAND_PREFIX "tune menu-support g")) {
		return;
	}

	printf("Enter a new auxiliary voice level (0-999), or press Enter to keep the current "
	       "value: ");
	if (fgets(str, sizeof(str) - 1, stdin) == NULL) {
		printf("Entry Error, Aux Voice Level setting not changed\n");
		return;
	}
	strip_newline(str);
	if (!str[0]) {
		printf("Entry Error, Aux Voice Level setting not changed\n");
		return;
	}
	if (parse_integer(str, 0, 999, &i)) {
		printf("Entry Error, Aux Voice Level setting not changed\n");
		return;
	}
	snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support g%d", i);
	astgetresp(str);
}

/*!
 * \brief Menu option to set txtone level.
 * \param keying	Boolean to indicate if we are currently keying.
 */

TUNE_PRIVATE void menu_txtone(int keying)
{
	char str[100];
	int i;

	if (astgetresp(COMMAND_PREFIX "tune menu-support h")) {
		return;
	}

	printf("Enter a new TX CTCSS level (0-999), or press Enter to keep the current value: ");
	if (fgets(str, sizeof(str) - 1, stdin) == NULL) {
		printf("Tx CTCSS Modulation Level setting not changed\n");
		if (keying) {
			astgetresp(COMMAND_PREFIX "tune menu-support hK");
		}
		return;
	}
	strip_newline(str);
	if (!str[0]) {
		printf("Tx CTCSS Modulation Level setting not changed\n");
		if (keying) {
			astgetresp(COMMAND_PREFIX "tune menu-support hK");
		}
		return;
	}
	if (parse_integer(str, 0, 999, &i)) {
		printf("Entry Error, Tx CTCSS Modulation Level setting not changed\n");
		return;
	}
	if (keying) {
		snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support hK%d", i);
	} else {
		snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support h%d", i);
	}
	astgetresp(str);
}

/*!
 * \brief Menu option view cos, ctcss and ptt status.
 */
TUNE_PRIVATE void menu_view_status(void)
{
	printf("Live COS, CTCSS, and PTT status. Press Enter to return.\n");
	astgetresp(COMMAND_PREFIX "tune menu-support v");
}

/*!
 * \brief Menu option to select list value.
 * \param value_name	Pointer to description of item being changed.
 * \param items			Pointer to array of options.
 * \param max_items		Number of items in the items array.
 * \param selection		Current selected item.
 */
TUNE_PRIVATE int menu_select_value(const char *value_name, const char *const *items, int max_items,
				   int selection)
{
	char str[100];
	int i;

	printf("\nPlease select from the following methods for %s:\n", value_name);

	for (i = 0; i < max_items; i++) {
		printf("%d) %s %s\n", i + 1, items[i], selection == i ? "- Current" : "");
	}

	printf("Select a new %s, or press Enter to keep the current value: ", value_name);
	if (fgets(str, sizeof(str) - 1, stdin) == NULL) {
		printf("Method not changed\n");
		return 0;
	}
	strip_newline(str);
	if (parse_integer(str, 1, max_items, &i)) {
		printf("Method not changed\n");
		return 0;
	}
	return i;
}

/*!
 * \brief Menu option to set delay value.
 * \param delay_type	Pointer to the description of the delay type.
 * \param menu_option	Pointer to the menusupport option to update.
 * \param delay			The current delay setting.
 */
TUNE_PRIVATE int menu_get_delay(const char *delay_type, const char *menu_option, int delay)
{
	char str[100];
	int value;

	snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support %s", menu_option);
	if (astgetresp(str)) {
		return delay;
	}

	printf("Enter a new %s setting (0-999), or press Enter to keep %d: ", delay_type, delay);
	if (fgets(str, sizeof(str) - 1, stdin) == NULL) {
		printf("Setting not changed\n");
		return delay;
	}
	strip_newline(str);
	if (!str[0]) {
		printf("Setting not changed\n");
		return delay;
	}
	if (parse_integer(str, 0, 999, &value)) {
		printf("Entry Error, setting not changed\n");
		return delay;
	}

	return value;
}

/*! \brief Prompt for an integer setting with an explicit range. */
TUNE_PRIVATE int menu_get_integer(const char *name, int current, int minimum, int maximum)
{
	char str[100];
	int value;

	printf("Enter a new %s (%d-%d), or press Enter to keep %d: ", name, minimum, maximum,
	       current);
	if (fgets(str, sizeof(str) - 1, stdin) == NULL) {
		printf("Setting not changed\n");
		return current;
	}
	strip_newline(str);
	if (!str[0]) {
		printf("Setting not changed\n");
		return current;
	}
	if (parse_integer(str, minimum, maximum, &value)) {
		printf("Entry error; setting not changed\n");
		return current;
	}
	return value;
}

/*!
 * \brief Options menu.
 */
TUNE_PRIVATE void options_menu(void)
{
	int flatrx = 0, txhasctcss = 0, echomode = 0;
	int reserved_field_1 = 0, reserved_field_2 = 0, carrierfrom = 0, ctcssfrom = 0;
	int rxondelay = 0, txoffdelay = 0, txprelim = 0, txlimonly = 0;
	int rxdemod = 0, txmixa = 0, txmixb = 0, txslimsp = 12000;
	int duplex3 = 0, duplex3mode = 0;
	int result;
	char str[256];

	for (;;) {
		/* get device parameters from Asterisk */
		if (astgetline(COMMAND_PREFIX "tune menu-support 0", str, sizeof(str) - 1)) {
			return;
		}
		if (sscanf(str, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &flatrx, &txhasctcss,
			   &echomode, &reserved_field_1, &reserved_field_2, &carrierfrom,
			   &ctcssfrom, &rxondelay, &txoffdelay, &txprelim, &txlimonly, &rxdemod,
			   &txmixa, &txmixb) != 14) {
			fprintf(stderr, "Error parsing device parameters: %s\n", str);
			return;
		}
		(void)reserved_field_1;
		(void)reserved_field_2;
		if (!astgetline(COMMAND_PREFIX "tune menu-support L", str, sizeof(str) - 1)) {
			sscanf(str, "TX soft limiting setpoint currently set to: %d", &txslimsp);
		}
		if (!astgetline(COMMAND_PREFIX "tune menu-support D", str, sizeof(str) - 1)) {
			sscanf(str, "Duplex 3 level currently set to: %d", &duplex3);
		}
		if (!astgetline(COMMAND_PREFIX "tune menu-support M", str, sizeof(str) - 1)) {
			char mode[16];
			if (sscanf(str, "Duplex 3 mode currently set to: %15s", mode) == 1) {
				duplex3mode = !strcmp(mode, "software");
			}
		}

		printf("\nOptions Menu\n");
		printf("1) Change RX Demodulation (currently '%s')\n", demodulation_type[rxdemod]);
		printf("2) Change RX On Delay (currently '%d')\n", rxondelay);
		printf("3) Change TX Off Delay (currently '%d')\n", txoffdelay);
		printf("4) Toggle TX Prelimiting (currently '%s')\n",
		       txprelim ? "enabled" : "disabled");
		printf("5) Toggle TX Limiting Only (currently '%s')\n",
		       txlimonly ? "enabled" : "disabled");
		printf("6) Change TX Mixer A (currently '%s')\n", mixer_type[txmixa]);
		printf("7) Change Tx Mixer B (currently '%s')\n", mixer_type[txmixb]);
		printf("L) Change TX Soft Limiter Setpoint (currently '%d')\n", txslimsp);
		printf("D) Change Duplex 3 Repeat Level (currently '%d')\n", duplex3);
		printf("M) Change Duplex 3 Mode (currently '%s')\n",
		       duplex3_mode_type[duplex3mode]);
		printf("0) Back\n");
		printf("\nPlease enter your selection now: ");

		if (fgets(str, sizeof(str) - 1, stdin) == NULL) {
			break;
		}
		if (strlen(str) != 2) { /* it's 2 because of \n at end */
			printf("Invalid Entry, try again\n");
			continue;
		}

		/* if to exit */
		if (str[0] == '0') {
			break;
		}

		switch (str[0]) {
		case '1': /* select rx demodulation */
			result =
				menu_select_value("RX Demodulation", demodulation_type, 3, rxdemod);
			if (result > 0) {
				snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support u%d",
					 result - 1);
				astgetresp(str);
			}
			break;
		case '2': /* set rx on delay */
			result = menu_get_delay("RX On Delay", "q", rxondelay);
			snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support q%d", result);
			astgetresp(str);
			break;
		case '3': /* set tx off delay */
			result = menu_get_delay("TX Off Delay", "r", txoffdelay);
			snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support r%d", result);
			astgetresp(str);
			break;
		case '4': /* toggle txprelim */
			if (txprelim) {
				if (astgetresp(COMMAND_PREFIX "tune menu-support s0")) {
					exit(255);
				}
			} else {
				if (astgetresp(COMMAND_PREFIX "tune menu-support s1")) {
					exit(255);
				}
			}
			break;
		case '5': /* toggle txlimonly */
			if (txlimonly) {
				if (astgetresp(COMMAND_PREFIX "tune menu-support t0")) {
					exit(255);
				}
			} else {
				if (astgetresp(COMMAND_PREFIX "tune menu-support t1")) {
					exit(255);
				}
			}
			break;
		case '6': /* select tx mixer a */
			result = menu_select_value("TX Mixer A", mixer_type, 5, txmixa);
			if (result > 0) {
				snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support w%d",
					 result - 1);
				astgetresp(str);
			}
			break;
		case '7': /* select tx mixer b */
			result = menu_select_value("TX Mixer B", mixer_type, 5, txmixb);
			if (result > 0) {
				snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support x%d",
					 result - 1);
				astgetresp(str);
			}
			break;
		case 'l': /* set tx soft limiter onset */
		case 'L':
			result =
				menu_get_integer("TX soft limiter setpoint", txslimsp, 5000, 13000);
			if (result != txslimsp) {
				snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support L%d",
					 result);
				astgetresp(str);
			}
			break;
		case 'd': /* set duplex 3 repeat level */
		case 'D':
			result = menu_get_integer("Duplex 3 repeat level", duplex3, 0, 999);
			if (result != duplex3) {
				snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support D%d",
					 result);
				astgetresp(str);
			}
			break;
		case 'm': /* select duplex 3 implementation */
		case 'M':
			result = menu_select_value("Duplex 3 Mode", duplex3_mode_type, 2,
						   duplex3mode);
			if (result > 0) {
				snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support M%d",
					 result - 1);
				astgetresp(str);
			}
			break;
		default:
			printf("Invalid Entry, try again\n");
			break;
		}
	}
}

/*!
 * \brief Main program entry point.
 */
int main(int argc, char *argv[])
{
	static const struct option long_options[] = {
		{"meter", required_argument, NULL, 'm'},
		{NULL, 0, NULL, 0},
	};
	int flatrx = 0, txhasctcss = 0, keying = 0, echomode = 0;
	int reserved_field_1 = 0, reserved_field_2 = 0, carrierfrom = 0, ctcssfrom = 0;
	int rxondelay = 0, txoffdelay = 0, txprelim = 0, txlimonly = 0;
	int rxdemod = 0, txmixa = 0, txmixb = 0;
	int rxmixerset = 0, rxsquelchadj = 0;
	float rxvoiceadj = 0;
	int txmixaset = 0, txmixbset = 0, txctcssadj = 0;
	int micplaymax = 0, spkrmax = 0, micmax = 0;
	char str[256];
	int result;
	int opt;
	const char *device = NULL;
	const char *meter = NULL;

	signal(SIGCHLD, ourhandler);

	while ((opt = getopt_long(argc, argv, "n:m:", long_options, NULL)) != -1) {
		switch (opt) {
		case 'n':
			device = optarg;
			break;
		case 'm':
			meter = optarg;
			break;
		default: /* '?' */
			fprintf(stderr, "Usage: %s [-n node#] [--meter rx|tx|status|all]\n",
				argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if ((device != NULL) && (strlen(device) > 0)) {
		snprintf(str, sizeof(str), COMMAND_PREFIX "active %s", device);
		if (astgetline(str, str, sizeof(str) - 1)) {
			printf("The chan_usbradioplus active device could not be set!\n\n");
			printf("Verify that Asterisk is running and chan_usbradioplus is "
			       "loaded.\n\n");
			exit(EXIT_FAILURE);
		}
		if (strstr(str, "Active (command) USB Radio device set to ") != str) {
			printf("%s\n", str);
			exit(EXIT_FAILURE);
		}
	}

	if (meter) {
		const char *command = NULL;

		if (!strcmp(meter, "rx"))
			command = "y";
		else if (!strcmp(meter, "tx"))
			command = "z";
		else if (!strcmp(meter, "status"))
			command = "v";
		else if (!strcmp(meter, "all"))
			command = "A";
		else {
			fprintf(stderr, "Meter must be rx, tx, status, or all\n");
			exit(EXIT_FAILURE);
		}
		printf("Continuous %s display. Press Enter to return.\n", meter);
		snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support %s", command);
		return astgetresp(str) ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	for (;;) {
		/* get device parameters from Asterisk */
		if (astgetline(COMMAND_PREFIX "tune menu-support 0+9", str, sizeof(str) - 1)) {
			printf("The setup information for chan_usbradioplus could not be "
			       "retrieved!\n\n");
			printf("Verify that Asterisk is running and chan_usbradioplus is "
			       "loaded.\n\n");
			exit(255);
		}
		if (sscanf(str,
			   "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%f,%d,%d,%d,%d,%d,%d,%d",
			   &flatrx, &txhasctcss, &echomode, &reserved_field_1, &reserved_field_2,
			   &carrierfrom, &ctcssfrom, &rxondelay, &txoffdelay, &txprelim, &txlimonly,
			   &rxdemod, &txmixa, &txmixb, &rxmixerset, &rxvoiceadj, &rxsquelchadj,
			   &txmixaset, &txmixbset, &txctcssadj, &micplaymax, &spkrmax,
			   &micmax) != 23) {
			fprintf(stderr, "Error parsing device parameters: %s\n", str);
			exit(255);
		}
		(void)reserved_field_1;
		(void)reserved_field_2;
		printf("\n");

		/* print selected USB device  at the top of our menu*/
		if (astgetresp(COMMAND_PREFIX "active")) {
			break;
		}
		printf("1) Select active USB device\n");
		if (flatrx) {
			printf("2) Auto-Detect Rx Noise Level Value (with no carrier)\n");
		} else {
			printf("2) Auto-Detect Rx Noise Level does not apply to this devices "
			       "configuration\n");
		}
		if (flatrx) {
			printf("3) Set Rx Voice Level using display (currently '%d')\n",
			       (int)((rxvoiceadj * 200.0) + .5));
		} else {
			printf("3) Set Rx Voice Level using display (currently '%d')\n",
			       rxmixerset);
		}
		if (flatrx) {
			printf("4) Auto-Detect Rx CTCSS Level Value (with carrier + CTCSS)\n");
		} else {
			printf("4) Auto-Detect Rx CTCSS Level does not apply to this devices "
			       "configuration\n");
		}
		if (flatrx) {
			printf("5) Set Rx Squelch Level (currently '%d')\n", rxsquelchadj);
		} else {
			printf("5) Set Rx Squelch Level does not apply to this devices "
			       "configuration\n");
		}
		if ((txmixa == TX_OUT_VOICE) || (txmixa == TX_OUT_COMPOSITE) ||
		    (txmixb == TX_OUT_VOICE) || (txmixb == TX_OUT_COMPOSITE)) {
			if (keying) {
				printf("6) Set Transmit Voice Level and send test tone (no "
				       "CTCSS)\n");
			} else {
				if ((txmixa == TX_OUT_VOICE) || (txmixa == TX_OUT_COMPOSITE)) {
					printf("6) Set Transmit Voice Level (currently '%d')\n",
					       txmixaset);
				} else {
					printf("6) Set Transmit Voice Level (currently '%d')\n",
					       txmixbset);
				}
			}
		} else {
			printf("7) Set Transmit Voice Level not available as configured\n");
		}
		if ((txmixa == TX_OUT_AUX) || (txmixb == TX_OUT_AUX)) {
			if (txmixa == TX_OUT_AUX) {
				printf("7) Set Transmit Aux Voice Level (currently '%d')\n",
				       txmixaset);
			} else {
				printf("7) Set Transmit Aux Voice Level (currently '%d')\n",
				       txmixbset);
			}
		} else {
			printf("7) Set Transmit Aux Voice Level not available as configured\n");
		}
		if (txhasctcss) {
			if (keying) {
				printf("8) Set Transmit CTCSS Level and send CTCSS tone\n");
			} else {
				printf("8) Set Transmit CTCSS Level (currently '%d')\n",
				       txctcssadj);
			}
		} else {
			printf("8) Set Transmit CTCSS Level does not apply to this devices "
			       "configuration\n");
		}
		if (flatrx) {
			printf("9) Auto-Detect Rx Voice Level Value (with carrier + 1KHz @ 3KHz "
			       "Dev)\n");
		} else {
			printf("9) Auto-Detect Rx Voice Level does not apply to this devices "
			       "configuration\n");
		}
		printf("E) Toggle Echo Mode (currently '%s')\n",
		       (echomode) ? "enabled" : "disabled");
		printf("F) Flash transmitter (three one-second PTT and tone bursts)\n");
		printf("G) Change Carrier From (currently '%s')\n", cd_signal_type[carrierfrom]);
		printf("H) Change CTCSS From (currently '%s')\n", sd_signal_type[ctcssfrom]);
		printf("P) Print Current Parameter Values\n");
		printf("O) Options Menu\n");
		printf("C) Configure Audio Processing\n");
		printf("R) View live RX audio statistics (press Enter to return)\n");
		printf("S) Swap Current USB device with another USB device\n");
		printf("T) Use PTT and test tone during TX level adjustments (currently '%s')\n",
		       (keying) ? "enabled" : "disabled");
		printf("V) View live COS, CTCSS, and PTT status (press Enter to return)\n");
		printf("W) Save current parameter values\n");
		printf("X) View live TX audio statistics (press Enter to return)\n");
		printf("0) Exit\n");
		printf("\nPlease enter your selection now: ");

		if (fgets(str, sizeof(str) - 1, stdin) == NULL) {
			break;
		}
		if (strlen(str) != 2) { /* it's 2 because of \n at end */
			printf("Invalid Entry, try again\n");
			continue;
		}

		/* if to exit */
		if (str[0] == '0') {
			break;
		}

		switch (str[0]) {
		case '1': /* select active usb device */
			menu_selectusb();
			break; /* select flatrx */
		case '2':
			if (!flatrx) {
				break;
			}
			if (astgetresp(COMMAND_PREFIX "tune menu-support a")) {
				exit(255);
			}
			break;
		case '3': /* set receive level using display */
			menu_rxvoice();
			break;
		case '4': /* set ctcss level */
			if (!flatrx) {
				break;
			}
			if (astgetresp(COMMAND_PREFIX "tune menu-support d")) {
				exit(255);
			}
			break;
		case '5': /* set squelch level */
			if (!flatrx) {
				break;
			}
			menu_rxsquelch();
			break;
		case '6': /* set tx level */
			menu_txvoice(keying);
			break;
		case '7': /* set aux level */
			menu_auxvoice();
			break;
		case '8': /* set ctcss level */
			if (!txhasctcss) {
				break;
			}
			menu_txtone(keying);
			break;
		case '9': /* set auto detect rx voice level */
			if (!flatrx) {
				break;
			}
			if (astgetresp(COMMAND_PREFIX "tune menu-support i")) {
				exit(255);
			}
			break;
		case 'e': /* toggle echo mode */
		case 'E':
			if (echomode) {
				if (astgetresp(COMMAND_PREFIX "tune menu-support k0")) {
					exit(255);
				}
			} else {
				if (astgetresp(COMMAND_PREFIX "tune menu-support k1")) {
					exit(255);
				}
			}
			break;
		case 'f': /* flash - toggle ptt and tone */
		case 'F':
			if (astgetresp(COMMAND_PREFIX "tune menu-support l")) {
				exit(255);
			}
			break;
		case 'g': /* select carrier from */
		case 'G':
			result = menu_select_value("Carrier From", cd_signal_type, 7, carrierfrom);
			if (result > 0) {
				snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support o%d",
					 result - 1);
				astgetresp(str);
			}
			break;
		case 'h': /* select ctcss from */
		case 'H':
			result = menu_select_value("CTCSS From", sd_signal_type, 6, ctcssfrom);
			if (result > 0) {
				snprintf(str, sizeof(str), COMMAND_PREFIX "tune menu-support p%d",
					 result - 1);
				astgetresp(str);
			}
			break;
		case 'o': /* options menu */
		case 'O':
			options_menu();
			break;
		case 'c': /* open the processing tuner */
		case 'C':
			launch_processing_tune();
			break;
		case 'p': /* print current values */
		case 'P':
			if (astgetresp(COMMAND_PREFIX "tune menu-support 2")) {
				exit(255);
			}
			break;
		case 'r': /* display receive audio statistics */
		case 'R':
			printf("Live RX audio statistics. Press Enter to return.\n");
			astgetresp(COMMAND_PREFIX "tune menu-support y");
			break;
		case 's': /* swap usb device with another device */
		case 'S':
			menu_swapusb();
			break;
		case 't': /* toggle test tone */
		case 'T':
			keying = !keying;
			printf("Transmit Test Tone/Keying is now %s\n",
			       (keying) ? "Enabled" : "Disabled");
			break;
		case 'v': /* view cos, ctcss, and ptt status - live */
		case 'V':
			menu_view_status();
			break;
		case 'w': /* write settings to configuration file */
		case 'W':
			if (astgetresp(COMMAND_PREFIX "tune menu-support j")) {
				exit(255);
			}
			break;
		case 'x': /* display transmit audio statistics */
		case 'X':
			printf("Live TX audio statistics. Press Enter to return.\n");
			astgetresp(COMMAND_PREFIX "tune menu-support z");
			break;
		default:
			printf("Invalid Entry, try again\n");
			break;
		}
	}

	return EXIT_SUCCESS;
}

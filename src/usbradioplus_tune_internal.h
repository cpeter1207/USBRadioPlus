#ifndef USBRADIOPLUS_TUNE_INTERNAL_H
#define USBRADIOPLUS_TUNE_INTERNAL_H

#include <stddef.h>
#include <sys/types.h>

typedef int (*asterisk_line_reader)(char *cmd, char *str, int max);
typedef int (*asterisk_response_reader)(char *cmd);
typedef int (*command_runner)(const char *command);

enum { TX_OUT_OFF, TX_OUT_VOICE, TX_OUT_LSD, TX_OUT_COMPOSITE, TX_OUT_AUX };

#ifdef URP_TUNE_TESTING
extern const char *const mixer_type[];
extern asterisk_line_reader astgetline;
extern asterisk_response_reader astgetresp;
extern command_runner run_command;
extern const char *asterisk_binary;
extern int (*make_pipe)(int pipefd[2]);
extern int (*set_nonblocking)(int fd);
extern int (*open_null_device)(void);
extern pid_t (*make_child)(void);
extern int (*copy_fd)(int oldfd, int newfd);
extern int (*waitfds)(int fd1, int fd2, int ms);
extern ssize_t (*read_bytes)(int fd, void *buffer, size_t count);

int set_nonblocking_real(int fd);
int open_null_real(void);
void ourhandler(int sig);
void launch_processing_tune(void);
int qcompar(const void *a, const void *b);
int explode_string(char *str, char *strp[], size_t limit, char delim, char quote);
int parse_integer(const char *text, int minimum, int maximum, int *value);
void strip_newline(char *text);
int doastcmd(char *cmd);
int waitfds_real(int fd1, int fd2, int ms);
int getcharfd(int fd);
int getstrfd(int fd, char *str, int max);
int astgetline_real(char *cmd, char *str, int max);
int astgetresp_real(char *cmd);
void menu_selectusb(void);
void menu_swapusb(void);
void menu_rxvoice(void);
void menu_rxsquelch(void);
void menu_txvoice(int keying);
void menu_auxvoice(void);
void menu_txtone(int keying);
void menu_view_status(void);
int menu_select_value(const char *value_name, const char *const *items, int max_items, int current);
int menu_get_delay(const char *delay_type, const char *menu_option, int delay);
int menu_get_integer(const char *name, int current, int minimum, int maximum);
void options_menu(void);
int usbradioplus_tune_main(int argc, char *argv[]);
#endif

#endif

#ifndef USBRADIOPLUS_CHANNEL_TEST_SHIMS_H
#define USBRADIOPLUS_CHANNEL_TEST_SHIMS_H

void test_ast_debug(int level, const char *format, ...);
void test_ast_log(int level, const char *format, ...);

/* Redirect operating-system and library boundaries for the linked channel test. */
#undef ast_debug
#define ast_debug test_ast_debug
#undef ast_log
#define ast_log test_ast_log

#endif

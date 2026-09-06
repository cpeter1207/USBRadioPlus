/** @file
 * @brief Injectable host operations used by the channel integration harness.
 */

#ifndef USBRADIOPLUS_CHANNEL_TEST_SHIMS_H
#define USBRADIOPLUS_CHANNEL_TEST_SHIMS_H

/** @brief Accept debug logging through the channel test's host-operation shim.
 * @param level Asterisk log severity or debug verbosity.
 * @param format printf-style message format.
 * @param ... Values for the preceding format string.
 */
void test_ast_debug(int level, const char *format, ...);
/** @brief Accept Asterisk logging through the channel test's host-operation shim.
 * @param level Asterisk log severity or debug verbosity.
 * @param format printf-style message format.
 * @param ... Values for the preceding format string.
 */
void test_ast_log(int level, const char *format, ...);

/* Redirect operating-system and library boundaries for the linked channel test. */
#undef ast_debug

#define ast_debug test_ast_debug
#undef ast_log

#define ast_log test_ast_log

#endif

/** @name File-local and build-time constants
 * @{ */
/** @def ast_debug
 * @brief Redirect Asterisk debug logging to the test host shim.
 */
/** @def ast_log
 * @brief Redirect Asterisk logging to the test host shim.
 */
/** @} */

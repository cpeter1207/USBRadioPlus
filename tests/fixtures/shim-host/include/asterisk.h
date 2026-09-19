#ifndef USBRADIOPLUS_TEST_ASTERISK_H
#define USBRADIOPLUS_TEST_ASTERISK_H

#define AST_MODULE_LOAD_DECLINE (-1)
#define AST_MODULE_LOAD_FAILURE (-2)
#define AST_MODULE_LOAD_SUCCESS 0
#define ASTERISK_GPL_KEY "GPL"
#define AST_MODFLAG_DEFAULT 0
#define AST_MODULE_SUPPORT_EXTENDED 1
#define LOG_ERROR 1

struct ast_module {
	int unused;
};

struct ast_module_info_fixture {
	struct ast_module *self;
};

struct ast_module_entry_points_fixture {
	int (*load)(void);
	int (*unload)(void);
	int (*reload)(void);
};

extern struct ast_module_info_fixture *ast_module_info;
extern const struct ast_module_entry_points_fixture ast_module_entry_points;

void ast_log(int level, const char *format, ...);

#define AST_MODULE_INFO(...)                                                                       \
	const struct ast_module_entry_points_fixture ast_module_entry_points = {                   \
		.load = load_module, .unload = unload_module, .reload = reload_module}

#endif

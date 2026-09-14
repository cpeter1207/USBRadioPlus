#include "../asterisk.h"

struct ast_datastore_info {
	const char *type;
	void (*destroy)(void *data);
};

struct ast_datastore {
	const struct ast_datastore_info *info;
	void *data;
};

struct ast_datastore *ast_datastore_alloc(const struct ast_datastore_info *info, const char *uid);
void ast_datastore_free(struct ast_datastore *datastore);

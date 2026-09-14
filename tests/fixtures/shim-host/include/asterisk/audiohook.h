#include "../asterisk.h"

enum ast_audiohook_type {
	AST_AUDIOHOOK_TYPE_MANIPULATE,
};

enum ast_audiohook_status {
	AST_AUDIOHOOK_STATUS_NEW,
	AST_AUDIOHOOK_STATUS_RUNNING,
	AST_AUDIOHOOK_STATUS_DONE,
};

enum ast_audiohook_direction {
	AST_AUDIOHOOK_DIRECTION_READ,
	AST_AUDIOHOOK_DIRECTION_WRITE,
};

#define AST_AUDIOHOOK_MANIPULATE_ALL_RATES (1U << 0)

struct ast_audiohook {
	enum ast_audiohook_status status;
	int (*manipulate_callback)(struct ast_audiohook *, struct ast_channel *, struct ast_frame *,
				   enum ast_audiohook_direction);
	int locked;
};

int ast_audiohook_init(struct ast_audiohook *audiohook, enum ast_audiohook_type type,
		       const char *source, unsigned int flags);
int ast_audiohook_attach(struct ast_channel *channel, struct ast_audiohook *audiohook);
void ast_audiohook_detach(struct ast_audiohook *audiohook);
void ast_audiohook_destroy(struct ast_audiohook *audiohook);
void ast_audiohook_lock(struct ast_audiohook *audiohook);
void ast_audiohook_unlock(struct ast_audiohook *audiohook);

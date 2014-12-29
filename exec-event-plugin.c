#include "exec-event-plugin.h"
#include <stdio.h>

#define	EXEC_EVENT_USER_CONTEXT(obj) \
	MODULE_CONTEXT(obj, exec_event_user_module)

#define DEFAULT_SOCKET_NAME	"/var/lib/dovecot/socket.sock"

enum exec_event_event {
	EXEC_EVENT_EVENT_SAVE		= 0x1,
	EXEC_EVENT_EVENT_COPY		= 0x2
};

#define	EXEC_EVENT_DEFAULT_EVENTS	(EXEC_EVENT_EVENT_SAVE)

struct exec_event_user {
	union mail_user_module_context	module_ctx;
	char				*username;
	const char			*socket_name;
};

struct exec_event_message {
	struct exec_event_message	*prev;
	struct exec_event_message	*next;
	enum exec_event_event		event;
	bool				ignore;
	uint32_t			uid;
	char				*destination_folder;
	char				*username;
	const char			*socket_name;
};

struct exec_event_mail_txn_context {
	pool_t				pool;
	struct exec_event_message	*messages;
	struct exec_event_message	*messages_tail;
};

static MODULE_CONTEXT_DEFINE_INIT(exec_event_user_module,
				  &mail_user_module_register);

static void exec_event_mail_user_created(struct mail_user *user)
{
	struct exec_event_user	*event_user;
	const char		*str;

	i_debug("exec_event_mail_user_created");
	i_debug("username = %s", user->username);

	event_user = p_new(user->pool, struct exec_event_user, 1);
	MODULE_CONTEXT_SET(user, exec_event_user_module, event_user);

	event_user->username = p_strdup(user->pool, user->username);
	str = mail_user_plugin_getenv(user, "exec_event_socket_name");
	if (!str) {
		event_user->socket_name = DEFAULT_SOCKET_NAME;
	} else {
		event_user->socket_name = str;
	}
	i_debug("socket_name = %s", event_user->socket_name);
}

static void exec_event_mail_save(void *txn, struct mail *mail)
{
	i_debug("exec_event_mail_save");
	i_debug("message UID = %d\n", mail->uid);
}

static void exec_event_mail_copy(void *txn, struct mail *src, struct mail *dst)
{
	struct exec_event_mail_txn_context	*ctx = (struct exec_event_mail_txn_context *) txn;
	struct exec_event_user			*mctx = EXEC_EVENT_USER_CONTEXT(dst->box->storage->user);
	struct exec_event_message		*msg;
	int					i;

	if (strcmp(src->box->storage->name, "raw") == 0) {
		/* special case: lda/lmtp is saving a mail */
		msg = p_new(ctx->pool, struct exec_event_message, 1);
		msg->event = EXEC_EVENT_EVENT_COPY;
		msg->ignore = FALSE;
		msg->username = p_strdup(ctx->pool, mctx->username);
		msg->destination_folder = p_strdup(ctx->pool, mailbox_get_name(dst->box));
		msg->socket_name = p_strdup(ctx->pool, mctx->socket_name);

		/* FIXME: Quick hack of the night */
		msg->username[0] = toupper(msg->username[0]);
		for (i = 0; i < strlen(msg->destination_folder); i++) {
			msg->destination_folder[i] = tolower(msg->destination_folder[i]);
		}

		DLLIST2_APPEND(&ctx->messages, &ctx->messages_tail, msg);		
	}
}

static void *exec_event_mail_transaction_begin(struct mailbox_transaction_context *t ATTR_UNUSED)
{
	pool_t					pool;
	struct exec_event_mail_txn_context	*ctx;

	pool = pool_alloconly_create("exec_event", 2048);
	ctx = p_new(pool, struct exec_event_mail_txn_context, 1);
	ctx->pool = pool;

	return ctx;
}



static int send_data(const char *socket_path, const char *username, uint32_t uid, const char *destination_folder)
{

	char	out_buf[1024];
	sprintf(out_buf, "{'username': '%s', 'mailbox': '%s'}\n", username, destination_folder);
	i_debug("Write '%s'", out_buf);
	int ret = -1;

	int fd = net_connect_unix(socket_path);
	if (fd == -1) {
		i_error("net_connect_unix(%s) failed: %m", socket_path);
		return -1;
	}

	net_set_nonblock(fd, FALSE);

	alarm(1);
	{
		if (net_transmit(fd, out_buf, strlen(out_buf)) < 0) {
			i_error("write(%s) failed: %m", socket_path);
			ret = -1;
		} else {
			i_debug("going on to read(%s)", socket_path);
			char res[1024];
			ret = net_receive(fd, res, sizeof(res)-1);
			if (ret < 0) {
				i_error("read(%s) failed: %m", socket_path);
			} else {
				res[ret] = '\0';
				if (strncmp(res, "OK ", 3) == 0) {
					ret = 0;
				}
			}
		}
	}
	alarm(0);
	net_disconnect(fd);
	return ret;
}


static void exec_event_mail_transaction_commit(void *txn, 
					       struct mail_transaction_commit_changes *changes)
{
	struct exec_event_mail_txn_context	*ctx = (struct exec_event_mail_txn_context *)txn;
	uint32_t				uid;
	struct exec_event_message		*msg;
	unsigned int				n = 0;
	struct seq_range_iter			iter;

	seq_range_array_iter_init(&iter, &changes->saved_uids);
	for (msg = ctx->messages; msg != NULL; msg = msg->next) {
		if (msg->event == EXEC_EVENT_EVENT_COPY) {
			if (!seq_range_array_iter_nth(&iter, n++, &uid)) uid = 0;
			msg->uid = uid;
			
			// i_debug("# uid = %d", msg->uid);
			//i_debug("# folder = %s", msg->destination_folder);
			//i_debug("# username = %s", msg->username);
			//i_debug("# socket_name = %s", msg->socket_name);

			send_data(msg->socket_name, msg->username,  msg->uid, msg->destination_folder);
		}
	}
	i_assert(!seq_range_array_iter_nth(&iter, n, &uid));

	pool_unref(&ctx->pool);
}

static void exec_event_mail_transaction_rollback(void *txn)
{
	struct exec_event_mail_txn_context	*ctx = (struct exec_event_mail_txn_context *) txn;

	pool_unref(&ctx->pool);
}

static const struct notify_vfuncs exec_event_vfuncs = {
	.mail_transaction_begin = exec_event_mail_transaction_begin,
	.mail_save = exec_event_mail_save,
	.mail_copy = exec_event_mail_copy,
	.mail_transaction_commit = exec_event_mail_transaction_commit,
	.mail_transaction_rollback = exec_event_mail_transaction_rollback,
};

static struct notify_context *exec_event_ctx;

static struct mail_storage_hooks exec_event_mail_storage_hooks = {
	.mail_user_created = exec_event_mail_user_created
};

void exec_event_plugin_init(struct module *module)
{
	i_debug("exec_event_plugin_init");
	exec_event_ctx = notify_register(&exec_event_vfuncs);
	mail_storage_hooks_add(module, &exec_event_mail_storage_hooks);
}

void exec_event_plugin_deinit(void)
{  
	i_debug("exec_event_plugin_deinit");
	mail_storage_hooks_remove(&exec_event_mail_storage_hooks);
	notify_unregister(exec_event_ctx);
}

const char *exec_event_plugin_dependencies[] = { "notify", NULL };

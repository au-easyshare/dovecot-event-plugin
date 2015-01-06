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
	const char			**sockets;
};

struct exec_event_message {
	struct exec_event_message	*prev;
	struct exec_event_message	*next;
	enum exec_event_event		event;
	bool				ignore;
	uint32_t			uid;
	char				*destination_folder;
	char				*username;
	const char			**sockets;
};

struct exec_event_mail_txn_context {
	pool_t				pool;
	struct exec_event_message	*messages;
	struct exec_event_message	*messages_tail;
};

static MODULE_CONTEXT_DEFINE_INIT(exec_event_user_module,
				  &mail_user_module_register);


#define MAX_SOCKETS	10

static const char **parse_sockets(pool_t pool, const char *str)
{
        const char *const *tmp;
	int	socket_num = 0;
	const char **sockets = (const char **)p_new(pool, char *, MAX_SOCKETS);
	i_debug("parsing '%s'", str);
        for (tmp = t_strsplit_spaces(str, ", "); *tmp != NULL; tmp++) {
		if (socket_num > MAX_SOCKETS) {
			i_error("too many sockets to write to");
			break;
		}
                sockets[socket_num++] = (const char *)p_strdup(pool, *tmp);
        }

	sockets[socket_num] = (char *)0;
        return sockets;
}


static void exec_event_mail_user_created(struct mail_user *user)
{
	struct exec_event_user	*event_user;
	const char		*str;

	i_debug("exec_event_mail_user_created");
	i_debug("username = %s", user->username);

	event_user = p_new(user->pool, struct exec_event_user, 1);
	MODULE_CONTEXT_SET(user, exec_event_user_module, event_user);

	event_user->username = p_strdup(user->pool, user->username);
	str = mail_user_plugin_getenv(user, "exec_event_socket_names");
	if (!str) {
		i_error("Sockets should have at least one entry exec_event_socket_names");
		event_user->sockets = (const char **)0;
	} else {
		event_user->sockets = parse_sockets(user->pool, str);
	}
	if (event_user->sockets != (const char **)0) {
		for (const char **tmp = event_user->sockets; *tmp != (const char *)0; tmp++) {
			i_debug("socket to write to '%s'", *tmp);
		}
	}
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

	if (strcmp(src->box->storage->name, "raw") == 0) {
		msg = p_new(ctx->pool, struct exec_event_message, 1);
		msg->event = EXEC_EVENT_EVENT_COPY;
		msg->ignore = FALSE;
		msg->username = p_strdup(ctx->pool, mctx->username);
		msg->destination_folder = p_strdup(ctx->pool, mailbox_get_name(dst->box));
		// msg->socket_name = p_strdup(ctx->pool, mctx->socket_name);
		msg->sockets = mctx->sockets;
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



static int send_data(const char **sockets, const char *username, uint32_t uid, const char *destination_folder)
{
	char	out_buf[1024];
	sprintf(out_buf, "{\"username\": \"%s\", \"mailbox\": \"%s\"}\n", username, destination_folder);
	i_debug("Write '%s'", out_buf);
	int ret = -1;

	if (sockets == (const char **)0) {
		i_error("No sockets configured to send");
		return -1;
	}
	for (; *sockets != (const char *)0; sockets++) {
		int fd = net_connect_unix(*sockets);
		if (fd == -1) {
			i_error("net_connect_unix(%s) failed: %m", *sockets);
			continue;
		}
		net_set_nonblock(fd, FALSE);
		alarm(1);
		{
			if (net_transmit(fd, out_buf, strlen(out_buf)) < 0) {
				i_error("write(%s) failed: %m", *sockets);
				ret = -1;
			} else {
				char res[1024];
				ret = net_receive(fd, res, sizeof(res)-1);
				if (ret < 0) {
					i_debug("read(%s) failed: %m", *sockets);
				} else {
					res[ret] = '\0';
					if (strncmp(res, "OK", 2) == 0) {
						ret = 0;
						i_debug("GOT OK");
					} else {
						i_debug("didn't get OK");
					}
				}
			}
		}
		alarm(0);
		net_disconnect(fd);
	}
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

			send_data(msg->sockets, msg->username,  msg->uid, msg->destination_folder);
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

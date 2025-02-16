/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "lua_common.h"
#include "dns.h"

#ifdef WITH_HIREDIS
#include "hiredis.h"
#include "adapters/libevent.h"
#endif

#define REDIS_DEFAULT_TIMEOUT 1.0

/***
 * @module rspamd_redis
 * This module implements redis asynchronous client for rspamd LUA API.
 * Here is an example of using of this module:
 * @example
local rspamd_redis = require "rspamd_redis"
local rspamd_logger = require "rspamd_logger"

local function symbol_callback(task)
	local redis_key = 'some_key'
	local function redis_cb(task, err, data)
		if not err then
			rspamd_logger.infox('redis returned %1=%2', redis_key, data)
		end
	end

	rspamd_redis.make_request(task, "127.0.0.1:6379", redis_cb,
		'GET', {redis_key})
	-- or in table form:
	-- rspamd_redis.make_request({task=task, host="127.0.0.1:6379,
	--	callback=redis_cb, timeout=2.0, cmd='GET', args={redis_key}})
end
 */

LUA_FUNCTION_DEF (redis, make_request);
LUA_FUNCTION_DEF (redis, make_request_sync);
LUA_FUNCTION_DEF (redis, connect);
LUA_FUNCTION_DEF (redis, connect_sync);
LUA_FUNCTION_DEF (redis, add_cmd);
LUA_FUNCTION_DEF (redis, exec);
LUA_FUNCTION_DEF (redis, gc);

static const struct luaL_reg redislib_f[] = {
	LUA_INTERFACE_DEF (redis, make_request),
	LUA_INTERFACE_DEF (redis, make_request_sync),
	LUA_INTERFACE_DEF (redis, connect),
	LUA_INTERFACE_DEF (redis, connect_sync),
	{NULL, NULL}
};

static const struct luaL_reg redislib_m[] = {
	LUA_INTERFACE_DEF (redis, add_cmd),
	LUA_INTERFACE_DEF (redis, exec),
	{"__gc", lua_redis_gc},
	{"__tostring", rspamd_lua_class_tostring},
};

#ifdef WITH_HIREDIS
/**
 * Struct for userdata representation
 */
struct lua_redis_userdata {
	redisAsyncContext *ctx;
	lua_State *L;
	struct rspamd_task *task;
	struct event timeout;
	gchar *server;
	gchar *reqline;
	gchar **args;
	gint cbref;
	guint nargs;
	guint16 port;
	guint16 terminated;
};

struct lua_redis_ctx {
	gboolean async;
	union {
		struct lua_redis_userdata async;
		redisContext *sync;
	} d;
	guint cmds_pending;
	ref_entry_t ref;
};

static struct lua_redis_ctx *
lua_check_redis (lua_State * L, gint pos)
{
	void *ud = luaL_checkudata (L, pos, "rspamd{redis}");
	luaL_argcheck (L, ud != NULL, pos, "'redis' expected");
	return ud ? *((struct lua_redis_ctx **)ud) : NULL;
}

static void
lua_redis_free_args (char **args, guint nargs)
{
	guint i;

	if (args) {
		for (i = 0; i < nargs; i ++) {
			g_free (args[i]);
		}

		g_free (args);
	}
}

static void
lua_redis_dtor (struct lua_redis_ctx *ctx)
{
	struct lua_redis_userdata *ud;

	if (ctx->async) {
		ud = &ctx->d.async;

		if (ud->ctx) {
			ud->terminated = 1;
			/*
			 * On calling of redisFree, hiredis calls for callbacks pending
			 * Hence, to avoid double free, we ensure that the object must
			 * still be alive here!
			 */
			ctx->ref.refcount = 100500;
			redisAsyncFree (ud->ctx);
			ctx->ref.refcount = 0;
			lua_redis_free_args (ud->args, ud->nargs);
			event_del (&ud->timeout);
			luaL_unref (ud->L, LUA_REGISTRYINDEX, ud->cbref);
		}
	}
	else {
		if (ctx->d.sync) {
			redisFree (ctx->d.sync);
		}
	}

	g_slice_free1 (sizeof (*ctx), ctx);
}

static gint
lua_redis_gc (lua_State *L)
{
	struct lua_redis_ctx *ctx = lua_check_redis (L, 1);

	if (ctx) {
		REF_RELEASE (ctx);
	}

	return 0;
}

static void
lua_redis_fin (void *arg)
{
	struct lua_redis_ctx *ctx = arg;

	REF_RELEASE (ctx);
}

/**
 * Push error of redis request to lua callback
 * @param code
 * @param ud
 */
static void
lua_redis_push_error (const gchar *err,
	struct lua_redis_ctx *ctx,
	gboolean connected)
{
	struct rspamd_task **ptask;
	struct lua_redis_userdata *ud = &ctx->d.async;

	/* Push error */
	lua_rawgeti (ud->L, LUA_REGISTRYINDEX, ud->cbref);
	ptask = lua_newuserdata (ud->L, sizeof (struct rspamd_task *));
	rspamd_lua_setclass (ud->L, "rspamd{task}", -1);

	*ptask = ud->task;
	/* String of error */
	lua_pushstring (ud->L, err);
	/* Data is nil */
	lua_pushnil (ud->L);
	if (lua_pcall (ud->L, 3, 0, 0) != 0) {
		msg_info ("call to callback failed: %s", lua_tostring (ud->L, -1));
		lua_pop (ud->L, 1);
	}

	if (connected) {
		rspamd_session_remove_event (ud->task->s, lua_redis_fin, ctx);
	}
}

static void
lua_redis_push_reply (lua_State *L, const redisReply *r)
{
	guint i;

	switch (r->type) {
	case REDIS_REPLY_INTEGER:
		lua_pushnumber (L, r->integer);
		break;
	case REDIS_REPLY_NIL:
		/* XXX: not the best approach */
		lua_newuserdata (L, sizeof (gpointer));
		break;
	case REDIS_REPLY_STRING:
	case REDIS_REPLY_STATUS:
		lua_pushlstring (L, r->str, r->len);
		break;
	case REDIS_REPLY_ARRAY:
		lua_createtable (L, r->elements, 0);
		for (i = 0; i < r->elements; ++i) {
			lua_redis_push_reply (L, r->element[i]);
			lua_rawseti (L, -2, i + 1); /* Store sub-reply */
		}
		break;
	default: /* should not happen */
		msg_info ("unknown reply type: %d", r->type);
		break;
	}
}

/**
 * Push data of redis request to lua callback
 * @param r redis reply data
 * @param ud
 */
static void
lua_redis_push_data (const redisReply *r, struct lua_redis_ctx *ctx)
{
	struct rspamd_task **ptask;
	struct lua_redis_userdata *ud = &ctx->d.async;

	/* Push error */
	lua_rawgeti (ud->L, LUA_REGISTRYINDEX, ud->cbref);
	ptask = lua_newuserdata (ud->L, sizeof (struct rspamd_task *));
	rspamd_lua_setclass (ud->L, "rspamd{task}", -1);

	*ptask = ud->task;
	/* Error is nil */
	lua_pushnil (ud->L);
	/* Data */
	lua_redis_push_reply (ud->L, r);

	if (lua_pcall (ud->L, 3, 0, 0) != 0) {
		msg_info ("call to callback failed: %s", lua_tostring (ud->L, -1));
		lua_pop (ud->L, 1);
	}

	rspamd_session_remove_event (ud->task->s, lua_redis_fin, ctx);
}

/**
 * Callback for redis replies
 * @param c context of redis connection
 * @param r redis reply
 * @param priv userdata
 */
static void
lua_redis_callback (redisAsyncContext *c, gpointer r, gpointer priv)
{
	redisReply *reply = r;
	struct lua_redis_ctx *ctx = priv;
	struct lua_redis_userdata *ud;

	REF_RETAIN (ctx);

	ud = &ctx->d.async;

	if (ud->terminated) {
		/* We are already at the termination stage, just go out */
		REF_RELEASE (ctx);
		return;
	}

	if (c->err == 0) {
		if (r != NULL) {
			if (reply->type != REDIS_REPLY_ERROR) {
				lua_redis_push_data (reply, ctx);
			}
			else {
				lua_redis_push_error (reply->str, ctx, TRUE);
			}
		}
		else {
			lua_redis_push_error ("received no data from server", ctx, TRUE);
		}
	}
	else {
		if (c->err == REDIS_ERR_IO) {
			lua_redis_push_error (strerror (errno), ctx, TRUE);
		}
		else {
			lua_redis_push_error (c->errstr, ctx, TRUE);
		}
	}

	REF_RELEASE (ctx);
}

static void
lua_redis_timeout (int fd, short what, gpointer u)
{
	struct lua_redis_ctx *ctx = u;

	REF_RETAIN (ctx);
	msg_info ("timeout while querying redis server");
	lua_redis_push_error ("timeout while connecting the server", ctx, TRUE);
	REF_RELEASE (ctx);
}


static void
lua_redis_parse_args (lua_State *L, gint idx, const gchar *cmd,
		gchar ***pargs, guint *nargs)
{
	gchar **args = NULL;
	gint top;

	if (idx != 0 && lua_type (L, idx) == LUA_TTABLE) {
		/* Get all arguments */
		lua_pushvalue (L, idx);
		lua_pushnil (L);
		top = 0;

		while (lua_next (L, -2) != 0) {
			if (lua_isstring (L, -1)) {
				top ++;
			}
			lua_pop (L, 1);
		}

		args = g_malloc ((top + 1) * sizeof (gchar *));
		lua_pushnil (L);
		args[0] = g_strdup (cmd);
		top = 1;

		while (lua_next (L, -2) != 0) {
			if (lua_isstring (L, -1)) {
				args[top++] = g_strdup (lua_tostring (L, -1));
			}
			lua_pop (L, 1);
		}

		lua_pop (L, 1);
	}
	else {
		/* Use merely cmd */
		args = g_malloc (sizeof (gchar *));
		args[0] = g_strdup (cmd);
		top = 1;
	}

	*pargs = args;
	*nargs = top;
}

static void
lua_redis_connect_cb (const struct redisAsyncContext *c, int status)
{
	/*
	 * Workaround to prevent double close:
	 * https://groups.google.com/forum/#!topic/redis-db/mQm46XkIPOY
	 */
#if defined(HIREDIS_MAJOR) && HIREDIS_MAJOR == 0 && HIREDIS_MINOR <= 11
	struct redisAsyncContext *nc = (struct redisAsyncContext *)c;
	if (status == REDIS_ERR) {
		nc->c.fd = -1;
	}
#endif
}



/***
 * @function rspamd_redis.make_request({params})
 * Make request to redis server, params is a table of key=value arguments in any order
 * @param {task} task worker task object
 * @param {ip|string} host server address
 * @param {function} callback callback to be called in form `function (task, err, data)`
 * @param {string} cmd command to be sent to redis
 * @param {table} args numeric array of strings used as redis arguments
 * @param {number} timeout timeout in seconds for request (1.0 by default)
 * @return {boolean} `true` if a request has been scheduled
 */
static int
lua_redis_make_request (lua_State *L)
{
	struct lua_redis_ctx *ctx;
	rspamd_inet_addr_t *ip = NULL;
	struct lua_redis_userdata *ud;
	struct rspamd_lua_ip *addr = NULL;
	struct rspamd_task *task = NULL;
	const gchar *cmd = NULL, *host;
	const gchar *password = NULL, *dbname = NULL;
	gint top, cbref = -1;
	struct timeval tv;
	gboolean ret = FALSE;
	gdouble timeout = REDIS_DEFAULT_TIMEOUT;

	if (lua_istable (L, 1)) {
		/* Table version */
		lua_pushstring (L, "task");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TUSERDATA) {
			task = lua_check_task (L, -1);
		}
		lua_pop (L, 1);

		lua_pushstring (L, "callback");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TFUNCTION) {
			/* This also pops function from the stack */
			cbref = luaL_ref (L, LUA_REGISTRYINDEX);
		}
		else {
			msg_err ("bad callback argument for lua redis");
			lua_pop (L, 1);
		}

		lua_pushstring (L, "cmd");
		lua_gettable (L, -2);
		cmd = lua_tostring (L, -1);
		lua_pop (L, 1);

		lua_pushstring (L, "host");
		lua_gettable (L, -2);

		if (lua_type (L, -1) == LUA_TUSERDATA) {
			addr = lua_check_ip (L, -1);
		}
		else if (lua_type (L, -1) == LUA_TSTRING) {
			host = lua_tostring (L, -1);

			if (rspamd_parse_inet_address (&ip, host, strlen (host))) {
				addr = g_alloca (sizeof (*addr));
				addr->addr = ip;

				if (rspamd_inet_address_get_port (ip) == 0) {
					rspamd_inet_address_set_port (ip, 6379);
				}

				if (task) {
					rspamd_mempool_add_destructor (task->task_pool,
							(rspamd_mempool_destruct_t)rspamd_inet_address_destroy,
							ip);
				}
			}
		}

		lua_pop (L, 1);

		lua_pushstring (L, "timeout");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TNUMBER) {
			timeout = lua_tonumber (L, -1);
		}
		lua_pop (L, 1);

		lua_pushstring (L, "password");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TSTRING) {
			password = lua_tostring (L, -1);
		}
		lua_pop (L, 1);

		lua_pushstring (L, "dbname");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TSTRING) {
			dbname = lua_tostring (L, -1);
		}
		lua_pop (L, 1);


		if (task != NULL && addr != NULL && cbref != -1 && cmd != NULL) {
			ctx = g_slice_alloc0 (sizeof (struct lua_redis_ctx));
			REF_INIT_RETAIN (ctx, lua_redis_dtor);
			ctx->async = TRUE;
			ud = &ctx->d.async;
			ud->task = task;
			ud->L = L;
			ud->cbref = cbref;
			lua_pushstring (L, "args");
			lua_gettable (L, -2);
			lua_redis_parse_args (L, -1, cmd, &ud->args, &ud->nargs);
			lua_pop (L, 1);
			ret = TRUE;
		}
		else {
			if (cbref != -1) {
				luaL_unref (L, LUA_REGISTRYINDEX, cbref);
			}

			msg_err ("incorrect function invocation");
		}
	}
	else if ((task = lua_check_task (L, 1)) != NULL) {
		addr = lua_check_ip (L, 2);
		top = lua_gettop (L);

		/* Now get callback */
		if (lua_isfunction (L, 3) && addr != NULL && addr->addr && top >= 4) {
			/* Create userdata */
			ctx = g_slice_alloc0 (sizeof (struct lua_redis_ctx));
			REF_INIT_RETAIN (ctx, lua_redis_dtor);
			ctx->async = TRUE;
			ud = &ctx->d.async;
			ud->task = task;
			ud->L = L;

			/* Pop other arguments */
			lua_pushvalue (L, 3);
			/* Get a reference */
			ud->cbref = luaL_ref (L, LUA_REGISTRYINDEX);

			cmd = luaL_checkstring (L, 4);
			if (top > 4) {
				lua_redis_parse_args (L, 5, cmd, &ud->args, &ud->nargs);
			}
			else {
				lua_redis_parse_args (L, 0, cmd, &ud->args, &ud->nargs);
			}

			ret = TRUE;
		}
		else {
			msg_err ("incorrect function invocation");
		}
	}

	if (ret) {
		ud->terminated = 0;
		ud->ctx = redisAsyncConnect (rspamd_inet_address_to_string (addr->addr),
				rspamd_inet_address_get_port (addr->addr));

		if (ud->ctx == NULL || ud->ctx->err) {
			if (ud->ctx) {
				redisAsyncFree (ud->ctx);
				ud->ctx = NULL;
			}

			REF_RELEASE (ctx);
			lua_pushboolean (L, FALSE);

			return 1;
		}

		redisAsyncSetConnectCallback (ud->ctx, lua_redis_connect_cb);
		redisLibeventAttach (ud->ctx, ud->task->ev_base);

		if (password) {
			redisAsyncCommand (ud->ctx, NULL, NULL, "AUTH %s", password);
		}
		if (dbname) {
			redisAsyncCommand (ud->ctx, NULL, NULL, "SELECT %s", dbname);
		}

		ret = redisAsyncCommandArgv (ud->ctx,
					lua_redis_callback,
					ctx,
					ud->nargs,
					(const gchar **)ud->args,
					NULL);

		if (ret == REDIS_OK) {
			rspamd_session_add_event (ud->task->s,
					lua_redis_fin,
					ctx,
					g_quark_from_static_string ("lua redis"));

			double_to_tv (timeout, &tv);
			event_set (&ud->timeout, -1, EV_TIMEOUT, lua_redis_timeout, ctx);
			event_base_set (ud->task->ev_base, &ud->timeout);
			event_add (&ud->timeout, &tv);
		}
		else {
			msg_info ("call to redis failed: %s", ud->ctx->errstr);
			redisAsyncFree (ud->ctx);
			ud->ctx = NULL;
			REF_RELEASE (ctx);
		}
	}

	lua_pushboolean (L, ret);

	return 1;
}

/***
 * @function rspamd_redis.make_request_sync({params})
 * Make blocking request to redis server, params is a table of key=value arguments in any order
 * @param {ip|string} host server address
 * @param {string} cmd command to be sent to redis
 * @param {table} args numeric array of strings used as redis arguments
 * @param {number} timeout timeout in seconds for request (1.0 by default)
 * @return {boolean + result} `true` and a result if a request has been successful
 */
static int
lua_redis_make_request_sync (lua_State *L)
{
	struct rspamd_lua_ip *addr = NULL;
	rspamd_inet_addr_t *ip = NULL;
	const gchar *cmd = NULL, *host;
	struct timeval tv;
	gboolean ret = FALSE;
	gdouble timeout = REDIS_DEFAULT_TIMEOUT;
	gchar **args = NULL;
	guint nargs = 0;
	redisContext *ctx;
	redisReply *r;

	if (lua_istable (L, 1)) {

		lua_pushstring (L, "cmd");
		lua_gettable (L, -2);
		cmd = lua_tostring (L, -1);
		lua_pop (L, 1);

		lua_pushstring (L, "host");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TUSERDATA) {
			addr = lua_check_ip (L, -1);
		}
		else if (lua_type (L, -1) == LUA_TSTRING) {
			host = lua_tostring (L, -1);
			if (rspamd_parse_inet_address (&ip, host, strlen (host))) {
				addr = g_alloca (sizeof (*addr));
				addr->addr = ip;

				if (rspamd_inet_address_get_port (ip) == 0) {
					rspamd_inet_address_set_port (ip, 6379);
				}
			}
		}
		lua_pop (L, 1);

		lua_pushstring (L, "timeout");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TNUMBER) {
			timeout = lua_tonumber (L, -1);
		}
		lua_pop (L, 1);

		lua_pushstring (L, "args");
		lua_gettable (L, -2);
		lua_redis_parse_args (L, -1, cmd, &args, &nargs);
		lua_pop (L, 1);

		if (addr && cmd) {
			ret = TRUE;
		}
	}

	if (ret) {
		double_to_tv (timeout, &tv);
		ctx = redisConnectWithTimeout (rspamd_inet_address_to_string (addr->addr),
				rspamd_inet_address_get_port (addr->addr), tv);

		if (ip) {
			rspamd_inet_address_destroy (ip);
		}

		if (ctx == NULL || ctx->err) {
			redisFree (ctx);
			lua_redis_free_args (args, nargs);
			lua_pushboolean (L, FALSE);

			return 1;
		}

		r = redisCommandArgv (ctx,
					nargs,
					(const gchar **)args,
					NULL);

		if (r != NULL) {
			if (r->type != REDIS_REPLY_ERROR) {
				lua_pushboolean (L, TRUE);
				lua_redis_push_reply (L, r);
			}
			else {
				lua_pushboolean (L, FALSE);
				lua_pushstring (L, r->str);
			}
			freeReplyObject (r);
			redisFree (ctx);

			return 2;
		}
		else {
			msg_info ("call to redis failed: %s", ctx->errstr);
			redisFree (ctx);
			lua_redis_free_args (args, nargs);
			lua_pushboolean (L, FALSE);
		}
	}
	else {
		if (ip) {
			rspamd_inet_address_destroy (ip);
		}
		msg_err ("bad arguments for redis request");
		lua_pushboolean (L, FALSE);
	}

	return 1;
}

/***
 * @function rspamd_redis.connect({params})
 * Make request to redis server, params is a table of key=value arguments in any order
 * @param {task} task worker task object
 * @param {ip|string} host server address
 * @param {number} timeout timeout in seconds for request (1.0 by default)
 * @return {redis} new connection object or nil if connection failed
 */
static int
lua_redis_connect (lua_State *L)
{
	struct rspamd_lua_ip *addr = NULL;
	rspamd_inet_addr_t *ip = NULL;
	const gchar *host;
	struct lua_redis_ctx *ctx = NULL, **pctx;
	struct lua_redis_userdata *ud;
	struct rspamd_task *task = NULL;
	gboolean ret = FALSE;

	if (lua_istable (L, 1)) {
		/* Table version */
		lua_pushstring (L, "task");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TUSERDATA) {
			task = lua_check_task (L, -1);
		}
		lua_pop (L, 1);


		lua_pushstring (L, "host");
		lua_gettable (L, -2);

		if (lua_type (L, -1) == LUA_TUSERDATA) {
			addr = lua_check_ip (L, -1);
		}
		else if (lua_type (L, -1) == LUA_TSTRING) {
			host = lua_tostring (L, -1);

			if (rspamd_parse_inet_address (&ip, host, strlen (host))) {
				addr = g_alloca (sizeof (*addr));
				addr->addr = ip;

				if (rspamd_inet_address_get_port (ip) == 0) {
					rspamd_inet_address_set_port (ip, 6379);
				}

				if (task) {
					rspamd_mempool_add_destructor (task->task_pool,
							(rspamd_mempool_destruct_t)rspamd_inet_address_destroy,
							ip);
				}
			}
		}

		lua_pop (L, 1);

		if (task != NULL && addr != NULL) {
			ctx = g_slice_alloc0 (sizeof (struct lua_redis_ctx));
			REF_INIT_RETAIN (ctx, lua_redis_dtor);
			ctx->async = TRUE;
			ud = &ctx->d.async;
			ud->task = task;
			ud->L = L;
			ud->cbref = -1;
			ret = TRUE;
		}
	}

	if (ret && ctx) {
		ud->terminated = 0;
		ud->ctx = redisAsyncConnect (rspamd_inet_address_to_string (addr->addr),
				rspamd_inet_address_get_port (addr->addr));

		if (ud->ctx == NULL || ud->ctx->err) {
			REF_RELEASE (ctx);
			lua_pushboolean (L, FALSE);

			return 1;
		}

		redisAsyncSetConnectCallback (ud->ctx, lua_redis_connect_cb);
		redisLibeventAttach (ud->ctx, ud->task->ev_base);
		pctx = lua_newuserdata (L, sizeof (ctx));
		*pctx = ctx;
		rspamd_lua_setclass (L, "rspamd{redis}", -1);
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

/***
 * @function rspamd_redis.connect_sync({params})
 * Make blocking request to redis server, params is a table of key=value arguments in any order
 * @param {ip|string} host server address
 * @param {number} timeout timeout in seconds for request (1.0 by default)
 * @return {redis} redis object if a request has been successful
 */
static int
lua_redis_connect_sync (lua_State *L)
{
	struct rspamd_lua_ip *addr = NULL;
	rspamd_inet_addr_t *ip = NULL;
	const gchar *host;
	struct timeval tv;
	gboolean ret = FALSE;
	gdouble timeout = REDIS_DEFAULT_TIMEOUT;
	struct lua_redis_ctx *ctx, **pctx;

	if (lua_istable (L, 1)) {
		lua_pushstring (L, "host");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TUSERDATA) {
			addr = lua_check_ip (L, -1);
		}
		else if (lua_type (L, -1) == LUA_TSTRING) {
			host = lua_tostring (L, -1);
			if (rspamd_parse_inet_address (&ip, host, strlen (host))) {
				addr = g_alloca (sizeof (*addr));
				addr->addr = ip;

				if (rspamd_inet_address_get_port (ip) == 0) {
					rspamd_inet_address_set_port (ip, 6379);
				}
			}
		}
		lua_pop (L, 1);

		lua_pushstring (L, "timeout");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TNUMBER) {
			timeout = lua_tonumber (L, -1);
		}
		lua_pop (L, 1);

		if (addr) {
			ret = TRUE;
		}
	}

	if (ret) {
		double_to_tv (timeout, &tv);
		ctx = g_slice_alloc0 (sizeof (struct lua_redis_ctx));
		REF_INIT_RETAIN (ctx, lua_redis_dtor);
		ctx->async = FALSE;
		ctx->d.sync = redisConnectWithTimeout (
				rspamd_inet_address_to_string (addr->addr),
				rspamd_inet_address_get_port (addr->addr), tv);

		if (ip) {
			rspamd_inet_address_destroy (ip);
		}

		if (ctx->d.sync == NULL || ctx->d.sync->err) {
			lua_pushboolean (L, FALSE);

			if (ctx->d.sync) {
				lua_pushstring (L, ctx->d.sync->errstr);
			}
			else {
				lua_pushstring (L, "unknown error");
			}

			REF_RELEASE (ctx);

			return 2;
		}

		pctx = lua_newuserdata (L, sizeof (ctx));
		*pctx = ctx;
		rspamd_lua_setclass (L, "rspamd{redis}", -1);

	}
	else {
		if (ip) {
			rspamd_inet_address_destroy (ip);
		}

		lua_pushboolean (L, FALSE);
		lua_pushstring (L, "bad arguments for redis request");
		return 2;
	}

	return 1;
}

/***
 * @method rspamd_redis:add_cmd(cmd, {args})
 * Append new cmd to redis pipeline
 * @param {string} cmd command to be sent to redis
 * @param {table} args array of strings used as redis arguments
 * @return {boolean} `true` if a request has been successful
 */
static int
lua_redis_add_cmd (lua_State *L)
{
	struct lua_redis_ctx *ctx = lua_check_redis (L, 1);
	const gchar *cmd = NULL;
	gint args_pos = 2;
	gchar **args = NULL;
	guint nargs = 0;

	if (ctx) {
		if (lua_type (L, 2) == LUA_TSTRING) {
			cmd = lua_tostring (L, 2);
			args_pos = 3;
		}

		if (ctx->async) {
			lua_pushstring (L, "Async redis pipelining is not implemented");
			lua_error (L);
			return 0;
		}
		else {
			if (ctx->d.sync) {
				lua_redis_parse_args (L, args_pos, cmd, &args, &nargs);

				if (nargs > 0) {
					redisAppendCommandArgv (ctx->d.sync, nargs,
							(const char **)args, NULL);
					ctx->cmds_pending ++;
					lua_redis_free_args (args, nargs);
				}
				else {
					lua_pushstring (L, "cannot append commands when not connected");
					lua_error (L);
					return 0;
				}

			}
			else {
				lua_pushstring (L, "cannot append commands when not connected");
				lua_error (L);
				return 0;
			}
		}
	}

	lua_pushboolean (L, 1);

	return 1;
}

/***
 * @method rspamd_redis:exec()
 * Executes pending commands (suitable for blocking IO only for now)
 * @return {table} pairs in format [bool, result] for each request pending
 */
static int
lua_redis_exec (lua_State *L)
{
	struct lua_redis_ctx *ctx = lua_check_redis (L, 1);
	redisReply *r;
	gint ret;
	guint i, nret = 0;

	if (ctx == NULL) {
		lua_error (L);

		return 1;
	}

	if (ctx->async) {
		lua_pushstring (L, "Async redis pipelining is not implemented");
		lua_error (L);
		return 0;
	}
	else {
		if (!ctx->d.sync) {
			lua_pushstring (L, "cannot exec commands when not connected");
			lua_error (L);
			return 0;
		}
		else {
			for (i = 0; i < ctx->cmds_pending; i ++) {
				ret = redisGetReply (ctx->d.sync, (void **)&r);

				if (ret == REDIS_OK) {
					if (r->type != REDIS_REPLY_ERROR) {
						lua_pushboolean (L, TRUE);
						lua_redis_push_reply (L, r);
					}
					else {
						lua_pushboolean (L, FALSE);
						lua_pushstring (L, r->str);
					}
					freeReplyObject (r);
				}
				else {
					msg_info ("call to redis failed: %s", ctx->d.sync->errstr);
					lua_pushboolean (L, FALSE);
					lua_pushstring (L, ctx->d.sync->errstr);
				}

				nret += 2;
			}
		}
	}

	return nret;
}
#else
static int
lua_redis_make_request (lua_State *L)
{
	msg_warn ("rspamd is compiled with no redis support");

	lua_pushboolean (L, FALSE);

	return 1;
}
static int
lua_redis_make_request_sync (lua_State *L)
{
	msg_warn ("rspamd is compiled with no redis support");

	lua_pushboolean (L, FALSE);

	return 1;
}
static int
lua_redis_connect (lua_State *L)
{
	msg_warn ("rspamd is compiled with no redis support");

	lua_pushboolean (L, FALSE);

	return 1;
}
static int
lua_redis_connect_sync (lua_State *L)
{
	msg_warn ("rspamd is compiled with no redis support");

	lua_pushboolean (L, FALSE);

	return 1;
}
static int
lua_redis_add_cmd (lua_State *L)
{
	msg_warn ("rspamd is compiled with no redis support");

	lua_pushboolean (L, FALSE);

	return 1;
}
static int
lua_redis_exec (lua_State *L)
{
	msg_warn ("rspamd is compiled with no redis support");

	lua_pushboolean (L, FALSE);

	return 1;
}
static int
lua_redis_gc (lua_State *L)
{
	return 0;
}
#endif

static gint
lua_load_redis (lua_State * L)
{
	lua_newtable (L);
	luaL_register (L, NULL, redislib_f);

	return 1;
}
/**
 * Open redis library
 * @param L lua stack
 * @return
 */
void
luaopen_redis (lua_State * L)
{
	luaL_newmetatable (L, "rspamd{redis}");
	lua_pushstring (L, "__index");
	lua_pushvalue (L, -2);
	lua_settable (L, -3);

	lua_pushstring (L, "class");
	lua_pushstring (L, "rspamd{redis}");
	lua_rawset (L, -3);

	luaL_register (L, NULL, redislib_m);
	lua_pop (L, 1);

	rspamd_lua_add_preload (L, "rspamd_redis", lua_load_redis);
}

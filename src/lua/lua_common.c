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

/* Lua module init function */
#define MODULE_INIT_FUNC "module_init"

const luaL_reg null_reg[] = {
	{"__tostring", rspamd_lua_class_tostring},
	{NULL, NULL}
};

static GQuark
lua_error_quark (void)
{
	return g_quark_from_static_string ("lua-routines");
}

/* Util functions */
/**
 * Create new class and store metatable on top of the stack
 * @param L
 * @param classname name of class
 * @param func table of class methods
 */
void
rspamd_lua_new_class (lua_State * L,
	const gchar *classname,
	const struct luaL_reg *methods)
{
	luaL_newmetatable (L, classname);   /* mt */
	lua_pushstring (L, "__index");
	lua_pushvalue (L, -2);      /* pushes the metatable */
	lua_settable (L, -3);       /* metatable.__index = metatable */

	lua_pushstring (L, "class");    /* mt,"__index",it,"class" */
	lua_pushstring (L, classname);  /* mt,"__index",it,"class",classname */
	lua_rawset (L, -3);         /* mt,"__index",it */
	luaL_register (L, NULL, methods);
}

/**
 * Create and register new class with static methods and store metatable on top of the stack
 */
void
rspamd_lua_new_class_full (lua_State *L,
	const gchar *classname,
	const gchar *static_name,
	const struct luaL_reg *methods,
	const struct luaL_reg *func)
{
	rspamd_lua_new_class (L, classname, methods);
	luaL_register (L, static_name, func);
}

gint
rspamd_lua_class_tostring (lua_State * L)
{
	gchar buf[32];

	if (!lua_getmetatable (L, 1)) {
		goto error;
	}
	lua_pushstring (L, "__index");
	lua_gettable (L, -2);

	if (!lua_istable (L, -1)) {
		goto error;
	}
	lua_pushstring (L, "class");
	lua_gettable (L, -2);

	if (!lua_isstring (L, -1)) {
		goto error;
	}

	snprintf (buf, sizeof (buf), "%p", lua_touserdata (L, 1));

	lua_pushfstring (L, "%s: %s", lua_tostring (L, -1), buf);

	return 1;

error:
	lua_pushstring (L, "invalid object passed to 'lua_common.c:__tostring'");
	lua_error (L);
	return 1;
}


void
rspamd_lua_setclass (lua_State * L, const gchar *classname, gint objidx)
{
	luaL_getmetatable (L, classname);
	if (objidx < 0) {
		objidx--;
	}
	lua_setmetatable (L, objidx);
}

/* assume that table is at the top */
void
rspamd_lua_table_set (lua_State * L, const gchar *index, const gchar *value)
{

	lua_pushstring (L, index);
	if (value) {
		lua_pushstring (L, value);
	}
	else {
		lua_pushnil (L);
	}
	lua_settable (L, -3);
}

const gchar *
rspamd_lua_table_get (lua_State *L, const gchar *index)
{
	const gchar *result;

	lua_pushstring (L, index);
	lua_gettable (L, -2);
	if (!lua_isstring (L, -1)) {
		return NULL;
	}
	result = lua_tostring (L, -1);
	lua_pop (L, 1);
	return result;
}

static void
lua_add_actions_global (lua_State *L)
{
	gint i;

	lua_newtable (L);

	for (i = METRIC_ACTION_REJECT; i <= METRIC_ACTION_NOACTION; i++) {
		lua_pushstring (L, rspamd_action_to_str (i));
		lua_pushinteger (L, i);
		lua_settable (L, -3);
	}
	/* Set global table */
	lua_setglobal (L, "rspamd_actions");
}

void
rspamd_lua_set_path (lua_State *L, struct rspamd_config *cfg)
{
	const gchar *old_path, *additional_path = NULL;
	const ucl_object_t *opts;
	gchar path_buf[PATH_MAX];

	lua_getglobal (L, "package");
	lua_getfield (L, -1, "path");
	old_path = luaL_checkstring (L, -1);

	if (strstr (old_path, RSPAMD_PLUGINSDIR) != NULL) {
		/* Path has been already set, do not touch it */
		lua_pop (L, 2);
		return;
	}

	opts = ucl_object_lookup (cfg->rcl_obj, "options");
	if (opts != NULL) {
		opts = ucl_object_lookup (opts, "lua_path");
		if (opts != NULL && ucl_object_type (opts) == UCL_STRING) {
			additional_path = ucl_object_tostring (opts);
		}
	}

	if (additional_path) {
		rspamd_snprintf (path_buf, sizeof (path_buf),
				"%s/lua/?.lua;%s/lua/?.lua;%s;%s;%s",
				RSPAMD_PLUGINSDIR, RSPAMD_CONFDIR, RSPAMD_RULESDIR,
				additional_path, old_path);
	}
	else {
		rspamd_snprintf (path_buf, sizeof (path_buf),
				"%s/lua/?.lua;%s/lua/?.lua;%s;%s",
				RSPAMD_PLUGINSDIR, RSPAMD_CONFDIR, RSPAMD_RULESDIR,
				old_path);
	}

	lua_pop (L, 1);
	lua_pushstring (L, path_buf);
	lua_setfield (L, -2, "path");
	lua_pop (L, 1);
}

lua_State *
rspamd_lua_init ()
{
	lua_State *L;

	L = luaL_newstate ();
	luaL_openlibs (L);

	luaopen_logger (L);
	luaopen_mempool (L);
	luaopen_config (L);
	luaopen_map (L);
	luaopen_trie (L);
	luaopen_task (L);
	luaopen_textpart (L);
	luaopen_mimepart (L);
	luaopen_image (L);
	luaopen_url (L);
	luaopen_classifier (L);
	luaopen_statfile (L);
	luaopen_regexp (L);
	luaopen_cdb (L);
	luaopen_xmlrpc (L);
	luaopen_http (L);
	luaopen_redis (L);
	luaopen_upstream (L);
	lua_add_actions_global (L);
	luaopen_session (L);
	luaopen_io_dispatcher (L);
	luaopen_dns_resolver (L);
	luaopen_rsa (L);
	luaopen_ip (L);
	luaopen_expression (L);
	luaopen_text (L);
	luaopen_util (L);
	luaopen_tcp (L);
	luaopen_html (L);
	luaopen_fann (L);
	luaopen_sqlite3 (L);
	luaopen_cryptobox (L);

	rspamd_lua_add_preload (L, "ucl", luaopen_ucl);

	return L;
}

/**
 * Initialize new locked lua_State structure
 */
struct lua_locked_state *
rspamd_init_lua_locked (struct rspamd_config *cfg)
{
	struct lua_locked_state *new;

	new = g_slice_alloc (sizeof (struct lua_locked_state));
	new->L = rspamd_lua_init ();
	new->m = rspamd_mutex_new ();

	return new;
}


/**
 * Free locked state structure
 */
void
rspamd_free_lua_locked (struct lua_locked_state *st)
{
	g_assert (st != NULL);

	lua_close (st->L);

	rspamd_mutex_free (st->m);

	g_slice_free1 (sizeof (struct lua_locked_state), st);
}

gboolean
rspamd_init_lua_filters (struct rspamd_config *cfg)
{
	struct rspamd_config **pcfg;
	GList *cur;
	struct script_module *module;
	lua_State *L = cfg->lua_state;
	GString *tb;
	gint err_idx;

	rspamd_lua_set_path (L, cfg);
	cur = g_list_first (cfg->script_modules);

	while (cur) {
		module = cur->data;
		if (module->path) {
			if (!rspamd_config_is_module_enabled (cfg, module->name)) {
				cur = g_list_next (cur);
				continue;
			}

			lua_pushcfunction (L, &rspamd_lua_traceback);
			err_idx = lua_gettop (L);

			if (luaL_loadfile (L, module->path) != 0) {
				msg_err_config ("load of %s failed: %s", module->path,
					lua_tostring (L, -1));
				cur = g_list_next (cur);
				lua_pop (L, 1); /*  Error function */
				continue;
			}

			/* Initialize config structure */
			pcfg = lua_newuserdata (L, sizeof (struct rspamd_config *));
			rspamd_lua_setclass (L, "rspamd{config}", -1);
			*pcfg = cfg;
			lua_setglobal (L, "rspamd_config");

			if (lua_pcall (L, 0, 0, err_idx) != 0) {
				tb = lua_touserdata (L, -1);
				msg_err_config ("init of %s failed: %v",
						module->path,
						tb);
				cur = g_list_next (cur);
				g_string_free (tb, TRUE);
				lua_pop (L, 2); /* Result and error function */
				continue;
			}

			msg_info_config ("init lua module %s", module->name);

			lua_pop (L, 1); /* Error function */
		}
		cur = g_list_next (cur);
	}

	/* Assign state */
	cfg->lua_state = L;

	return TRUE;
}

/* Callback functions */

gint
rspamd_lua_call_filter (const gchar *function, struct rspamd_task *task)
{
	gint result = 0;
	struct rspamd_task **ptask;
	lua_State *L = task->cfg->lua_state;

	lua_getglobal (L, function);
	ptask = lua_newuserdata (L, sizeof (struct rspamd_task *));
	rspamd_lua_setclass (L, "rspamd{task}", -1);
	*ptask = task;

	if (lua_pcall (L, 1, 1, 0) != 0) {
		msg_info_task ("call to %s failed", function);
	}

	/* retrieve result */
	if (!lua_isnumber (L, -1)) {
		msg_info_task ("function %s must return a number", function);
	}
	else {
		result = lua_tonumber (L, -1);
	}

	lua_pop (L, 1);             /* pop returned value */

	return result;
}

gint
rspamd_lua_call_chain_filter (const gchar *function,
	struct rspamd_task *task,
	gint *marks,
	guint number)
{
	gint result;
	guint i;
	lua_State *L = task->cfg->lua_state;

	lua_getglobal (L, function);

	for (i = 0; i < number; i++) {
		lua_pushnumber (L, marks[i]);
	}
	if (lua_pcall (L, number, 1, 0) != 0) {
		msg_info_task ("call to %s failed", function);
	}

	/* retrieve result */
	if (!lua_isnumber (L, -1)) {
		msg_info_task ("function %s must return a number", function);
	}
	result = lua_tonumber (L, -1);
	lua_pop (L, 1);             /* pop returned value */

	return result;
}


/*
 * LUA custom consolidation function
 */
struct consolidation_callback_data {
	struct rspamd_task *task;
	double score;
	const gchar *func;
};

static void
lua_consolidation_callback (gpointer key, gpointer value, gpointer arg)
{
	double res;
	struct symbol *s = (struct symbol *)value;
	struct consolidation_callback_data *data =
		(struct consolidation_callback_data *)arg;
	lua_State *L;
	struct rspamd_task *task;

	L = data->task->cfg->lua_state;
	task = data->task;

	lua_getglobal (L, data->func);

	lua_pushstring (L, (const gchar *)key);
	lua_pushnumber (L, s->score);
	if (lua_pcall (L, 2, 1, 0) != 0) {
		msg_info_task ("call to %s failed", data->func);
	}

	/* retrieve result */
	if (!lua_isnumber (L, -1)) {
		msg_info_task ("function %s must return a number", data->func);
	}
	res = lua_tonumber (L, -1);
	lua_pop (L, 1);             /* pop returned value */
	data->score += res;
}

double
rspamd_lua_consolidation_func (struct rspamd_task *task,
	const gchar *metric_name,
	const gchar *function_name)
{
	struct metric_result *metric_res;
	double res = 0.;
	struct consolidation_callback_data data = { task, 0, function_name };

	if (function_name == NULL) {
		return 0;
	}

	metric_res = g_hash_table_lookup (task->results, metric_name);
	if (metric_res == NULL) {
		return res;
	}

	g_hash_table_foreach (metric_res->symbols, lua_consolidation_callback,
		&data);

	return data.score;
}

double
rspamd_lua_normalize (struct rspamd_config *cfg, long double score, void *params)
{
	GList *p = params;
	long double res = score;
	lua_State *L = cfg->lua_state;

	/* Call specified function and put input score on stack */
	if (!p->data) {
		msg_info_config ("bad function name while calling normalizer");
		return score;
	}

	lua_getglobal (L, p->data);
	lua_pushnumber (L, score);

	if (lua_pcall (L, 1, 1, 0) != 0) {
		msg_info_config ("call to %s failed", p->data);
	}

	/* retrieve result */
	if (!lua_isnumber (L, -1)) {
		msg_info_config ("function %s must return a number", p->data);
	}
	res = lua_tonumber (L, -1);
	lua_pop (L, 1);

	return res;
}


void
rspamd_lua_dumpstack (lua_State *L)
{
	gint i, t, r = 0;
	gint top = lua_gettop (L);
	gchar buf[BUFSIZ];

	r += rspamd_snprintf (buf + r, sizeof (buf) - r, "lua stack: ");
	for (i = 1; i <= top; i++) { /* repeat for each level */
		t = lua_type (L, i);
		switch (t)
		{
		case LUA_TSTRING: /* strings */
			r += rspamd_snprintf (buf + r,
					sizeof (buf) - r,
					"str: %s",
					lua_tostring (L, i));
			break;

		case LUA_TBOOLEAN: /* booleans */
			r += rspamd_snprintf (buf + r, sizeof (buf) - r,
					lua_toboolean (L, i) ? "bool: true" : "bool: false");
			break;

		case LUA_TNUMBER: /* numbers */
			r += rspamd_snprintf (buf + r,
					sizeof (buf) - r,
					"number: %.2f",
					lua_tonumber (L, i));
			break;

		default: /* other values */
			r += rspamd_snprintf (buf + r,
					sizeof (buf) - r,
					"type: %s",
					lua_typename (L, t));
			break;

		}
		if (i < top) {
			r += rspamd_snprintf (buf + r, sizeof (buf) - r, " -> "); /* put a separator */
		}
	}
	msg_info (buf);
}

gpointer
rspamd_lua_check_class (lua_State *L, gint index, const gchar *name)
{
	gpointer p;

	if (lua_type (L, index) == LUA_TUSERDATA) {
		p = lua_touserdata (L, index);
		if (p) {
			if (lua_getmetatable (L, index)) {
				lua_getfield (L, LUA_REGISTRYINDEX, name);  /* get correct metatable */
				if (lua_rawequal (L, -1, -2)) {  /* does it have the correct mt? */
					lua_pop (L, 2);  /* remove both metatables */
					return p;
				}
				lua_pop (L, 2);
			}
		}
	}
	return NULL;
}

int
rspamd_lua_typerror (lua_State *L, int narg, const char *tname)
{
	const char *msg = lua_pushfstring (L, "%s expected, got %s", tname,
			luaL_typename (L, narg));
	return luaL_argerror (L, narg, msg);
}


void
rspamd_lua_add_preload (lua_State *L, const gchar *name, lua_CFunction func)
{
	lua_getglobal (L, "package");
	lua_pushstring (L, "preload");
	lua_gettable (L, -2);
	lua_pushcfunction (L, func);
	lua_setfield (L, -2, name);
	lua_pop (L, 1);
}


gboolean
rspamd_lua_parse_table_arguments (lua_State *L, gint pos,
		GError **err, const gchar *extraction_pattern, ...)
{
	const gchar *p, *key = NULL, *end, *cls;
	va_list ap;
	gboolean required = FALSE, failed = FALSE;
	gchar classbuf[128];
	enum {
		read_key = 0,
		read_arg,
		read_class_start,
		read_class,
		read_semicolon
	} state = read_key;
	gsize keylen = 0, *valuelen, clslen;

	if (pos < 0) {
		/* Get absolute pos */
		pos = lua_gettop (L) + pos + 1;
	}

	g_assert (extraction_pattern != NULL);
	g_assert (lua_type (L, pos) == LUA_TTABLE);

	p = extraction_pattern;
	end = p + strlen (extraction_pattern);

	va_start (ap, extraction_pattern);

	while (p <= end) {
		switch (state) {
		case read_key:
			if (*p == '=') {
				if (key == NULL) {
					g_set_error (err, lua_error_quark (), 1, "cannot read key");
					va_end (ap);

					return FALSE;
				}

				state = read_arg;
				keylen = p - key;
			}
			else if (*p == '*' && key == NULL) {
				required = TRUE;
			}
			else if (key == NULL) {
				key = p;
			}
			p ++;
			break;
		case read_arg:
			g_assert (keylen != 0);
			lua_pushlstring (L, key, keylen);
			lua_gettable (L, pos);

			switch (g_ascii_toupper (*p)) {
			case 'S':
				if (lua_type (L, -1) == LUA_TSTRING) {
					*(va_arg (ap, const gchar **)) = lua_tostring (L, -1);
				}
				else if (lua_type (L, -1) == LUA_TNIL) {
					failed = TRUE;
					*(va_arg (ap, const gchar **)) = NULL;
				}
				else {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, -1)), "string");
					va_end (ap);

					return FALSE;
				}
				lua_pop (L, 1);
				break;

			case 'I':
				if (lua_type (L, -1) == LUA_TNUMBER) {
					*(va_arg (ap, gint64 *)) = lua_tonumber (L, -1);
				}
				else if (lua_type (L, -1) == LUA_TNIL) {
					failed = TRUE;
					*(va_arg (ap,  gint64 *)) = 0;
				}
				else {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, -1)),
							"int64");
					va_end (ap);

					return FALSE;
				}
				lua_pop (L, 1);
				break;

			case 'F':
				if (lua_type (L, -1) == LUA_TFUNCTION) {
					*(va_arg (ap, gint *)) = luaL_ref (L, LUA_REGISTRYINDEX);
				}
				else if (lua_type (L, -1) == LUA_TNIL) {
					failed = TRUE;
					*(va_arg (ap,  gint *)) = -1;
					lua_pop (L, 1);
				}
				else {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, -1)),
							"function");
					va_end (ap);
					lua_pop (L, 1);

					return FALSE;
				}

				/* luaL_ref pops argument from the stack */
				break;

			case 'B':
				if (lua_type (L, -1) == LUA_TBOOLEAN) {
					*(va_arg (ap, gboolean *)) = lua_toboolean (L, -1);
				}
				else if (lua_type (L, -1) == LUA_TNIL) {
					failed = TRUE;
					*(va_arg (ap,  gboolean *)) = 0;
				}
				else {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, -1)),
							"bool");
					va_end (ap);

					return FALSE;
				}
				lua_pop (L, 1);
				break;

			case 'N':
				if (lua_type (L, -1) == LUA_TNUMBER) {
					*(va_arg (ap, gdouble *)) = lua_tonumber (L, -1);
				}
				else if (lua_type (L, -1) == LUA_TNIL) {
					failed = TRUE;
					*(va_arg (ap,  gdouble *)) = 0;
				}
				else {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, -1)),
							"double");
					va_end (ap);

					return FALSE;
				}
				lua_pop (L, 1);
				break;

			case 'V':
				valuelen = va_arg (ap, gsize *);
				if (lua_type (L, -1) == LUA_TSTRING) {
					*(va_arg (ap, const gchar **)) = lua_tolstring (L, -1,
							valuelen);
				}
				else if (lua_type (L, -1) == LUA_TNIL) {
					failed = TRUE;
					*(va_arg (ap, const char **)) = NULL;
					*valuelen = 0;
				}
				else {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, -1)),
							"string");
					va_end (ap);

					return FALSE;
				}
				lua_pop (L, 1);
				break;
			case 'U':
				if (lua_type (L, -1) == LUA_TNIL) {
					failed = TRUE;
				}
				else if (lua_type (L, -1) != LUA_TUSERDATA) {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, -1)),
							"int64");
					va_end (ap);

					return FALSE;
				}

				state = read_class_start;
				clslen = 0;
				cls = NULL;
				p ++;
				continue;
			default:
				g_assert (0);
				break;
			}

			if (failed && required) {
				g_set_error (err, lua_error_quark (), 2, "required parameter "
						"%.*s is missing", (gint)keylen, key);
				va_end (ap);

				return FALSE;
			}

			/* Reset read params */
			state = read_semicolon;
			failed = FALSE;
			required = FALSE;
			keylen = 0;
			key = NULL;
			p ++;
			break;

		case read_class_start:
			if (*p == '{') {
				cls = p + 1;
				state = read_class;
			}
			else {
				lua_pop (L, 1);
				g_set_error (err, lua_error_quark (), 2, "missing classname for "
						"%.*s", (gint)keylen, key);
				va_end (ap);

				return FALSE;
			}
			p ++;
			break;

		case read_class:
			if (*p == '}') {
				clslen = p - cls;
				if (clslen == 0) {
					lua_pop (L, 1);
					g_set_error (err,
							lua_error_quark (),
							2,
							"empty classname for "
									"%*.s",
							(gint) keylen,
							key);
					va_end (ap);

					return FALSE;
				}

				rspamd_snprintf (classbuf, sizeof (classbuf), "rspamd{%*s}",
						(gint) clslen, cls);

				if (!failed && rspamd_lua_check_class (L, -1, classbuf)) {
					*(va_arg (ap, void **)) = *(void **)lua_touserdata (L, -1);
				}
				else {
					if (!failed) {
						g_set_error (err,
								lua_error_quark (),
								2,
								"invalid class for key %*.s, expected %s, got %s",
								(gint) keylen,
								key,
								classbuf,
								lua_tostring (L, -1));
						va_end (ap);

						return FALSE;
					}
				}

				lua_pop (L, 1);

				if (failed && required) {
					g_set_error (err,
							lua_error_quark (),
							2,
							"required parameter "
									"%.*s is missing",
							(gint) keylen,
							key);
					va_end (ap);

					return FALSE;
				}

				/* Reset read params */
				state = read_semicolon;
				failed = FALSE;
				required = FALSE;
				keylen = 0;
				key = NULL;
			}
			p ++;
			break;

		case read_semicolon:
			if (*p == ';' || p == end) {
				state = read_key;
				key = NULL;
				keylen = 0;
				failed = FALSE;
			}
			else {
				g_set_error (err, lua_error_quark (), 2, "bad format string: %s,"
								" at char %c, position %d",
						extraction_pattern, *p, (int)(p - extraction_pattern));
				va_end (ap);

				return FALSE;
			}

			p++;
			break;
		}
	}

	va_end (ap);

	return TRUE;
}

gint
rspamd_lua_traceback (lua_State *L)
{
	lua_Debug d;
	GString *tb;
	const gchar *msg = lua_tostring (L, 1);
	gint i = 1;

	tb = g_string_sized_new (100);
	g_string_append_printf (tb, "%s; trace:", msg);

	while (lua_getstack (L, i++, &d)) {
		lua_getinfo (L, "nSl", &d);
		g_string_append_printf (tb, " [%d]:{%s:%d - %s [%s]};",
				i - 1, d.short_src, d.currentline,
				(d.name ? d.name : "<unknown>"), d.what);
	}

	lua_pushlightuserdata (L, tb);

	return 1;
}

guint
rspamd_lua_table_size (lua_State *L, gint tbl_pos)
{
	guint tbl_size = 0;

	if (!lua_istable (L, tbl_pos)) {
		return 0;
	}

#if LUA_VERSION_NUM >= 502
	tbl_size = lua_rawlen (L, tbl_pos);
#else
	tbl_size = lua_objlen (L, tbl_pos);
#endif

	return tbl_size;
}

gboolean
lua_push_internet_address (lua_State *L, InternetAddress *ia)
{
	const char *addr, *at;

#ifndef GMIME24
	if (internet_address_get_type (ia) == INTERNET_ADDRESS_NAME) {
		lua_newtable (L);
		addr = internet_address_get_addr (ia);
		rspamd_lua_table_set (L, "name", internet_address_get_name (ia));
		rspamd_lua_table_set (L, "addr", addr);

		if (addr) {
			at = strchr (addr, '@');
			if (at != NULL) {
				lua_pushstring(L, "user");
				lua_pushlstring(L, addr, at - addr);
				lua_settable (L, -3);
				lua_pushstring (L, "domain");
				lua_pushstring (L, at + 1);
				lua_settable (L, -3);
			}
		}

		return TRUE;
	}
	return FALSE;
#else
	InternetAddressMailbox *iamb;

	if (ia && INTERNET_ADDRESS_IS_MAILBOX (ia)) {
		lua_newtable (L);
		iamb = INTERNET_ADDRESS_MAILBOX (ia);
		addr = internet_address_mailbox_get_addr (iamb);

		if (addr) {
			rspamd_lua_table_set (L, "name", internet_address_get_name (ia));
			rspamd_lua_table_set (L, "addr", addr);
			/* Set optional fields */

			at = strchr (addr, '@');
			if (at != NULL) {
				lua_pushstring(L, "user");
				lua_pushlstring(L, addr, at - addr);
				lua_settable (L, -3);
				lua_pushstring (L, "domain");
				lua_pushstring (L, at + 1);
				lua_settable (L, -3);
			}
			return TRUE;
		}
	}

	return FALSE;
#endif
}

/*
 * Push internet addresses to lua as a table
 */
void
lua_push_internet_address_list (lua_State *L, InternetAddressList *addrs)
{
	InternetAddress *ia;
	gint idx = 1;

#ifndef GMIME24
	/* Gmime 2.2 version */
	InternetAddressList *cur;

	lua_newtable (L);
	cur = addrs;
	while (cur) {
		ia = internet_address_list_get_address (cur);
		if (lua_push_internet_address (L, ia)) {
			lua_rawseti (L, -2, idx++);
		}
		cur = internet_address_list_next (cur);
	}
#else
	/* Gmime 2.4 version */
	gsize len, i;

	lua_newtable (L);
	if (addrs != NULL) {
		len = internet_address_list_length (addrs);
		for (i = 0; i < len; i++) {
			ia = internet_address_list_get_address (addrs, i);
			if (lua_push_internet_address (L, ia)) {
				lua_rawseti (L, -2, idx++);
			}
		}
	}
#endif
}

/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_files.c
 * @brief Process simple 'users' policy files.
 *
 * @copyright 2000,2006 The FreeRADIUS server project
 * @copyright 2000 Jeff Carneal (jeff@apex.net)
 */
RCSID("$Id$")

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/module_rlm.h>
#include <freeradius-devel/server/pairmove.h>
#include <freeradius-devel/server/users_file.h>
#include <freeradius-devel/util/htrie.h>
#include <freeradius-devel/unlang/call_env.h>
#include <freeradius-devel/unlang/transaction.h>

#include <ctype.h>
#include <fcntl.h>

typedef struct {
	tmpl_t *key;
	fr_type_t	key_data_type;

	char const *common_filename;
	fr_htrie_t *common_htrie;
	PAIR_LIST_LIST *common_def;

	char const *recv_filename;
	fr_htrie_t *recv_htrie;
	PAIR_LIST_LIST *recv_pl;

	char const *auth_filename;
	fr_htrie_t *auth_htrie;
	PAIR_LIST_LIST *auth_pl;

	char const *recv_acct_filename;
	fr_htrie_t *recv_acct_users;
	PAIR_LIST_LIST *recv_acct_pl;

	char const *send_filename;
	fr_htrie_t *send_htrie;
	PAIR_LIST_LIST *send_pl;
} rlm_files_t;

typedef struct {
	fr_value_box_t	key;
} rlm_files_env_t;

static fr_dict_t const *dict_freeradius;
static fr_dict_t const *dict_radius;

extern fr_dict_autoload_t rlm_files_dict[];
fr_dict_autoload_t rlm_files_dict[] = {
	{ .out = &dict_freeradius, .proto = "freeradius" },
	{ .out = &dict_radius, .proto = "radius" },
	{ NULL }
};

static fr_dict_attr_t const *attr_fall_through;
static fr_dict_attr_t const *attr_next_shortest_prefix;

extern fr_dict_attr_autoload_t rlm_files_dict_attr[];
fr_dict_attr_autoload_t rlm_files_dict_attr[] = {
	{ .out = &attr_fall_through, .name = "Fall-Through", .type = FR_TYPE_BOOL, .dict = &dict_freeradius },
	{ .out = &attr_next_shortest_prefix, .name = "Next-Shortest-Prefix", .type = FR_TYPE_BOOL, .dict = &dict_freeradius },

	{ NULL }
};


static const conf_parser_t module_config[] = {
	{ FR_CONF_OFFSET_FLAGS("filename", CONF_FLAG_FILE_INPUT, rlm_files_t, common_filename) },
	{ FR_CONF_OFFSET_FLAGS("recv_filename", CONF_FLAG_FILE_INPUT, rlm_files_t, recv_filename) },
	{ FR_CONF_OFFSET_FLAGS("acct_filename", CONF_FLAG_FILE_INPUT, rlm_files_t, recv_acct_filename) },
	{ FR_CONF_OFFSET_FLAGS("auth_filename", CONF_FLAG_FILE_INPUT, rlm_files_t, auth_filename) },
	{ FR_CONF_OFFSET_FLAGS("send_filename", CONF_FLAG_FILE_INPUT, rlm_files_t, send_filename) },
	{ FR_CONF_OFFSET_FLAGS("key", CONF_FLAG_NOT_EMPTY, rlm_files_t, key), .dflt = "%{%{Stripped-User-Name} || %{User-Name}}", .quote = T_DOUBLE_QUOTED_STRING },
	CONF_PARSER_TERMINATOR
};


static uint32_t pairlist_hash(void const *a)
{
	return fr_value_box_hash(((PAIR_LIST_LIST const *)a)->box);
}

static int8_t pairlist_cmp(void const *a, void const *b)
{
	int ret;

	ret = fr_value_box_cmp(((PAIR_LIST_LIST const *)a)->box, ((PAIR_LIST_LIST const *)b)->box);
	return CMP(ret, 0);
}

static int pairlist_to_key(uint8_t **out, size_t *outlen, void const *a)
{
	return fr_value_box_to_key(out, outlen, ((PAIR_LIST_LIST const *)a)->box);
}

static int getrecv_filename(TALLOC_CTX *ctx, char const *filename, fr_htrie_t **ptree, PAIR_LIST_LIST **pdefault, fr_type_t data_type)
{
	int rcode;
	PAIR_LIST_LIST users;
	PAIR_LIST_LIST search_list;	// Temporary list header used for matching in htrie
	PAIR_LIST *entry, *next;
	PAIR_LIST_LIST *user_list, *default_list;
	fr_htrie_t *tree;
	fr_htrie_type_t htype;
	fr_value_box_t *box;
	map_t		*reply_head;

	if (!filename) {
		*ptree = NULL;
		return 0;
	}

	pairlist_list_init(&users);
	rcode = pairlist_read(ctx, dict_radius, filename, &users);
	if (rcode < 0) {
		return -1;
	}

	htype = fr_htrie_hint(data_type);

	/*
	 *	Walk through the 'users' file list
	 */
	entry = NULL;
	while ((entry = fr_dlist_next(&users.head, entry))) {
		map_t *map = NULL;
		map_t *prev, *next_map;
		fr_dict_attr_t const *da;
		map_t *sub_head, *set_head;

		reply_head = NULL;

		/*
		 *	Do various sanity checks.
		 */
		while ((map = map_list_next(&entry->check, map))) {
			if (!tmpl_is_attr(map->lhs)) {
				ERROR("%s[%d] Left side of check item %s is not an attribute",
				      entry->filename, entry->lineno, map->lhs->name);
				return -1;
			}

			/*
			 *	Disallow regexes for now.
			 */
			if ((map->op == T_OP_REG_EQ) || (map->op == T_OP_REG_NE)) {
				fr_assert(tmpl_is_regex(map->rhs));
			}

			/*
			 *	Move assignment operations to the reply list.
			 */
			switch (map->op) {
			case T_OP_EQ:
			case T_OP_SET:
			case T_OP_ADD_EQ:
				prev = map_list_remove(&entry->check, map);
				map_list_insert_after(&entry->reply, reply_head, map);
				reply_head = map;
				map = prev;
				break;

			default:
				break;
			}
		} /* end of loop over check items */

		/*
		 *	Note that we also re-arrange any control items which are in the reply item list.
		 */
		sub_head = set_head = NULL;

		/*
		 *	Look for server configuration items
		 *	in the reply list.
		 *
		 *	It's a common enough mistake, that it's
		 *	worth doing.
		 */
		for (map = map_list_head(&entry->reply);
		     map != NULL;
		     map = next_map) {
			next_map = map_list_next(&entry->reply, map);
			if (!tmpl_is_attr(map->lhs)) {
				ERROR("%s[%d] Left side of reply item %s is not an attribute",
				      entry->filename, entry->lineno, map->lhs->name);
				return -1;
			}
			da = tmpl_attr_tail_da(map->lhs);

			if (fr_comparison_op[map->op] && (map->op != T_OP_LE) && (map->op != T_OP_GE)) {
				ERROR("%s[%d] Invalid operator reply item %s %s ...",
				      entry->filename, entry->lineno, map->lhs->name, fr_tokens[map->op]);
				return -1;
			}

			/*
			 *	Regex assignments aren't allowed.
			 *
			 *	Execs are being deprecated.
			 */
			if (tmpl_contains_regex(map->rhs) || tmpl_is_exec(map->rhs)) {
				ERROR("%s[%d] Invalid right-hand side of assignment for attribute %s",
				      entry->filename, entry->lineno, da->name);
				return -1;
			}

			if (da == attr_next_shortest_prefix) {
				if (htype != FR_HTRIE_TRIE) {
					ERROR("%s[%d] Cannot use %s when key is not an IP / IP prefix",
					      entry->filename, entry->lineno, da->name);
					return -1;
				}

				if (!tmpl_is_data(map->rhs) || (tmpl_value_type(map->rhs) != FR_TYPE_BOOL)) {
					ERROR("%s[%d] Value for %s must be static boolean",
					      entry->filename, entry->lineno, da->name);
					return -1;
				}

				entry->next_shortest_prefix = tmpl_value(map->rhs)->vb_bool;
				(void) map_list_remove(&entry->reply, map);
				continue;
			}

			/*
			 *	Check for Fall-Through in the reply list.  If so, delete it and set the flag
			 *	in the entry.
			 *
			 *	Note that we don't free "map", as the map functions usually make the "next"
			 *	map be talloc parented from the current one.  So freeing this one will likely
			 *	free all subsequent maps.
			 */
			if (da == attr_fall_through) {
				if (!tmpl_is_data(map->rhs) || (tmpl_value_type(map->rhs) != FR_TYPE_BOOL)) {
					ERROR("%s[%d] Value for %s must be static boolean",
					      entry->filename, entry->lineno, da->name);
					return -1;
				}

				entry->fall_through = tmpl_value(map->rhs)->vb_bool;
				(void) map_list_remove(&entry->reply, map);
				continue;
			}

			/*
			 *	Removals are applied before anything else.
			 */
			if (map->op == T_OP_SUB_EQ) {
				if (sub_head == map) continue;

				(void) map_list_remove(&entry->reply, map);
				map_list_insert_after(&entry->reply, sub_head, map);
				sub_head = map;
				continue;
			}

			/*
			 *	Over-rides are applied after deletions.
			 */
			if (map->op == T_OP_SET) {
				if (set_head == map) continue;

				if (!set_head) set_head = sub_head;

				(void) map_list_remove(&entry->reply, map);
				map_list_insert_after(&entry->reply, set_head, map);
				set_head = map;
				continue;
			}
		}
	}

	tree = fr_htrie_alloc(ctx,  htype, pairlist_hash, pairlist_cmp, pairlist_to_key, NULL);
	if (!tree) {
		pairlist_free(&users);
		return -1;
	}

	default_list = NULL;
	box = fr_value_box_alloc(ctx, data_type, NULL);

	/*
	 *	We've read the entries in linearly, but putting them
	 *	into an indexed data structure would be much faster.
	 *	Let's go fix that now.
	 */
	for (entry = fr_dlist_head(&users.head); entry != NULL; entry = next) {
		/*
		 *	Remove this entry from the input list.
		 */
		next = fr_dlist_next(&users.head, entry);
		fr_dlist_remove(&users.head, entry);

		/*
		 *	@todo - loop over entry->reply, calling
		 *	unlang_fixup_update() or unlang_fixup_filter()
		 *	to double-check the maps.
		 *
		 *	Those functions do normalization and sanity
		 *	checks which are needed if this module is
		 *	going to call an unlang function to *apply*
		 *	the maps.
		 */

		/*
		 *	DEFAULT entries get their own list.
		 */
		if (strcmp(entry->name, "DEFAULT") == 0) {
			if (!default_list) {
				default_list = talloc_zero(ctx, PAIR_LIST_LIST);
				pairlist_list_init(default_list);
				default_list->name = entry->name;

				/*
				 *	Don't insert the DEFAULT list
				 *	into the tree, instead make it
				 *	it's own list.
				 */
				*pdefault = default_list;
			}

			/*
			 *	Append the entry to the DEFAULT list
			 */
			fr_dlist_insert_tail(&default_list->head, entry);
			continue;
		}

		/*
		 *	Not DEFAULT, must be a normal user. First look
		 *	for a matching list header already in the tree.
		 */
		search_list.name = entry->name;
		search_list.box = box;

		/*
		 *	Has to be of the correct data type.
		 */
		if (fr_value_box_from_str(box, box, data_type, NULL,
					  entry->name, strlen(entry->name), NULL, false) < 0) {
			ERROR("%s[%d] Failed parsing key %s - %s",
			      entry->filename, entry->lineno, entry->name, fr_strerror());
			goto error;
		}

		/*
		 *	Find an exact match, especially for patricia tries.
		 */
		user_list = fr_htrie_match(tree, &search_list);
		if (!user_list) {
			user_list = talloc_zero(ctx, PAIR_LIST_LIST);
			pairlist_list_init(user_list);
			user_list->name = entry->name;
			user_list->box = fr_value_box_alloc(user_list, data_type, NULL);

			(void) fr_value_box_copy(user_list, user_list->box, box);

			/*
			 *	Insert the new list header.
			 */
			if (!fr_htrie_insert(tree, user_list)) {
				ERROR("%s[%d] Failed inserting key %s - %s",
				      entry->filename, entry->lineno, entry->name, fr_strerror());
				goto error;

			error:
				fr_value_box_clear_value(box);
				talloc_free(tree);
				return -1;
			}
		}
		fr_value_box_clear_value(box);

		/*
		 *	Append the entry to the user list
		 */
		fr_dlist_insert_tail(&user_list->head, entry);
	}

	*ptree = tree;

	return 0;
}



/*
 *	(Re-)read the "users" file into memory.
 */
static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	rlm_files_t *inst = talloc_get_type_abort(mctx->inst->data, rlm_files_t);

	inst->key_data_type = tmpl_expanded_type(inst->key);
	if (fr_htrie_hint(inst->key_data_type) == FR_HTRIE_INVALID) {
		cf_log_err(mctx->inst->conf, "Invalid data type '%s' for 'files' module.",
			   fr_type_to_str(inst->key_data_type));
		return -1;
	}

#undef READFILE
#define READFILE(_x, _y, _d) if (getrecv_filename(inst, inst->_x, &inst->_y, &inst->_d, inst->key_data_type) != 0) do { ERROR("Failed reading %s", inst->_x); return -1;} while (0)

	READFILE(common_filename, common_htrie, common_def);
	READFILE(recv_filename, recv_htrie, recv_pl);
	READFILE(recv_acct_filename, recv_acct_users, recv_acct_pl);
	READFILE(auth_filename, auth_htrie, auth_pl);
	READFILE(send_filename, send_htrie, send_pl);

	return 0;
}

/*
 *	Common code called by everything below.
 */
static unlang_action_t file_common(rlm_rcode_t *p_result, UNUSED rlm_files_t const *inst, rlm_files_env_t *env,
				   request_t *request, fr_htrie_t *tree, PAIR_LIST_LIST *default_list)
{
	PAIR_LIST_LIST const	*user_list;
	PAIR_LIST const 	*user_pl, *default_pl;
	bool			found = false, trie = false;
	PAIR_LIST_LIST		my_list;
	uint8_t			key_buffer[16], *key;
	size_t			keylen = 0;
	fr_edit_list_t		*el, *child;

	if (!tree && !default_list) RETURN_MODULE_NOOP;

	RDEBUG2("Looking for key \"%pV\"", &env->key);

	el = unlang_interpret_edit_list(request);
	MEM(child = fr_edit_list_alloc(request, 50, el));

	if (tree) {
		my_list.name = NULL;
		my_list.box = &env->key;
		user_list = fr_htrie_find(tree, &my_list);

		trie = (tree->type == FR_HTRIE_TRIE);

		/*
		 *	Convert the value-box to a key for use in a trie.  The trie assumes that the key
		 *	starts at the high bit of the data, and that isn't always the case.  e.g. "bool" and
		 *	"integer" may be in host byte order, in which case we have to convert them to network
		 *	byte order.
		 */
		if (user_list && trie) {
			key = key_buffer;
			keylen = sizeof(key_buffer) * 8;

			(void) fr_value_box_to_key(&key, &keylen, &env->key);

			RDEBUG3("Keylen %ld", keylen);
			RHEXDUMP3(key, (keylen + 7) >> 3, "KEY ");
		}

		user_pl = user_list ? fr_dlist_head(&user_list->head) : NULL;
	} else {
		user_pl = NULL;
		user_list = NULL;
	}

redo:
	default_pl = default_list ? fr_dlist_head(&default_list->head) : NULL;

	/*
	 *	Find the entry for the user.
	 */
	while (user_pl || default_pl) {
		map_t *map = NULL;
		PAIR_LIST const *pl;
		bool match = true;

		/*
		 *	Figure out which entry to match on.
		 */
		if (!default_pl && user_pl) {
			pl = user_pl;

			RDEBUG3("DEFAULT[] USER[%d]=%s", user_pl->lineno, user_pl->name);
			user_pl = fr_dlist_next(&user_list->head, user_pl);

		} else if (!user_pl && default_pl) {
			pl = default_pl;
			RDEBUG3("DEFAULT[%d]= USER[]=", default_pl->lineno);
			default_pl = fr_dlist_next(&default_list->head, default_pl);

		} else if (user_pl->order < default_pl->order) {
			pl = user_pl;

			RDEBUG3("DEFAULT[%d]= USER[%d]=%s (choosing user)", default_pl->lineno, user_pl->lineno, user_pl->name);
			user_pl = fr_dlist_next(&user_list->head, user_pl);

		} else {
			pl = default_pl;
			RDEBUG3("DEFAULT[%d]= USER[%d]=%s (choosing default)", default_pl->lineno, user_pl->lineno, user_pl->name);
			default_pl = fr_dlist_next(&default_list->head, default_pl);
		}

		/*
		 *	Run the check items.
		 */
		while ((map = map_list_next(&pl->check, map))) {
			int rcode;

			RDEBUG3("    %s %s %s", map->lhs->name, fr_tokens[map->op], map->rhs ? map->rhs->name : "{ ... }");

			/*
			 *	Control items get realized to VPs, and
			 *	copied to a temporary list, which is
			 *	then copied to control if the entire
			 *	line matches.
			 */
			switch (map->op) {
			case T_OP_EQ:
			case T_OP_SET:
			case T_OP_ADD_EQ:
				fr_assert(0);
				goto fail;

				/*
				 *	Evaluate the map, including regexes.
				 */
			default:
				rcode = radius_legacy_map_cmp(request, map);
				if (rcode < 0) {
					RPWARN("Failed parsing map for check item %s, skipping it", map->lhs->name);
				fail:
					fr_edit_list_abort(child);
					RETURN_MODULE_FAIL;
				}

				if (!rcode) {
					RDEBUG3("    failed match");
					match = false;
				}
				break;
			}

			if (!match) break;
		}

		if (!match) continue;

		RDEBUG2("Found match \"%s\" on line %d of %s", pl->name, pl->lineno, pl->filename);
		found = true;

		/* ctx may be reply */
		if (radius_legacy_map_list_apply(request, &pl->reply, child) < 0) {
			RPWARN("Failed parsing reply item");
			goto fail;
		}

		if (pl->fall_through) {
			continue;
		}

		/*
		 *	We're not doing patricia tries.  Stop now.
		 */
		if (!trie) {
			break;
		}

		/*
		 *	We're doing patricia tries, but we've been
		 *	told to not walk back up the trie, OR we're at the top of the tree.  Stop.
		 */
		if (!pl->next_shortest_prefix || (keylen == 0)) {
			break;
		}

		/*
		 *	Walk back up the trie looking for shorter prefixes.
		 *
		 *	Note that we've already found an entry, so we
		 *	MUST start with that prefix, otherwise we
		 *	would end up in an loop of finding the same
		 *	prefix over and over.
		 */
		if (keylen > user_list->box->vb_ip.prefix) keylen = user_list->box->vb_ip.prefix;

		do {
			keylen--;
			user_list = fr_trie_lookup_by_key(tree->store, key, keylen);
			if (!user_list) {
				user_pl = NULL;
				continue;
			}

			user_pl = fr_dlist_head(&user_list->head);
			RDEBUG("Found matching shorter subnet %s at key length %ld", user_pl->name, keylen);
			goto redo;
		} while (keylen > 0);
	}

	/*
	 *	See if we succeeded.
	 */
	if (!found) {
		fr_edit_list_abort(child);
		RETURN_MODULE_NOOP; /* on to the next module */
	}

	fr_edit_list_commit(child);
	RETURN_MODULE_OK;
}


/*
 *	Find the named user in the database.  Create the
 *	set of attribute-value pairs to check and reply with
 *	for this user from the database. The main code only
 *	needs to check the password, the rest is done here.
 */
static unlang_action_t CC_HINT(nonnull) mod_authorize(rlm_rcode_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_files_t const *inst = talloc_get_type_abort_const(mctx->inst->data, rlm_files_t);
	rlm_files_env_t *env_data = talloc_get_type_abort(mctx->env_data, rlm_files_env_t);

	return file_common(p_result, inst, env_data, request,
			   inst->recv_htrie ? inst->recv_htrie : inst->common_htrie,
			   inst->recv_htrie ? inst->recv_pl : inst->common_def);
}


/*
 *	Pre-Accounting - read the recv_acct_users file for check_items and
 *	config. Reply items are Not Recommended(TM) in recv_acct_users,
 *	except for Fallthrough, which should work
 */
static unlang_action_t CC_HINT(nonnull) mod_preacct(rlm_rcode_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_files_t const *inst = talloc_get_type_abort_const(mctx->inst->data, rlm_files_t);
	rlm_files_env_t *env_data = talloc_get_type_abort(mctx->env_data, rlm_files_env_t);

	return file_common(p_result, inst, env_data, request,
			   inst->recv_acct_users ? inst->recv_acct_users : inst->common_htrie,
			   inst->recv_acct_users ? inst->recv_acct_pl : inst->common_def);
}

static unlang_action_t CC_HINT(nonnull) mod_authenticate(rlm_rcode_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_files_t const *inst = talloc_get_type_abort_const(mctx->inst->data, rlm_files_t);
	rlm_files_env_t *env_data = talloc_get_type_abort(mctx->env_data, rlm_files_env_t);

	return file_common(p_result, inst, env_data, request,
			   inst->auth_htrie ? inst->auth_htrie : inst->common_htrie,
			   inst->auth_htrie ? inst->auth_pl : inst->common_def);
}

static unlang_action_t CC_HINT(nonnull) mod_post_auth(rlm_rcode_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_files_t const *inst = talloc_get_type_abort_const(mctx->inst->data, rlm_files_t);
	rlm_files_env_t *env_data = talloc_get_type_abort(mctx->env_data, rlm_files_env_t);

	return file_common(p_result, inst, env_data, request,
			   inst->send_htrie ? inst->send_htrie : inst->common_htrie,
			   inst->send_htrie ? inst->send_pl : inst->common_def);
}

/*
 *	@todo - Whilst this causes `key` to be evaluated on a per-call basis,
 *	it is still evaluated during module instantiation to determine the tree type in use
 *	so more restructuring is needed to make the module protocol agnostic.
 *
 *	Or we need to regenerate the tree on every call.
 */
static const call_env_method_t method_env = {
	FR_CALL_ENV_METHOD_OUT(rlm_files_env_t),
	.env = (call_env_parser_t[]){
		{ FR_CALL_ENV_OFFSET("key", FR_TYPE_VOID, CALL_ENV_FLAG_REQUIRED, rlm_files_env_t, key),
				     .pair.dflt = "%{%{Stripped-User-Name} || %{User-Name}}", .pair.dflt_quote = T_DOUBLE_QUOTED_STRING },
		CALL_ENV_TERMINATOR
	},
};

/* globally exported name */
extern module_rlm_t rlm_files;
module_rlm_t rlm_files = {
	.common = {
		.magic		= MODULE_MAGIC_INIT,
		.name		= "files",
		.inst_size	= sizeof(rlm_files_t),
		.config		= module_config,
		.instantiate	= mod_instantiate
	},
	.method_names = (module_method_name_t[]){
		{ .name1 = "recv",		.name2 = "accounting-request",	.method = mod_preacct,
		  .method_env = &method_env	},
		{ .name1 = "recv",		.name2 = CF_IDENT_ANY,		.method = mod_authorize,
		  .method_env = &method_env	},
		{ .name1 = "authenticate",	.name2 = CF_IDENT_ANY,		.method = mod_authenticate,
		  .method_env = &method_env	},
		{ .name1 = "send",		.name2 = CF_IDENT_ANY,		.method = mod_post_auth,
		  .method_env = &method_env	},
		MODULE_NAME_TERMINATOR
	}

};

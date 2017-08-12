#include "module.h"
#include "stb_sb.h"
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>
#include "inso_utils.h"
#include "module_msgs.h"

static void alias_msg      (const char*, const char*, const char*);
static void alias_cmd      (const char*, const char*, const char*, int);
static bool alias_save     (FILE*);
static bool alias_init     (const IRCCoreCtx*);
static void alias_modified (void);
static void alias_quit     (void);
static void alias_mod_msg  (const char*, const IRCModMsg*);

enum { ALIAS_ADD, ALIAS_ADD_GLOBAL, ALIAS_DEL, ALIAS_DEL_GLOBAL, ALIAS_LIST, ALIAS_LIST_GLOBAL, ALIAS_SET_PERM };

const IRCModuleCtx irc_mod_ctx = {
	.name        = "alias",
	.desc        = "Allows defining simple responses to !commands",
	.priority    = -1000,
	.flags       = IRC_MOD_DEFAULT,
	.on_save     = &alias_save,
	.on_modified = &alias_modified,
	.on_msg      = &alias_msg,
	.on_cmd      = &alias_cmd,
	.on_init     = &alias_init,
	.on_quit     = &alias_quit,
	.on_mod_msg  = &alias_mod_msg,
	.commands    = DEFINE_CMDS (
		[ALIAS_ADD]         = CMD1("alias"     ) CMD1("alias+"   ),
		[ALIAS_ADD_GLOBAL]  = CMD1("galias"    ) CMD1("galias+"  ),
		[ALIAS_DEL]         = CMD1("unalias"   ) CMD1("delalias" ) CMD1("rmalias"    ) CMD1("alias-"),
		[ALIAS_DEL_GLOBAL]  = CMD1("gunalias"  ) CMD1("gdelalias") CMD1("grmalias"   ) CMD1("galias-"),
		[ALIAS_LIST]        = CMD1("lsalias"   ) CMD1("lsa"      ) CMD1("listalias"  ) CMD1("listaliases"),
		[ALIAS_LIST_GLOBAL] = CMD1("lsgalias"  ) CMD1("lsga"     ),
		[ALIAS_SET_PERM]    = CMD1("chaliasmod") CMD1("chamod"   ) CMD1("aliasaccess") CMD1("setaliasaccess")
	),
	.cmd_help = DEFINE_CMDS (
		[ALIAS_ADD]         = "<key> <text> | Adds or updates a channel-specific alias named <key>, it can then be recalled with !<key>",
		[ALIAS_ADD_GLOBAL]  = "<key> <text> | Adds or updates a global alias named <key>",
		[ALIAS_DEL]         = "<key> | Removes the channel-specific alias named <key>",
		[ALIAS_DEL_GLOBAL]  = "<key> | Removes the global alias named <key>",
		[ALIAS_LIST]        = "| Shows the aliases available in this channel",
		[ALIAS_LIST_GLOBAL] = "| Shows the aliases available in all channels",
		[ALIAS_SET_PERM]    = "<key> <NORMAL|WLIST|ADMIN> | Sets the permission level required to use the alias idenfitied by <key>"
	),
	.help_url = "https://insobot.handmade.network/forums/t/2393",
};

#define ALIAS_CHAR '!'

static const IRCCoreCtx* ctx;

enum {
	AP_NORMAL = 0,
	AP_WHITELISTED,
	AP_ADMINONLY,

	AP_COUNT,
};

//NOTE: must be uppercase
static const char* alias_permission_strs[] = {
	"NORMAL",
	"WLIST",
	"ADMIN",
};

typedef struct {
	int permission;
	bool me_action;
	char* msg;
	time_t last_use; // should technically be per channel
	char* author;
} Alias;

static char*** alias_keys;
static Alias*  alias_vals;

static void alias_load(){
	int save_format_ver = 0;
	char** keys = NULL;
	Alias val = { .permission = AP_NORMAL };

	FILE* f = fopen(ctx->get_datafile(), "rb");

	if(fscanf(f, "VERSION %d\n", &save_format_ver) == 1){
		if(save_format_ver != 2){
			fprintf(stderr, "Unknown save format version %d! Can't load any aliases.\n", save_format_ver);
			fclose(f);
			return;
		}

		while(!feof(f)){
			char* token;

			while(fscanf(f, "%ms", &token) == 1){
				if(isupper(*token)){
					if(strncmp(token, "AUTHOR:", 7) == 0){
						val.author = strdup(token+7);
						free(token);
						continue;
					}

					int perm_idx = -1;
					for(int i = 0; i < AP_COUNT; ++i){
						if(strcmp(token, alias_permission_strs[i]) == 0){
							perm_idx = i;
							break;
						}
					}

					if(perm_idx >= 0){
						val.permission = perm_idx;
					} else {
						fprintf(stderr, "Unknown permission in file: '%s'\n", token);
					}

					free(token);
					break;
				} else {
					sb_push(keys, token);
				}
			}

			if(keys && fscanf(f, " %m[^\n]", &token) == 1){
				val.msg = token;
				val.me_action = (strstr(token, "/me") == token);
				sb_push(alias_keys, keys);
				sb_push(alias_vals, val);
				fprintf(stderr, "Loaded alias [%s] = [%s]\n", *keys, val.msg);
			}

			keys = NULL;
		}
	} else {
		// This is probably the original format which didn't have the VERSION header, convert it
		char* key;
		while(fscanf(f, "%ms %m[^\n]", &key, &val.msg) == 2){
			fprintf(stderr, "Loaded old style alias [%s] = [%s]\n", key, val.msg);
			sb_push(keys, key);
			sb_push(alias_keys, keys);
			sb_push(alias_vals, val);
			keys = NULL;
		}
	}

	fclose(f);
}

static bool alias_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	alias_load();
	return true;
}

static void alias_quit(void){
	for(size_t i = 0; i < sb_count(alias_keys); ++i){
		for(size_t j = 0; j < sb_count(alias_keys[i]); ++j){
			free(alias_keys[i][j]);
		}
		sb_free(alias_keys[i]);
		free(alias_vals[i].msg);
		free(alias_vals[i].author);
	}
	sb_free(alias_keys);
	sb_free(alias_vals);
}

static void alias_modified(void){
	alias_quit();
	alias_load();
}

static bool alias_valid_1st_char(char c){
	// this needs to exclude atleast irc channel prefixes: # & + ~ . !
	return c == '\\' || isalnum(c);
}

enum { ALIAS_NOT_FOUND = 0, ALIAS_FOUND_CHAN = 1, ALIAS_FOUND_GLOBAL = 2 };

static int alias_find(const char* chan, const char* key, int* idx, int* sub_idx){

	if(chan){
		char full_key[strlen(chan) + strlen(key) + 2];
		snprintf(full_key, sizeof(full_key), "%s,%s", chan, key);

		for(size_t i = 0; i < sb_count(alias_keys); ++i){
			for(size_t j = 0; j < sb_count(alias_keys[i]); ++j){
				if(strcasecmp(full_key, alias_keys[i][j]) == 0){
					if(idx) *idx = i;
					if(sub_idx) *sub_idx = j;
					return ALIAS_FOUND_CHAN;
				}
			}
		}
	}
	
	for(size_t i = 0; i < sb_count(alias_keys); ++i){
		for(size_t j = 0; j < sb_count(alias_keys[i]); ++j){
			if(strcasecmp(key, alias_keys[i][j]) == 0){
				if(idx) *idx = i;
				if(sub_idx) *sub_idx = j;
				return ALIAS_FOUND_GLOBAL;
			}
		}
	}

	return ALIAS_NOT_FOUND;
}

static void alias_add(const char* chan, const char* key, const char* msg, int perm, const char* author){
	Alias* alias;
	int idx;

	int required = chan ? ALIAS_FOUND_CHAN : ALIAS_FOUND_GLOBAL;

	if(alias_find(chan, key, &idx, NULL) == required){
		alias = alias_vals + idx;
		free(alias->msg);
		free(alias->author);
	} else {

		char* full_key;
		if(chan){
			asprintf_check(&full_key, "%s,%s", chan, key);
		} else {
			full_key = strdup(key);
		}

		char** keys = NULL;
		Alias a = {};

		sb_push(keys, full_key);
		sb_push(alias_keys, keys);
		sb_push(alias_vals, a);

		alias = &sb_last(alias_vals);
	}

	alias->msg        = strdup(msg);
	alias->permission = perm;
	alias->me_action  = (strstr(msg, "/me") == msg);
	alias->author     = strdup(author);
}

static void alias_del(int idx, int sub_idx){
	free(alias_keys[idx][sub_idx]);
	sb_erase(alias_keys[idx], sub_idx);

	if(sb_count(alias_keys[idx]) == 0){
		sb_free(alias_keys[idx]);
		sb_erase(alias_keys, idx);

		free(alias_vals[idx].msg);
		sb_erase(alias_vals, idx);
	}
}

static void alias_list(const char* chan, const char* name, int type){
	char alias_buf[512];
	char* alias_ptr = alias_buf;
	size_t alias_sz = sizeof(alias_buf);

	// NOTE: only prints the first key if there are multiple per alias

	for(size_t i = 0; i < sb_count(alias_keys); ++i){
		for(size_t j = 0; j < sb_count(alias_keys[i]); ++j){
			const char* key = alias_keys[i][j];

			if(type == ALIAS_LIST && !alias_valid_1st_char(*key)){
				char* endptr = strchr(key, ',');
				if(endptr && strncmp(chan, key, endptr - key) == 0){
					key = endptr + 1;
					snprintf_chain(&alias_ptr, &alias_sz, "!%s ", key);
					break;
				}
			} else if(type == ALIAS_LIST_GLOBAL && alias_valid_1st_char(*key)){
				snprintf_chain(&alias_ptr, &alias_sz, "!%s ", key);
				break;
			}
		}
	}

	if(alias_ptr == alias_buf){
		strcpy(alias_buf, "(none)");
	}

	if(type == ALIAS_LIST_GLOBAL){
		ctx->send_msg(chan, "%s: Global aliases: %s", inso_dispname(ctx, name), alias_buf);
	} else {
		ctx->send_msg(chan, "%s: Aliases in %s: %s", inso_dispname(ctx, name), chan, alias_buf);
	}
}

static void alias_cmd(const char* chan, const char* name, const char* arg, int cmd){

	bool is_admin = strcasecmp(chan+1, name) == 0 || inso_is_admin(ctx, name); 
	bool is_wlist = is_admin || inso_is_wlist(ctx, name);

	if(!is_wlist) return;

	switch(cmd){
		case ALIAS_ADD: {
			if(!*arg++){
				alias_list(chan, name, ALIAS_LIST);
				break;
			}

			if(!alias_valid_1st_char(*arg)) goto usage_add;

			const char* space = strchr(arg, ' ');
			if(!space) goto usage_add;

			char* key = strndupa(arg, space - arg);
			for(char* k = key; *k; ++k) *k = tolower(*k);

			if(strncmp(space+1, "->", 2) == 0){
				// Aliasing new key to existing command

				char* otherkey;
				if(sscanf(space + 3, " %ms", &otherkey) != 1){
					ctx->send_msg(chan, "%s: Alias it to what, exactly?", name);
					break;
				}

				int idx, sub_idx;
				if(alias_find(chan, key, &idx, &sub_idx) == ALIAS_FOUND_CHAN){
					if(alias_vals[idx].permission == AP_ADMINONLY && !is_admin){
						ctx->send_msg("%s: You don't have permission to change %s.", name, key);
						break;
					} else {
						alias_del(idx, sub_idx);
					}
				}

				int otheridx;
				if(alias_find(chan, otherkey, &otheridx, NULL)){
					char* chan_key;
					asprintf_check(&chan_key, "%s,%s", chan, key);
					sb_push(alias_keys[otheridx], chan_key);
					ctx->send_msg(chan, "%s: Alias %s set.", name, key);
				} else {
					ctx->send_msg(chan, "%s: Can't alias %s as %s is not defined.", name, key, otherkey);
					free(otherkey);
				}
			} else {
				alias_add(chan, key, space+1, AP_NORMAL, name);
				ctx->send_msg(chan, "%s: Alias %s set.", name, key);
			}

		} break;

		case ALIAS_ADD_GLOBAL: {
			//TODO: implement the -> thing for global aliases? should only be able to alias global to global i think
			if(!*arg++){
				alias_list(chan, name, ALIAS_LIST_GLOBAL);
				break;
			}

			if(!alias_valid_1st_char(*arg)) goto usage_add;

			const char* space = strchr(arg, ' ');
			if(!space) goto usage_add;

			char* key = strndupa(arg, space - arg);
			for(char* k = key; *k; ++k) *k = tolower(*k);

			alias_add(NULL, key, space+1, AP_NORMAL, name);
			ctx->send_msg(chan, "%s: Global alias %s set.", name, key);
		} break;

		case ALIAS_DEL: {
			if(!*arg++ || !alias_valid_1st_char(*arg)) goto usage_del;

			int idx, sub_idx;
			int found = alias_find(chan, arg, &idx, &sub_idx);

			if(found == ALIAS_FOUND_CHAN){
				if(alias_vals[idx].permission == AP_ADMINONLY && !is_admin){
					ctx->send_msg("%s: You don't have permission to delete %s.", name, arg);
					break;
				} else {
					alias_del(idx, sub_idx);
					ctx->send_msg(chan, "%s: Removed alias %s.", name, arg);
				}
			} else if(found == ALIAS_FOUND_GLOBAL){
				//TODO: create a blank alias for this channel to disable the global one only here.
				ctx->send_msg(chan, "%s: That's a global alias, poke insofaras to implement hiding them per channel, or use " CONTROL_CHAR "gdelalias to remove it everywhere." , name);
			} else {
				ctx->send_msg(chan, "%s: That alias doesn't exist.", name);
			}
		} break;

		case ALIAS_DEL_GLOBAL: {
			if(!*arg++ || !alias_valid_1st_char(*arg)) goto usage_del;

			int idx, sub_idx;
			if(alias_find(NULL, arg, &idx, &sub_idx)){
				if(alias_vals[idx].permission == AP_ADMINONLY && !is_admin){
					ctx->send_msg("%s: You don't have permission to change %s.", name, arg);
					break;
				} else {
					alias_del(idx, sub_idx);
					ctx->send_msg(chan, "%s: Removed global alias %s.", name, arg);
				}
			} else {
				ctx->send_msg(chan, "%s: That global alias doesn't exist.", name);
			}
		} break;

		case ALIAS_LIST:
		case ALIAS_LIST_GLOBAL: {
			alias_list(chan, name, cmd);
		} break;

		//FIXME: potential issues:
		// * changing permissions of global alias through local alias ptr
		
		case ALIAS_SET_PERM: {
			if(!*arg++ || !alias_valid_1st_char(*arg)) goto usage_setperm;

			const char* space = strchr(arg, ' ');
			if(!space) goto usage_setperm;

			char* key = strndupa(arg, space - arg);
			for(char* k = key; *k; ++k) *k = tolower(*k);

			int idx;
			if(!alias_find(chan, key, &idx, NULL)){
				ctx->send_msg(chan, "%s: No alias called '%s'.", name, key);
				break;
			}

			bool perm_set = false;
			const char* permstr = space+1;
			for(int i = 0; i < AP_COUNT; ++i){
				if(strcasecmp(permstr, alias_permission_strs[i]) == 0){
					if(i == AP_ADMINONLY && !is_admin){
						ctx->send_msg(chan, "%s: You don't have permission to set that permission... Yeah.", name);
					} else {
						alias_vals[idx].permission = i;
						ctx->send_msg(chan, "%s: Set permissions on %s to %s.", name, key, permstr);
						perm_set = true;
					}
					break;
				}
			}

			if(!perm_set){
				ctx->send_msg(chan, "%s: Not sure what permission level '%s' is.", name, permstr);
			}
		} break;
	}

	ctx->save_me();

	return;

usage_add:
	ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "(g)alias <key> <text>", name); return;
usage_del:
	ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "(g)unalias <key>", name); return;
usage_setperm:
	ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "chaliasmod <key> [NORMAL|WLIST|ADMIN]", name); return;
}

static void alias_msg(const char* chan, const char* name, const char* msg){
	if(*msg != ALIAS_CHAR || !alias_valid_1st_char(msg[1])) return;

	const char* key = strndupa(msg+1, strchrnul(msg, ' ') - (msg+1));
	int idx, sub_idx;
	if(!alias_find(chan, key, &idx, &sub_idx)){
		return;
	}

	// if some other module already responded to this !cmd, then don't say anything.
	if(ctx->responded()){
		return;
	}

	const char* arg = msg + strlen(key) + 1;
	while(*arg == ' ') ++arg;

	size_t arg_len = strlen(arg);
	size_t name_len = strlen(name);
	char* msg_buf = NULL;

	Alias* value = alias_vals + idx;

	// don't repeat the same alias too soon
	time_t now = time(0);
	if(now - value->last_use <= 5){
		return;
	}
	value->last_use = now;

	bool has_cmd_perms = (value->permission == AP_NORMAL) || strcasecmp(chan+1, name) == 0;
	if(!has_cmd_perms){
		if (value->permission == AP_WHITELISTED){
			has_cmd_perms = inso_is_wlist(ctx, name);
		} else if (value->permission == AP_ADMINONLY){
			has_cmd_perms = inso_is_admin(ctx, name);
		} else {
			// Some kind of weird unknown permission type. Assume normal access.
			has_cmd_perms = true;
		}
	}
	if(!has_cmd_perms) return;

	char*  urlenc_arg;
	size_t urlenc_arg_len;
	{
		CURL* why_do_i_need_this = curl_easy_init();
		urlenc_arg = curl_easy_escape(why_do_i_need_this, arg, arg_len);
		urlenc_arg_len = urlenc_arg ? strlen(urlenc_arg) : 0;
		curl_easy_cleanup(why_do_i_need_this);
	}

	for(const char* str = value->msg + (value->me_action ? 3 : 0); *str; ++str){
		if(str[0] == '%' && str[1] == 't'){
			memcpy(sb_add(msg_buf, name_len), name, name_len);
			++str;
		} else if(str[0] == '%' && str[1] == 'a'){
			if(*arg){
				memcpy(sb_add(msg_buf, arg_len), arg, arg_len);
			}
			++str;
		} else if(str[0] == '%' && str[1] == 'u'){
			if(urlenc_arg && *urlenc_arg){
				memcpy(sb_add(msg_buf, urlenc_arg_len), urlenc_arg, urlenc_arg_len);
			}
			++str;
		} else if(str[0] == '%' && str[1] == 'n'){
			if(*arg){
				memcpy(sb_add(msg_buf, arg_len), arg, arg_len);
			} else {
				memcpy(sb_add(msg_buf, name_len), name, name_len);
			}
			++str;
		} else {
			sb_push(msg_buf, *str);
		}
	}
	sb_push(msg_buf, 0);

	if(*msg_buf == '.' || *msg_buf == '!' || *msg_buf == '\\' || *msg_buf == '/'){
		*msg_buf = ' ';
	}

	if(value->me_action){
		ctx->send_msg(chan, "\001ACTION %s\001", msg_buf);
	} else {
		ctx->send_msg(chan, "%s", msg_buf);
	}

	sb_free(msg_buf);
	curl_free(urlenc_arg);
}

static bool alias_save(FILE* file){
	fputs("VERSION 2b\n", file);
	for(size_t i = 0; i < sb_count(alias_keys); ++i){
		for(size_t j = 0; j < sb_count(alias_keys[i]); ++j){
			fprintf(file, "%s ", alias_keys[i][j]);
		}

		if(alias_vals[i].author){
			fprintf(file, "AUTHOR:%s ", alias_vals[i].author);
		}

		int perms = alias_vals[i].permission;
		if(perms < 0 || perms >= AP_COUNT) perms = AP_NORMAL;
		fputs(alias_permission_strs[perms], file);

		fprintf(file, " %s\n", alias_vals[i].msg);
	}
	return true;
}

static void alias_mod_msg(const char* sender, const IRCModMsg* msg){

	bool is_info = strcmp(msg->cmd, "alias_info") == 0;

	if(is_info || strcmp(msg->cmd, "alias_exists") == 0){
		const char** arglist = (const char**)msg->arg;
		const char* keys = arglist[0];
		const char* chan = arglist[1];

		const char* prev_p = keys;
		const char* p;

		do {
			p = strchrnul(prev_p, ' ');
			char* key = strndupa(prev_p, p - prev_p);
			prev_p = p+1;

			int idx;
			int result = alias_find(chan, key, &idx, NULL);
			if(result){
				if(is_info){
					Alias* a = alias_vals + idx;
					AliasInfo info = {
						.content = a->msg,
						.author  = a->author,
						.last_used = a->last_use,
						.perms = a->permission,
						.is_action = a->me_action,
					};
					msg->callback((intptr_t)&info, msg->cb_arg);
				} else {
					msg->callback(result, msg->cb_arg);
				}
				break;
			}
		} while(*p);
	} else if(strcmp(msg->cmd, "alias_exec") == 0){
		AliasReq* req = (AliasReq*)msg->arg;

		if(!(req->alias && req->chan && req->user)){
			return;
		}

		size_t len = strlen(req->alias);
		char* buf = alloca(len+2);

		char* p = buf;
		if(*req->alias != ALIAS_CHAR){
			*p++ = ALIAS_CHAR;
		}
		memcpy(p, req->alias, len+1);

		alias_msg(req->chan, req->user, buf);
	}
}

#include <time.h>
#include <string.h>
#include <ctype.h>
#include <yajl/yajl_tree.h>
#include <yajl/yajl_gen.h>
#include "module.h"
#include "module_msgs.h"
#include "stb_sb.h"
#include "inso_utils.h"
#include "inso_gist.h"
#include "inso_tz.h"

static bool sched_init (const IRCCoreCtx*);
static void sched_cmd  (const char*, const char*, const char*, int);
static void sched_tick (time_t);
static void sched_quit (void);
static void sched_mod_msg (const char*, const IRCModMsg*);

enum { SCHED_ADD, SCHED_DEL, SCHED_EDIT, SCHED_SHOW, SCHED_LINK, SCHED_NEXT };

const IRCModuleCtx irc_mod_ctx = {
	.name        = "schedule",
	.desc        = "Stores stream schedules",
	.on_init     = &sched_init,
	.on_cmd      = &sched_cmd,
	.on_tick     = &sched_tick,
	.on_quit     = &sched_quit,
	.on_mod_msg  = &sched_mod_msg,
	.commands    = DEFINE_CMDS (
		[SCHED_ADD]  = CMD("sched+"),
		[SCHED_DEL]  = CMD("sched-"),
		[SCHED_EDIT] = CMD("schedit"),
		[SCHED_SHOW] = CMD("sched") CMD("sched?"),
		[SCHED_LINK] = CMD("schedlist"),
		[SCHED_NEXT] = CMD("next") CMD("snext")
	),
	.cmd_help = DEFINE_CMDS (
		[SCHED_ADD]  = "[#chan] [days] <HH:MM>[-HH:MM][TZ] [Title] | Adds a new schedule. [days] is either a YYYY-MM-DD date, or a comma separated list of 3-letter days. "
		               "[TZ] is a timezone abbreviation like GMT, if given it should be after the HH:MM time without a space inbetween.",
		[SCHED_DEL]  = "[#chan] <ID> | Deletes the schedule identified by <ID> (for [chan] if given)",
		[SCHED_EDIT] = "[#chan] <ID> [days] <HH:MM>[-HH:MM][TZ] [Title] | Edits a schedule, omitted parameters will not change. See sched+ for more info",
		[SCHED_SHOW] = "[#chan] | Shows schedules for the current channel (or [chan] if given)",
		[SCHED_LINK] = "| Shows the URL to the amalgamated schedule webpage",
		[SCHED_NEXT] = "| Shows which stream is scheduled to occur next"
	)
};

static const IRCCoreCtx* ctx;

// FIXME: this representation is pretty awkward
typedef struct {
	time_t start;
	time_t end;
	char* title;
	uint8_t repeat;
} SchedEntry;

typedef struct {
	time_t offset;
	const char* user;
	const SchedEntry* entry;
} SchedOffset;

static char**       sched_keys;
static SchedEntry** sched_vals;
static inso_gist*   gist;

static SchedOffset* sched_offsets;
static time_t       offset_expiry;

enum { MON, TUE, WED, THU, FRI, SAT, SUN, DAYS_IN_WEEK };
static const char* days[] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };

static int get_dow(struct tm* tm){
	return tm->tm_wday ? tm->tm_wday - 1 : SUN;
}

static void sched_free(void){
	for(size_t i = 0; i < sb_count(sched_keys); ++i){
		for(size_t j = 0; j < sb_count(sched_vals[i]); ++j){
			free(sched_vals[i][j].title);
		}
		sb_free(sched_vals[i]);
		free(sched_keys[i]);
	}
	sb_free(sched_keys);
	sb_free(sched_vals);
}

static int sched_get(const char* name){
	size_t i;
	for(i = 0; i < sb_count(sched_keys); ++i){
		if(strcmp(sched_keys[i], name) == 0){
			return i;
		}
	}
	return -1;
}

static int sched_get_add(const char* name){
	int i;
	if((i = sched_get(name)) == -1){
		sb_push(sched_keys, strdup(name));
		sb_push(sched_vals, 0);
		i = sb_count(sched_vals) - 1;
	}
	return i;
}

static int sched_off_cmp(const void* a, const void* b){
	return ((SchedOffset*)a)->offset - ((SchedOffset*)b)->offset;
}

static void sched_offsets_update(void){
	sb_free(sched_offsets);

	struct tm now_tm = {};
	time_t now = time(0);

	gmtime_r(&now, &now_tm);
	now_tm.tm_mday -= get_dow(&now_tm);
	now_tm.tm_hour = now_tm.tm_min = now_tm.tm_sec = 0;

	time_t week_start = timegm(&now_tm);
	now -= week_start;

	for(size_t i = 0; i < sb_count(sched_vals); ++i){
		const char* user = sched_keys[i];
		const SchedEntry* scheds = sched_vals[i];

		for(size_t j = 0; j < sb_count(scheds); ++j){
			const SchedEntry* s = scheds + j;
			time_t t = s->start - week_start;

			if(!s->repeat && now - t < (12*60*60)){
				SchedOffset so = { t, user, s };
				sb_push(sched_offsets, so);
			}

			struct tm date = {};
			gmtime_r(&s->start, &date);
			date.tm_year = now_tm.tm_year;
			date.tm_mon = now_tm.tm_mon;

			for(int k = 0; k < DAYS_IN_WEEK; ++k){
				if(!(s->repeat & (1 << k))) continue;

				date.tm_mday = now_tm.tm_mday + k;
				t = timegm(&date) - week_start;
				SchedOffset so = { t, user, s };
				sb_push(sched_offsets, so);
			}
		}
	}

	offset_expiry = week_start + (7*24*60*60);
	qsort(sched_offsets, sb_count(sched_offsets), sizeof(SchedOffset), &sched_off_cmp);
}

static bool sched_reload(void){
	inso_gist_file* files = NULL;
	int ret = inso_gist_load(gist, &files);

	if(ret == INSO_GIST_304){
		puts("mod_schedule: not modified.");
		return true;
	}

	if(ret != INSO_GIST_OK){
		puts("mod_schedule: gist error.");
		return false;
	}

	puts("mod_schedule: doing full reload");

	char* data = NULL;
	for(inso_gist_file* f = files; f; f = f->next){
		if(strcmp(f->name, "schedule.json") != 0){
			continue;
		} else {
			data = f->content;
			break;
		}
	}

	if(!data){
		puts("mod_schedule: data null?!");
		return false;
	}

	yajl_val root = yajl_tree_parse(data, NULL, 0);
	if(!YAJL_IS_ARRAY(root)){
		puts("mod_schedule: data is not an array?!");
		return false;
	}
	
	sched_free();

	enum { K_USER, K_START, K_END, K_TITLE, K_REPEAT };

	struct {
		const char** path;
		yajl_type type;
	} keys[] = {
		{ (const char*[]){ "user"  , NULL }, yajl_t_string },
		{ (const char*[]){ "start" , NULL }, yajl_t_string },
		{ (const char*[]){ "end"   , NULL }, yajl_t_string },
		{ (const char*[]){ "title" , NULL }, yajl_t_string },
		{ (const char*[]){ "repeat", NULL }, yajl_t_number },
	};

	for(size_t i = 0; i < root->u.array.len; ++i){
		yajl_val vals[5];
		for(size_t j = 0; j < ARRAY_SIZE(vals); ++j){
			vals[j] = yajl_tree_get(root->u.array.values[i], keys[j].path, keys[j].type);
			if(!vals[j]) printf("mod_schedule: parse error %zu/%zu\n", i, j);
		}

		SchedEntry entry = {
			.title  = strdup(vals[K_TITLE]->u.string),
			.repeat = vals[K_REPEAT]->u.number.i,
		};

		struct tm tm = {};

		strptime(vals[K_START]->u.string, "%FT%TZ", &tm);
		entry.start = timegm(&tm);

		strptime(vals[K_END]->u.string, "%FT%TZ", &tm);
		entry.end = timegm(&tm);

		int diff = (entry.end - entry.start) / 60;
		localtime_r(&entry.start, &tm);
		printf("mod_schedule: got entry: [%s] [%02d:%02d] [%dmin] [%x] [%s]\n", vals[0]->u.string, tm.tm_hour, tm.tm_min, diff, entry.repeat, entry.title);

		int index = sched_get_add(vals[0]->u.string);
		sb_push(sched_vals[index], entry);
	}

	yajl_tree_free(root);
	inso_gist_file_free(files);
	sched_offsets_update();

	return true;
}

static bool sched_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	char* gist_id = getenv("INSOBOT_SCHED_GIST_ID");
	if(!gist_id || !*gist_id){
		fputs("mod_schedule: INSOBOT_SCHED_GIST_ID undefined, can't continue.\n", stderr);
		return false;
	}

	char* gist_user = getenv("INSOBOT_GIST_USER");
	if(!gist_user || !*gist_user){
		fputs("mod_schedule: No INSOBOT_GIST_USER env, can't continue.\n", stderr);
		return false;
	}
	
	char* gist_token = getenv("INSOBOT_GIST_TOKEN");
	if(!gist_token || !*gist_token){
		fputs("mod_schedule: No INSOBOT_GIST_TOKEN env, can't continue.\n", stderr);
		return false;
	}

	gist = inso_gist_open(gist_id, gist_user, gist_token);
	return sched_reload();
}

static void sched_upload(void){
	inso_gist_file* file = NULL;

	yajl_gen json = yajl_gen_alloc(NULL);
	yajl_gen_config(json, yajl_gen_beautify, 1);

	char time_buf[256] = {};

	yajl_gen_array_open(json);
	for(size_t i = 0; i < sb_count(sched_keys); ++i){
		for(size_t j = 0; j < sb_count(sched_vals[i]); ++j){
			yajl_gen_map_open(json);

			yajl_gen_string(json, "user", 4);
			yajl_gen_string(json, sched_keys[i], strlen(sched_keys[i]));

			strftime(time_buf, sizeof(time_buf), "%FT%TZ", gmtime(&sched_vals[i][j].start));
			yajl_gen_string(json, "start", 5);
			yajl_gen_string(json, time_buf, strlen(time_buf));

			strftime(time_buf, sizeof(time_buf), "%FT%TZ", gmtime(&sched_vals[i][j].end));
			yajl_gen_string(json, "end", 3);
			yajl_gen_string(json, time_buf, strlen(time_buf));

			yajl_gen_string(json, "title", 5);
			yajl_gen_string(json, sched_vals[i][j].title, strlen(sched_vals[i][j].title));

			yajl_gen_string(json, "repeat", 6);
			yajl_gen_integer(json, sched_vals[i][j].repeat);

			yajl_gen_map_close(json);
		}
	}
	yajl_gen_array_close(json);

	size_t json_len = 0;
	const unsigned char* json_data = NULL;
	yajl_gen_get_buf(json, &json_data, &json_len);
	inso_gist_file_add(&file, "schedule.json", json_data);
	yajl_gen_free(json);

#if 1
	inso_gist_save(gist, "insobot stream schedule", file);
#else
	printf("schedule.json: [%s]\n", file->content);
#endif
	inso_gist_file_free(file);

	sched_offsets_update();
}

static bool sched_parse_user(const char* in, const char* fallback, char* out, size_t out_len){
	bool found = false;

	if(*in == '#'){
		size_t len = strlen(in) - 1;
		if(len < out_len){
			memcpy(out, in+1, len+1);
			found = true;
		}
	} else {
		*stpncpy(out, fallback, out_len-1) = '\0';
	}

	for(size_t i = 0; i < out_len; ++i)
		out[i] = tolower(out[i]);

	return found;
}

static bool sched_parse_id(const char* in, int* id_out, int* day_out){
	char day[4] = {};
	int id, day_id = -1;
	int ret = sscanf(in, "%d%3s", &id, day);

	if(ret == 2){
		for(size_t i = 0; i < ARRAY_SIZE(days); ++i){
			if(strcasecmp(day, days[i]) == 0){
				day_id = i;
				break;
			}
		}
	}

	if(ret == 0 || id < 0){
		return false;
	} else {
		*id_out  = id;
		*day_out = day_id;
		return true;
	}
}

static bool sched_parse_days(const char* _in, struct tm* date, unsigned* day_mask){
	time_t now = time(0);

	char* in = strdupa(_in);

	*day_mask = 0;

	gmtime_r(&now, date);
	date->tm_hour = 0;
	date->tm_min = 0;
	date->tm_sec = 0;

	bool found = false;

	// try specific strings, TODO: it would be nice to have "tomorrow" etc work somehow
	struct {
		const char* text;
		int repeat_val;
	} repeat_tokens[] = {
		{ "today"   , 0    },
		{ "daily"   , 0x7F },
		{ "weekdays", 0x1F },
		{ "weekends", 0x60 },
		{ "weekly"  , 1 << get_dow(date) },
	};

	for(size_t i = 0; i < ARRAY_SIZE(repeat_tokens); ++i){
		if(strcasecmp(in, repeat_tokens[i].text) == 0){
			*day_mask = repeat_tokens[i].repeat_val;
			found = true;
			break;
		}
	}

	// try comma separated days list
	if(!found){
		char* day_state;
		char* day = strtok_r(in, ",", &day_state);
		while(day){
			for(size_t i = 0; i < ARRAY_SIZE(days); ++i){
				if(strcasecmp(day, days[i]) == 0){
					*day_mask |= (1 << i);
					found = true;
					break;
				}
			}
			day = strtok_r(NULL, ",", &day_state);
		}
	}

	// make sure the start date is on one of the repeat days
	if(*day_mask){
		int today = get_dow(date);
		if(!(*day_mask & (1 << today))){
			for(int i = 0; i < 7; ++i){
				if(*day_mask & (1 << i)){
					date->tm_mday += (i - today);
					timegm(date);
					break;
				}
			}
		}
	}

	// return now if found
	if(found) return true;

	// try explicit date
	char* p = strptime(in, "%F", date);
	if(p && p != in && !*p){
		date->tm_hour = 0;
		date->tm_min = 0;
		date->tm_sec = 0;
		timegm(date);
		return true;
	}

	return false;
}

static bool sched_parse_time(const char* in, int* mins_start, int* mins_end, unsigned* day_mask, bool* got_duration){
	int time_pieces[4] = {};
	int read_count[2] = {};

	int time_count = sscanf(
		in,
		"%d:%d%n-%d:%d%n",
		time_pieces + 0,
		time_pieces + 1,
		read_count  + 0,
		time_pieces + 2,
		time_pieces + 3,
		read_count  + 1
	);

	if(time_count < 2){
		return false;
	}

	for(int i = 0; i < 4; ++i){
		if(time_pieces[i] < 0) return false;
	}

	if(time_count == 2){
		time_pieces[2] = time_pieces[0] + 1;
		time_pieces[3] = time_pieces[1];
		if(got_duration) *got_duration = false;
	} else {
		if(got_duration) *got_duration = true;
	}

	*mins_start = (time_pieces[0]*60) + time_pieces[1];
	*mins_end   = (time_pieces[2]*60) + time_pieces[3];

	if(*mins_end < *mins_start){
		*mins_end += (24*60);
	}

	// parse timezone
	if(time_count == 2 || time_count == 4){
		const char* tz_name = in + read_count[time_count >> 2];
		int tz_offset;

		if(tz_name && tz_abbr2off(tz_name, &tz_offset)){
			*mins_start -= tz_offset;
			*mins_end   -= tz_offset;
		}
	}

	// adjust day mask if necessary
	if(*mins_start < 0){
		*day_mask |= ((*day_mask & 1) << 7);
		*day_mask >>= 1;
	} else if(*mins_start > (24*60)){
		*day_mask <<= 1;
		*day_mask |= (*day_mask >> 7);
		*day_mask &= 0x7f;
	}

	return true;
}

static void sched_add(const char* chan, const char* name, const char* _arg){
	if(!*_arg++){
		ctx->send_msg(
			chan,
			"%s: usage: " CONTROL_CHAR "sched+ [#chan] [days] <HH:MM>[-HH:MM][TZ] [Title]. "
			"'days' can be a list like 'mon,tue,fri', strings like 'daily', 'weekends' etc, or a date like '2016-03-14'.",
			name
		);
		return;
	}

	char* arg_state;
	char* arg;
	if(!(arg = strtok_r(strdupa(_arg), " \t", &arg_state))){
		goto fail;
	}

	// parse channel
	char sched_user[128];
	if(sched_parse_user(arg, name, sched_user, sizeof(sched_user))){
		if(!(arg = strtok_r(NULL, " \t", &arg_state))){
			goto fail;
		}
	}

	// parse days
	struct tm date;
	unsigned day_mask;
	if(sched_parse_days(arg, &date, &day_mask)){
		if(!(arg = strtok_r(NULL, " \t", &arg_state))){
			goto fail;
		}
	}

	// parse time
	int start_mins, end_mins;
	if(!sched_parse_time(arg, &start_mins, &end_mins, &day_mask, NULL)){
		goto fail;
	}

	SchedEntry sched = {
		.start  = timegm(&date) + start_mins * 60,
		.end    = timegm(&date) + end_mins   * 60,
		.repeat = day_mask,
	};

	// parse title
	char* title = NULL;
	while((arg = strtok_r(NULL, " \t", &arg_state))){
		if(title){
			sb_push(title, ' ');
		}
		size_t len = strlen(arg);
		memcpy(sb_add(title, len), arg, len);
	}

	if(!title){
		sched.title = strdup("Untitled Stream");
	} else {
		sched.title = strndup(title, sb_count(title));
		sb_free(title);
	}

	// add it
	int index = sched_get_add(sched_user);
	sb_push(sched_vals[index], sched);

	ctx->send_msg(
		chan,
		"Added schedule for \0038%s\017's [\00311%s\017] stream \0038#%zu\017:\00310 " SCHEDULE_URL " \017",
		sched_user,
		sched.title,
		sb_count(sched_vals[index])-1
	);

	sched_upload();
	return;

fail:
	ctx->send_msg(chan, "Unable to parse time.");
}

static void sched_edit(const char* chan, const char* name, const char* _arg){
	if(!*_arg++){
		ctx->send_msg(
			chan,
			"%s: usage: " CONTROL_CHAR "schedit [#chan] <id> [days] [HH:MM[-HH:MM][TZ]] [Title]. "
			"Missing fields will keep their previous value.",
			name
		);
		return;
	}

	char* arg_state;
	char* arg;
	if(!(arg = strtok_r(strdupa(_arg), " \t", &arg_state))){
		ctx->send_msg(chan, "%s: Couldn't parse ID.", name);
		return;
	}

	// parse channel
	char sched_user[128];
	if(sched_parse_user(arg, name, sched_user, sizeof(sched_user))){
		if(!(arg = strtok_r(NULL, " \t", &arg_state))){
			ctx->send_msg(chan, "%s: Couldn't parse ID.", name);
			return;
		}
	}

	// parse id
	SchedEntry* entry = NULL;
	int id, day_id;
	if(!sched_parse_id(arg, &id, &day_id)){
		ctx->send_msg(chan, "%s: Couldn't parse ID.", name);
		return;
	} else {
		if(!(arg = strtok_r(NULL, " \t", &arg_state))){
			ctx->send_msg(chan, "%s: Nothing to edit...", name);
			return;
		}
	}

	// check id validity
	{
		int index = sched_get(sched_user);
		if(index == -1){
			ctx->send_msg(chan, "%s: Couldn't find any schedules by that user.", name);
			return;
		}
		entry = sched_vals[index];
		if(sb_count(entry) <= (size_t)id){
			ctx->send_msg(chan, "%s: %s doesn't have a schedule with id %d.", name, sched_user, id);
			return;
		}
		entry += id;
	}

	// TODO
	if(day_id != -1){
		ctx->send_msg(chan, "%s: Sorry, sub-ids NYI :(", name);
		return;
	}

	enum {
		EDIT_DATE  = (1 << 0),
		EDIT_TIME  = (1 << 1),
		EDIT_TITLE = (1 << 2),
	};
	uint32_t edit_mask = 0;

	// parse days
	struct tm date;
	unsigned day_mask;
	if(sched_parse_days(arg, &date, &day_mask)){
		edit_mask |= EDIT_DATE;
		arg = strtok_r(NULL, " \t", &arg_state);
	}

	// parse time
	int start_mins, end_mins;
	bool got_duration;
	if(arg && sched_parse_time(arg, &start_mins, &end_mins, &day_mask, &got_duration)){
		edit_mask |= EDIT_TIME;
		arg = strtok_r(NULL, " \t", &arg_state);
	}

	// parse title
	char* title = NULL;
	while(arg){
		edit_mask |= EDIT_TITLE;
		if(title){
			sb_push(title, ' ');
		}
		size_t len = strlen(arg);
		memcpy(sb_add(title, len), arg, len);
		arg = strtok_r(NULL, " \t", &arg_state);
	}

	// apply changes
	if(edit_mask & EDIT_DATE){
		if(day_mask){
			entry->repeat = day_mask;
		}
		struct tm old_date, new_date = date;
		int diff = entry->end - entry->start;
		gmtime_r(&entry->start, &old_date);
		new_date.tm_hour = old_date.tm_hour;
		new_date.tm_min  = old_date.tm_min;
		entry->start = timegm(&new_date);
		entry->end = entry->start + diff;
	}

	if(edit_mask & EDIT_TIME){
		int diff;
		if(got_duration){
			diff = (end_mins - start_mins) * 60;
		} else {
			diff = entry->end - entry->start;
		}
		struct tm old_date;
		gmtime_r(&entry->start, &old_date);
		old_date.tm_hour = 0;
		old_date.tm_min  = 0;
		entry->start = timegm(&old_date) + start_mins * 60;
		entry->end = entry->start + diff;
	}

	if(edit_mask & EDIT_TITLE){
		free(entry->title);
		entry->title = strndup(title, sb_count(title));
		sb_free(title);
	}

	sched_upload();

	ctx->send_msg(
		chan, 
		"Updated \0038%s\017's [\00311%s\017] stream schedule \0038#%d\017:\00310 " SCHEDULE_URL " \017",
		sched_user,
		entry->title,
		id
	);
}

static bool sched_del_i(int index, int id){
	free(sched_vals[index][id].title);
	sb_erase(sched_vals[index], id);

	if(sb_count(sched_vals[index]) == 0){
		free(sched_keys[index]);
		sb_free(sched_vals[index]);

		sb_erase(sched_keys, index);
		sb_erase(sched_vals, index);

		return true;
	} else {
		return false;
	}
}

static void sched_del(const char* chan, const char* name, const char* _arg){
	char* state;
	char* arg = strtok_r(strdupa(_arg), " ", &state);

	if(!arg){
		ctx->send_msg(chan, "%s: usage: " CONTROL_CHAR "sched- [#chan] <schedule_id>", name);
		return;
	}

	char sched_user[64];
	if(sched_parse_user(arg, name, sched_user, sizeof(sched_user))){
		arg = strtok_r(NULL, " ", &state);
	}

	int index = sched_get(sched_user);
	if(index == -1){
		ctx->send_msg(chan, "%s: I don't have any schedule info for '%s'", name, sched_user);
		return;
	}

	int id, day_id;
	if(!sched_parse_id(arg, &id, &day_id)){
		ctx->send_msg(chan, "%s: I need an ID to remove", name);
		return;
	}

	// TODO
	if(day_id != -1){
		ctx->send_msg(chan, "%s: Removing individual days NYI :(", name);
		return;
	}

	if((size_t)id >= sb_count(sched_vals[index])){
		ctx->send_msg(chan, "%s: %s has %zu schedules. I can't delete number %d.", name, sched_user, sb_count(sched_vals[index]), id);
		return;
	}

	sched_del_i(index, id);
	sched_upload();

	ctx->send_msg(
		chan,
		"%s: Deleted \0038%s\017's schedule \0038#%d\017:\00310 " SCHEDULE_URL " \017",
		name,
		sched_user,
		id
	);
}

static void sched_show(const char* chan, const char* name, const char* arg){
	if(*arg) ++arg;

	char sched_user[64];
	sched_parse_user(arg, chan+1, sched_user, sizeof(sched_user));

	int index = sched_get(sched_user);
	if(index == -1){
		ctx->send_msg(chan, "%s: No schedules for %s", name, sched_user);
		return;
	}

	char* sched_buf = NULL;
	for(size_t i = 0; i < sb_count(sched_vals[index]); ++i){
		if(sched_buf){
			sb_push(sched_buf, ' ');
		}

		SchedEntry* s = sched_vals[index] + i;
		struct tm date;
		gmtime_r(&s->start, &date);

		char date_str[64];
		switch(s->repeat){
			case 0x7f: strcpy(date_str, "Daily"); break;
			case 0x1f: strcpy(date_str, "Weekdays"); break;
			case 0x60: strcpy(date_str, "Weekends"); break;
			case 0x00: {
				strftime(date_str, sizeof(date_str), "%F", &date);
			} break;

			default: {
				*date_str = 0;
				char* p = date_str;

				for(int i = 0; i < 7; ++i){
					if(s->repeat & (1 << i)){
						if(*date_str) *p++ = ',';
						p = stpcpy(p, days[i]);
					}
				}
			} break;
		}

		char tmp[256];

		int len = snprintf(
			tmp, sizeof(tmp),
			"[%zu: %s • %s • %02d:%02d UTC]",
			i, s->title, date_str, date.tm_hour, date.tm_min
		);

		if(len < 0) break;
		if(len >= isizeof(tmp)){
			len = sizeof(tmp) - 1;
		}
		memcpy(sb_add(sched_buf, len), tmp, len);
	}
	sb_push(sched_buf, 0);

	ctx->send_msg(chan, "%s: %s's schedules: %s", name, sched_user, sched_buf);
	sb_free(sched_buf);
}

// TODO: show live as well?
static void sched_next(const char* chan){
	time_t now = time(0);
	struct tm tmp = {};
	gmtime_r(&now, &tmp);

	tmp.tm_mday -= get_dow(&tmp);
	tmp.tm_hour = tmp.tm_min = tmp.tm_sec = 0;
	time_t base = timegm(&tmp);

	SchedOffset* next = NULL;
	int diff = 0;

	for(SchedOffset* s = sched_offsets; s < sb_end(sched_offsets); ++s){
		if(now < base + s->offset){
			next = s;
			diff = (base + s->offset) - now;
			break;
		}
	}

	if(!next && sb_count(sched_offsets)){
		next = sched_offsets;
		diff = (base + next->offset + (7*24*60*60)) - now;
	}

	if(next){
		int h = (diff / (60*60));
		int m = (diff / 60) % 60;
		int s = (diff % 60);
		ctx->send_msg(
			chan,
			"Next scheduled stream: [%s - %s] in [%02d:%02d:%02d].",
			next->user,
			next->entry->title,
			h, m, s
		);
	}
}

static void sched_cmd(const char* chan, const char* name, const char* arg, int cmd){
	switch(cmd){
		case SCHED_ADD: {
			if(inso_is_wlist(ctx, name)){
				inso_gist_lock(gist);
				sched_reload();
				sched_add(chan, name, arg);
				inso_gist_unlock(gist);
			}
		} break;
		
		case SCHED_DEL: {
			if(inso_is_wlist(ctx, name)){
				inso_gist_lock(gist);
				sched_reload();
				sched_del(chan, name, arg);
				inso_gist_unlock(gist);
			}
		} break;

		case SCHED_EDIT: {
			if(inso_is_wlist(ctx, name)){
				inso_gist_lock(gist);
				sched_reload();
				sched_edit(chan, name, arg);
				inso_gist_unlock(gist);
			}
		} break;

		case SCHED_SHOW: {
			sched_reload();
			sched_show(chan, name, arg);
		} break;

		case SCHED_LINK: {
			ctx->send_msg(chan, "%s: You can view all known schedules here: " SCHEDULE_URL, name);
		} break;

		case SCHED_NEXT: {
			sched_next(chan);
		} break;
	}
}

static void sched_tick(time_t now){
	if(now >= offset_expiry){
		sched_offsets_update();
	}
}

static void sched_quit(void){
	sched_free();
	sb_free(sched_offsets);
	inso_gist_close(gist);
}

static void sched_mod_msg(const char* sender, const IRCModMsg* msg){

	// XXX: we might need to pull from the gist here...

	if(strcmp(msg->cmd, "sched_iter") == 0){
		const char* name = (const char*)msg->arg;
		bool iter_all = true;
		int index = 0;

		if(name){
			iter_all = false;
			if((index = sched_get(name)) == -1){
				return;
			}
		}

		for(; (size_t)index < sb_count(sched_keys); ++index){
			SchedMsg result = {
				.user = sched_keys[index],
			};

			for(size_t i = 0; i < sb_count(sched_vals[index]); ++i){
				SchedEntry* ent = sched_vals[index] + i;
				result.sched_id = i;
				result.start  = ent->start;
				result.end    = ent->end;
				result.title  = ent->title;
				result.repeat = ent->repeat;

				SchedIterCmd cmd = msg->callback((intptr_t)&result, msg->cb_arg);

				// if the callback changed anything, save back those changes.

				ent->start  = result.start;
				ent->end    = result.end;
				ent->repeat = result.repeat;

				if(result.title != ent->title){
					free(ent->title);
					ent->title = (char*)result.title;
				}

				if(cmd & SCHED_ITER_DELETE){
					if(sched_del_i(index, i)){
						--index;
						break;
					}
					--i;
				}

				if(cmd & SCHED_ITER_STOP){
					return;
				}
			}

			if(!iter_all) break;
		}

		return;
	}

	if(strcmp(msg->cmd, "sched_add") == 0){
		SchedMsg* request = (SchedMsg*)msg->arg;
		SchedEntry sched = {};

		if(!request->user || !request->start || !request->end || request->start > request->end){
			if(msg->callback){
				msg->callback(false, msg->cb_arg);
			}
			return;
		}

		const char* title = request->title ?: "Untitled Stream";
		char* user = strdupa(request->user);
		for(char* c = user; *c; ++c) *c = tolower(*c);

		// check if we can merge this with an existing schedule
		int index = sched_get(user);
		if(index != -1){
			struct tm want = {}, have = {};
			gmtime_r(&request->start, &want);

			for(size_t i = 0; i < sb_count(sched_vals[index]); ++i){
				SchedEntry* s = sched_vals[index] + i;
				gmtime_r(&s->start, &have);

				if(strcasecmp(title, s->title) == 0
					&& s->end - s->start == request->end - request->start
					&& want.tm_hour == have.tm_hour
					&& want.tm_min  == have.tm_min
					&& s->repeat){

					s->repeat |= (1 << get_dow(&want));
					return;
				}
			}
		}

		// otherwise, add it.
		sched.start  = request->start;
		sched.end    = request->end;
		sched.repeat = request->repeat & 0x7f;
		sched.title  = strdup(title);

		index = sched_get_add(user);
		sb_push(sched_vals[index], sched);

		return;
	}

	if(strcmp(msg->cmd, "sched_save") == 0){
		sched_upload();
		return;
	}
}

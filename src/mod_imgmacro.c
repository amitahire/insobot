#include "module.h"
#include "stb_sb.h"
#include "inso_utils.h"
#include <yajl/yajl_tree.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <cairo/cairo.h>
#include <ctype.h>
#include <glob.h>

static bool im_init (const IRCCoreCtx*);
static void im_cmd  (const char*, const char*, const char*, int);
static void im_pm   (const char*, const char*);
static bool im_save (FILE*);
static void im_quit (void);
static void im_ipc  (int, const uint8_t*, size_t);

enum { IM_CREATE, IM_SHOW, IM_LIST, IM_AUTO };

const IRCModuleCtx irc_mod_ctx = {
	.name        = "imgmacro",
	.desc        = "Creates image macros / \"memes\"",
	.on_init     = &im_init,
	.on_cmd      = &im_cmd,
	.on_pm       = &im_pm,
	.on_save     = &im_save,
	.on_quit     = &im_quit,
	.on_ipc      = &im_ipc,
	.commands    = DEFINE_CMDS (
		[IM_CREATE] = CMD("newimg")  CMD("mkmeme"),
		[IM_SHOW]   = CMD("img")     CMD("meme"),
		[IM_LIST]   = CMD("lsimg")   CMD("memelist"),
		[IM_AUTO]   = CMD("autoimg") CMD("automeme")
	),
	.cmd_help = DEFINE_CMDS (
		[IM_CREATE] = "<template> <\"top\"> [\"bottom\"] | Generates a new image macro from <template> and the <top> and optional [bottom] text lines.",
		[IM_SHOW]   = "[ID] | Recall the URL for the image macro with the given [ID], or a random one otherwise.",
		[IM_LIST]   = "| This should list image macros, but is unimplemented >_>",
		[IM_AUTO]   = "| Generates a completely random image macro from mod_markov text"
	)
};

static const IRCCoreCtx* ctx;

typedef struct IMEntry_ {
	int id;
	char* url;
	char* text;
	char* del;
	bool from_album;
} IMEntry;

static IMEntry* im_entries;

static const char* imgur_client_id;
static const char* imgur_album_id;
static const char* imgur_album_hash;
static struct curl_slist* imgur_curl_headers;
static CURL* curl;

static char* im_base_dir;

static char* im_get_template(const char* name){
	char dir_buf[PATH_MAX];
	dir_buf[PATH_MAX - 1] = 0;

	strncpy(dir_buf, im_base_dir, sizeof(dir_buf) - 1);

	if(name){
		if(strchr(name, '.')) return NULL;
		if(inso_strcat(dir_buf, sizeof(dir_buf), name) < 0) return NULL;
		if(inso_strcat(dir_buf, sizeof(dir_buf), ".png") < 0) return NULL;

		printf("imgmacro template: [%s]\n", dir_buf);

		struct stat st;
		if(stat(dir_buf, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
	} else {
		inso_strcat(dir_buf, sizeof(dir_buf), "*.png");

		glob_t glob_data;
		if(glob(dir_buf, 0, NULL, &glob_data) != 0 || glob_data.gl_pathc == 0){
			return NULL;
		}
		char* path = glob_data.gl_pathv[rand() % glob_data.gl_pathc];
		strcpy(dir_buf, path);

		globfree(&glob_data);
	}

	return strdup(dir_buf);
}

static char* im_lookup(int id){
	for(IMEntry* i = im_entries; i < sb_end(im_entries); ++i){
		if(i->id == id) return i->url;
	}
	return NULL;
}

static bool im_upload(const uint8_t* png, unsigned int png_len, IMEntry* e){

#if 0
	FILE* f = fopen("debug-image.png", "wb");
	fwrite(png, png_len, 1, f);
	fflush(f);
	fclose(f);
	return false;
#endif

	char title[32];
	snprintf(title, sizeof(title), "%d", e->id);

	struct curl_httppost *form = NULL, *last = NULL;

	curl_formadd(&form, &last,
	             CURLFORM_PTRNAME, "image",
	             CURLFORM_BUFFER, "image.png",
	             CURLFORM_BUFFERPTR, png,
	             CURLFORM_BUFFERLENGTH, png_len,
	             CURLFORM_END);

	curl_formadd(&form, &last,
	             CURLFORM_PTRNAME, "title",
	             CURLFORM_PTRCONTENTS, title,
	             CURLFORM_END);

	curl_formadd(&form, &last,
	             CURLFORM_PTRNAME, "description",
	             CURLFORM_PTRCONTENTS, e->text,
	             CURLFORM_END);

	if(imgur_album_hash){
		curl_formadd(&form, &last,
		             CURLFORM_PTRNAME, "album",
		             CURLFORM_PTRCONTENTS, imgur_album_hash,
		             CURLFORM_END);
	}

	char* data = NULL;
	inso_curl_reset(curl, "https://api.imgur.com/3/image", &data);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, imgur_curl_headers);
	curl_easy_setopt(curl, CURLOPT_HTTPPOST, form);

	long result = inso_curl_perform(curl, &data);
	if(result < 0){
		printf("mod_imgmacro: curl error: %s\n", curl_easy_strerror(-result));
	}

	curl_formfree(form);

	static const char* id_path[]  = { "data", "id", NULL };
	static const char* del_path[] = { "data", "deletehash", NULL };

	yajl_val root = yajl_tree_parse(data, NULL, 0);
	yajl_val id   = yajl_tree_get(root, id_path, yajl_t_string);
	yajl_val del  = yajl_tree_get(root, del_path, yajl_t_string);

	if(result == 200 && root && id && del){
		printf("DELETE HASH: [%s] = [%s]\n", id->u.string, del->u.string);
		asprintf_check(&e->url, "https://i.imgur.com/%s.png", id->u.string);
		e->del = strdup(del->u.string);
		ctx->save_me();
	} else {
		printf("mod_imgmacro: root/id/del null\n");
		result = 0;
	}

	yajl_tree_free(root);
	sb_free(data);

	return result == 200;
}

static cairo_status_t im_png_write(void* arg, const uint8_t* data, unsigned int data_len){
	char** out = arg;
	memcpy(sb_add(*out, data_len), data, data_len);
	return CAIRO_STATUS_SUCCESS;
}

enum { IM_TEXT_TOP, IM_TEXT_BOTTOM };

static void im_draw_text(cairo_t* cairo, double w, double h, const char* text, int where){
	if(!text) return;

	cairo_save(cairo);

	cairo_text_extents_t te;
	cairo_text_extents(cairo, text, &te);

	cairo_translate(cairo, w / 2.0, h / 2.0);

	double scale = te.width > w ? w / te.width : 1.0;
	scale *= 0.95;

	if(scale < 0.1) scale = 0.1;

	cairo_scale(cairo, scale, scale);

	double offset = h / (2.1 * scale);

	if(where == IM_TEXT_TOP){
		offset = -offset - te.y_bearing / 2.0;
	} else {
		offset = offset + te.y_bearing / 2.0;
	}

	cairo_move_to(
		cairo,
		-(te.width / 2 + te.x_bearing),
		-(te.height / 2 + te.y_bearing) + offset
	);

	cairo_text_path(cairo, text);
	cairo_set_source_rgb(cairo, 1, 1, 1);
	cairo_fill_preserve(cairo);
	cairo_set_source_rgb(cairo, 0, 0, 0);
	cairo_stroke(cairo);

	cairo_restore(cairo);
}

static IMEntry* im_create(const char* template, const char* top, const char* bot){

	// TODO: check if one already exists first, cmp top / bot with IMEntry.text

	cairo_surface_t* img = cairo_image_surface_create_from_png(template);
	cairo_t* cairo = cairo_create(img);

	double img_w = cairo_image_surface_get_width(img);
	double img_h = cairo_image_surface_get_height(img);

	cairo_select_font_face(cairo, "Impact", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

	double font_sz = img_w < img_h ? img_w / 8.0 : img_h / 8.0;
	cairo_set_font_size(cairo, font_sz);

	cairo_set_line_width(cairo, font_sz / 24.0);
	cairo_set_line_cap  (cairo, CAIRO_LINE_CAP_SQUARE);
	cairo_set_line_join (cairo, CAIRO_LINE_JOIN_BEVEL);

	im_draw_text(cairo, img_w, img_h, top, IM_TEXT_TOP);
	im_draw_text(cairo, img_w, img_h, bot, IM_TEXT_BOTTOM);

	cairo_surface_flush(img);

	int next_id = sb_count(im_entries) ? sb_last(im_entries).id + 1 : 0;
	size_t text_len = strlen(top) + 4;
	if(bot) text_len += strlen(bot);

	char* full_text = malloc(text_len);
	strcpy(full_text, top);
	strcat(full_text, " / ");
	if(bot) strcat(full_text, bot);

	for(char* c = full_text; *c; ++c) *c = toupper(*c);

	IMEntry e = {
		.id = next_id,
		.text = full_text,
	};

	char* png_data = NULL;
	cairo_surface_write_to_png_stream(img, &im_png_write, &png_data);

	bool ok = im_upload(png_data, sb_count(png_data), &e);

	cairo_destroy(cairo);
	cairo_surface_destroy(img);

	if(ok){
		ctx->send_ipc(0, "update", 7);
		sb_push(im_entries, e);
		ctx->save_me();
	} else {
		free(full_text);
	}

	sb_free(png_data);

	return ok ? &sb_last(im_entries) : NULL;
}

static bool im_find_url(const char* url){
	for(IMEntry* e = im_entries; e < sb_end(im_entries); ++e){
		if(strcmp(e->url, url) == 0) return true;
	}
	return false;
}

static void im_load_album(void){

	if(!imgur_album_id){
		return;
	}

	char url[256];
	snprintf(url, sizeof(url), "https://api.imgur.com/3/album/%s/images", imgur_album_id);

	char* data = NULL;
	inso_curl_reset(curl, url, &data);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, imgur_curl_headers);
	inso_curl_perform(curl, &data);

	static const char* data_path[]  = { "data", NULL };
	static const char* id_path[]    = { "id", NULL };
	static const char* title_path[] = { "title", NULL };
	static const char* desc_path[]  = { "description", NULL };

	yajl_val root = yajl_tree_parse(data, NULL, 0);
	yajl_val imgs = yajl_tree_get(root, data_path, yajl_t_array);

	if(root && imgs){
		for(size_t i = 0; i < imgs->u.array.len; ++i){
			yajl_val key = imgs->u.array.values[i];

			yajl_val img_id    = yajl_tree_get(key, id_path   , yajl_t_string);
			yajl_val img_title = yajl_tree_get(key, title_path, yajl_t_string);
			yajl_val img_desc  = yajl_tree_get(key, desc_path , yajl_t_string); 
			if(!img_id || !img_title || !img_desc) continue;

			snprintf(url, sizeof(url), "https://i.imgur.com/%s.png", img_id->u.string);

			// don't add dups
			if(im_find_url(url)){
				continue;
			}

			IMEntry e = {
				.id = atoi(img_title->u.string),
				.url  = strdup(url),
				.text = strdup(img_desc->u.string),
				.del  = strdup("???"),
			};

			sb_push(im_entries, e);
		}
	}

	ctx->save_me();

	yajl_tree_free(root);
	sb_free(data);
}

static bool im_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	imgur_client_id  = getenv("INSOBOT_IMGUR_CLIENT_ID");     // Required: imgur api key / client id.
	imgur_album_id   = getenv("INSOBOT_IMGMACRO_ALBUM_ID");   // Optional: Album ID to retrieve images from.
	imgur_album_hash = getenv("INSOBOT_IMGMACRO_ALBUM_HASH"); // Optional: Album hash, to add new images to.

	if(!imgur_client_id){
		puts("mod_imgmacro: No imgur client id, init failed.");
		return false;
	}

	// curl setup
	char header_buf[256];
	snprintf(header_buf, sizeof(header_buf), "Authorization: Client-ID %s", imgur_client_id);
	imgur_curl_headers = curl_slist_append(NULL, header_buf);
	curl = curl_easy_init();

	// template dir setup
	const char* data_dir = getenv("XDG_DATA_HOME");

	struct stat st;
	if(!data_dir|| stat(data_dir, &st) != 0 || !S_ISDIR(st.st_mode)){
		data_dir = getenv("HOME");
		assert(data_dir);

		asprintf_check(&im_base_dir, "%s/.local/share/insobot/imgmacro/", data_dir);
	} else {
		asprintf_check(&im_base_dir, "%s/insobot/imgmacro/", data_dir);
	}
	inso_mkdir_p(im_base_dir);

	// load entries from data file
	IMEntry e;
	FILE* f = fopen(ctx->get_datafile(), "r");
	while(fscanf(f, "%d %ms %ms %m[^\n]", &e.id, &e.url, &e.del, &e.text) == 4){
		sb_push(im_entries, e);
	}
	fclose(f);

	// load more entries from album, if set
	im_load_album();

	return true;
}

static intptr_t imgmacro_markov_cb(intptr_t result, intptr_t arg){
	if(result && !*(char*)arg){
		*(char**)arg = (char*)result;
	} else if(result){
		free((char*)result);
	}
	return 0;
}

static void im_cmd(const char* chan, const char* name, const char* arg, int cmd){
	if(!inso_is_wlist(ctx, name)) return;

	switch(cmd){
		case IM_CREATE: {
			char template[64], txt_top[128], txt_bot[128];

			int i = sscanf(arg, " %63s \"%127[^\"]\" \"%127[^\"]\"", template, txt_top, txt_bot);

			if(i < 2){
				ctx->send_msg(chan, "%s: Usage: mkmeme <img> <\"top text\"> [\"bottom text\"]", name);
				break;
			}

			for(char* c = template; *c; ++c) *c = tolower(*c);
			for(char* c = txt_top; *c; ++c) *c = toupper(*c);
			for(char* c = txt_bot; *c; ++c) *c = toupper(*c);

			char* img_name = im_get_template(template);
			if(!img_name){
				ctx->send_msg(chan, "%s: Unknown template image", name);
				break;
			}

			char* maybe_bot = i == 3 ? txt_bot : NULL;

			IMEntry* e = im_create(img_name, txt_top, maybe_bot);
			if(e){
				ctx->send_msg(chan, "%s Meme %d: %s", name, e->id, e->url);
			} else {
				ctx->send_msg(chan, "Error creating image");
			}

			free(img_name);
		} break;

		case IM_SHOW: {
			int id;
			char* link = NULL;

			if(sscanf(arg, " %d", &id) != 1){
				int total = sb_count(im_entries);
				if(total == 0){
					ctx->send_msg(chan, "%s: None here :(", name);
					break;
				} else {
					link = im_entries[rand() % total].url;
				}
			} else {
				link = im_lookup(id);
			}

			if(link){
				ctx->send_msg(chan, "%s: %s", name, link);
			} else {
				ctx->send_msg(chan, "%s: Unknown id.", name);
			}
		} break;

		case IM_LIST: {

		} break;

		case IM_AUTO: {
			char* markov_text = NULL;
			MOD_MSG(ctx, "markov_gen", 0, &imgmacro_markov_cb, &markov_text);
			if(!markov_text) break;

			size_t word_count = 1;
			for(char* c = markov_text; *c; ++c){
				if(*c == ' ') word_count++;
				*c = toupper(*c);
			}
			word_count = INSO_MIN(word_count, 12u);

			size_t half_count = word_count / 2;
			char *txt_top = markov_text, *txt_bot = NULL;

			for(char* c = markov_text; *c; ++c){
				if(*c == ' ' && --half_count <= 0){
					*c = '\0';
					txt_bot = c+1;
					break;
				}
			}

			// give the bottom text a bit more than half to maybe finish sentences.
			half_count = (word_count*3) / 2;
			if(txt_bot){
				for(char* c = txt_bot; *c; ++c){
					if(*c == ' ' && --half_count <= 0){
						*c = '\0';
						break;
					}
				}
			}

			char* img_name = im_get_template(NULL);
			IMEntry* e = im_create(img_name, txt_top, txt_bot);
			if(e){
				ctx->send_msg(chan, "%s Meme %d: %s", name, e->id, e->url);
			} else {
				ctx->send_msg(chan, "Error creating image");
			}

			free(img_name);
			free(markov_text);
		} break;
	}
}

static void im_pm(const char* name, const char* msg){
	int len = inso_match_cmd(msg, irc_mod_ctx.commands[IM_CREATE], true);
	if(len > 0){
		im_cmd(name, name, msg + len, IM_CREATE);
	}
}

static bool im_save(FILE* f){
	for(IMEntry* i = im_entries; i < sb_end(im_entries); ++i){
		fprintf(f, "%d %s %s %s\n", i->id, i->url, i->del, i->text);
	}
	return true;
}

static void im_quit(void){
	for(IMEntry* i = im_entries; i < sb_end(im_entries); ++i){
		free(i->url);
		free(i->text);
		free(i->del);
	}
	sb_free(im_entries);

	free(im_base_dir);
	curl_slist_free_all(imgur_curl_headers);
	curl_easy_cleanup(curl);
	cairo_debug_reset_static_data();
}

static void im_ipc(int sender_id, const uint8_t* data, size_t data_len){
	if(data_len == 7 && memcmp(data, "update", 7) == 0){
		im_load_album();
	}
}

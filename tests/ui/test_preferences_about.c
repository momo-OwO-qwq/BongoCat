#include "preferences_state.h"
#include "preferences_import_internal.h"
#include "preferences_render_internal.h"
#include "ui_catime.h"
#include "runtime.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"
#include "about/preferences_about_internal.h"
#include "ui_paint_cache.h"
#include "ui_paint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#include <sys/utime.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif


#include "test.h"

void about_disk_cache(BongoCatApp *app) {
    char root[BONGO_CAT_PATH_CAP], directory[BONGO_CAT_PATH_CAP], path[BONGO_CAT_PATH_CAP];
    CHECK(bongo_cat_path_join(root, sizeof(root), app->cache_root, "about-cache-test"));
    CHECK(bongo_cat_path_join(directory, sizeof(directory), root, "about"));
    CHECK(bongo_cat_path_create_directory(directory));
    CHECK(bongo_cat_path_join(path, sizeof(path), directory, "wechat-v1.svg"));
    /* Larger than the network's initial buffer, but well below the QR limit. */
    char svg[9000];
    snprintf(svg, sizeof(svg),
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"240\" height=\"240\">"
        "<!--%08000d--><rect width=\"240\" height=\"240\" fill=\"white\"/></svg>", 0);
    size_t svg_size = strlen(svg);
    Uint32 event_type = SDL_RegisterEvents(1);
    for (int stale = 0; stale < 2; stale++) {
        FILE *file = bongo_cat_file_open(path, "wb");
        CHECK(file != NULL);
        if (!file) return;
        CHECK(fwrite(svg, 1, svg_size, file) == svg_size);
        CHECK(fflush(file) == 0);
        if (stale) {
            time_t old = time(NULL) - 2 * 86400;
#ifdef _WIN32
            struct __utimbuf64 times = {old, old};
            CHECK(_futime64(_fileno(file), &times) == 0);
#else
            struct timespec times[2] = {{old, 0}, {old, 0}};
            CHECK(futimens(fileno(file), times) == 0);
#endif
        }
        CHECK(fclose(file) == 0);
        SDL_PathInfo before = {0}, after = {0};
        CHECK(SDL_GetPathInfo(path, &before));
        BongoCatAboutRequest *job = bongo_cat_about_request(BONGO_ABOUT_WECHAT,
            event_type, 0, root, false);
        CHECK(job != NULL);
        if (!job) return;
        SDL_WaitThread(job->thread, NULL);
        job->thread = NULL;
        CHECK(SDL_GetAtomicInt(&job->done));
        CHECK(job->status == 200 && job->qr_pixels);
        CHECK(job->refresh_needed == (stale != 0));
        CHECK(!job->response && !job->capacity);
        bongo_cat_about_request_free(job);
        CHECK(SDL_GetPathInfo(path, &after));
        CHECK(before.modify_time == after.modify_time);
        file = bongo_cat_file_open(path, "rb");
        CHECK(file != NULL);
        if (file) {
            char bytes[sizeof(svg)] = {0};
            CHECK(fread(bytes, 1, svg_size, file) == svg_size);
            CHECK(strcmp(bytes, svg) == 0);
            CHECK(fclose(file) == 0);
        }
        SDL_FlushEvent(event_type);
    }
    CHECK(bongo_cat_file_remove(path));
    CHECK(bongo_cat_path_remove(directory));
    CHECK(bongo_cat_path_remove(root));
}

void about_refresh_failure(BongoCatApp *app) {
    BongoCatPreferences *value = calloc(1, sizeof(*value));
    CHECK(value != NULL);
    if (!value) return;
    value->app = app;
    value->visible = true;
    value->about.event_type = SDL_RegisterEvents(1);
    value->about.qr_attempted = value->about.contributors_attempted = true;
    value->about.qr_pixels = calloc(240 * 240, 4);
    value->about.contributors = calloc(1, sizeof(*value->about.contributors));
    unsigned char *pixels = value->about.qr_pixels;
    BongoCatAboutFeed *feed = value->about.contributors;
    CHECK(pixels && feed);
    for (int cancelled = 0; cancelled < 2; cancelled++) {
        BongoCatAboutRequest *job = calloc(1, sizeof(*job));
        CHECK(job != NULL);
        if (!job) break;
        job->kind = BONGO_ABOUT_WECHAT;
        /* A failed refresh and a cancelled successful refresh must both
           preserve the old cache without automatically retrying. */
        if (cancelled) {
            job->status = 200;
            job->qr_pixels = calloc(240 * 240, 4);
            job->refresh_needed = true;
            SDL_SetAtomicInt(&job->cancel, 1);
        }
        SDL_SetAtomicInt(&job->done, 1);
        value->about.qr_request = job;
        bongo_cat_about_refresh(value);
        CHECK(value->about.qr_pixels == pixels && value->about.contributors == feed);
        CHECK(!value->about.qr_request && !value->about.contributors_request);
        CHECK(!value->about.qr_texture_dirty);
    }
    bongo_cat_about_clear(value, false);
    free(value);
}

void about_unchanged_contributors(BongoCatApp *app) {
    BongoCatPreferences *value = calloc(1, sizeof(*value));
    CHECK(value != NULL);
    if (!value) return;
    value->app = app;
    value->visible = true;
    value->about.event_type = SDL_RegisterEvents(1);
    value->about.qr_attempted = value->about.contributors_attempted = true;
    for (int update = 0; update < 4; update++) {
        BongoCatAboutRequest *job = calloc(1, sizeof(*job));
        CHECK(job != NULL);
        if (!job) break;
        job->feed = calloc(1, sizeof(*job->feed));
        CHECK(job->feed != NULL);
        if (!job->feed) { free(job); break; }
        job->feed->count = 1;
        job->feed->people[0].pixels = calloc(BONGO_ABOUT_AVATAR_SIZE * BONGO_ABOUT_AVATAR_SIZE, 4);
        CHECK(job->feed->people[0].pixels != NULL);
        if (!job->feed->people[0].pixels) { bongo_cat_about_request_free(job); break; }
        if (update >= 2) job->feed->people[0].pixels[0] = 255;
        if (update == 3) strcpy(job->feed->people[0].name, "Renamed contributor");
        job->kind = BONGO_ABOUT_CONTRIBUTORS;
        job->status = 200;
        SDL_SetAtomicInt(&job->done, 1);
        BongoCatAboutFeed *previous = value->about.contributors;
        value->about.contributors_request = job;
        value->about.portraits_loaded = true;
        value->about.portraits_next = 1;
        bongo_cat_about_refresh(value);
        CHECK(!value->about.contributors_request);
        if (update == 1) {
            CHECK(value->about.contributors == previous);
            CHECK(value->about.portraits_loaded && value->about.portraits_next == 1);
        } else {
            CHECK(value->about.contributors != previous);
            CHECK(!value->about.portraits_loaded && value->about.portraits_next == 0);
        }
    }
    bongo_cat_about_clear(value, false);
    free(value);
}

void hidden_about_completion(BongoCatApp *app) {
    BongoCatPreferences *value = calloc(1, sizeof(*value));
    CHECK(value != NULL);
    if (!value) return;
    value->app = app;
    BongoCatAboutRequest *job = calloc(1, sizeof(*job));
    CHECK(job != NULL);
    if (job) {
        job->kind = BONGO_ABOUT_WECHAT;
        job->qr_pixels = calloc(240 * 240, 4);
        job->status = 200;
        SDL_SetAtomicInt(&job->cancel, 1);
        SDL_SetAtomicInt(&job->done, 1);
        value->about.qr_request = job;
        /* No event or settings window: the normal loop must still reap it. */
        bongo_cat_preferences_update(value);
        CHECK(!value->about.qr_request && !value->about.contributors_request);
        CHECK(!value->about.qr_pixels && !value->about.contributors);
        CHECK(!value->about.qr_attempted && !value->about.contributors_attempted);
    }
    bongo_cat_about_shutdown(value);
    free(value);
}

void about_session_cache(BongoCatPreferences *value) {
    BongoCatAboutState *s = &value->about;
    CHECK(s->contributors_attempted || (s->contributors_request &&
        SDL_GetAtomicInt(&s->contributors_request->cancel)));
    CHECK(s->qr_attempted || (s->qr_request && SDL_GetAtomicInt(&s->qr_request->cancel)));
    BongoCatAboutRequest *contributors_request = s->contributors_request;
    BongoCatAboutRequest *qr_request = s->qr_request;
    /* A second show only raises the already open window. */
    bongo_cat_preferences_show(value);
    CHECK(s->contributors_request == contributors_request);
    CHECK(s->qr_request == qr_request);
    if (!s->contributors) s->contributors = calloc(1, sizeof(*s->contributors));
    if (!s->qr_pixels) s->qr_pixels = calloc(240 * 240, 4);
    BongoCatAboutFeed *feed = s->contributors;
    unsigned char *pixels = s->qr_pixels;
    CHECK(feed && pixels);
    s->qr_open = true;
    SDL_GL_MakeCurrent(value->window, value->gl_context);
    bongo_cat_preferences_page_cache_clear(value, 3, 0);
    bongo_cat_preferences_page_cache_clear(value, 0, 3);
    CHECK(!s->qr_open);
    CHECK(s->contributors == feed && s->qr_pixels == pixels);
    CHECK(s->contributors_request == contributors_request && s->qr_request == qr_request);
    /* A DPI/theme asset rebuild must retain downloaded data and attempt flags. */
    bool qr_attempted = s->qr_attempted;
    bongo_cat_about_assets_clear(value, true);
    CHECK(s->qr_attempted == qr_attempted);
    CHECK(s->contributors == feed && s->qr_pixels == pixels);
    SDL_GL_MakeCurrent(value->app->window, value->app->gl_context);
}

static float original_paragraph_height(const char *text,
    const struct nk_user_font *font, float width, float leading) {
    float height = 0;
    while (*text) {
        int length = 0, remaining = (int)strlen(text);
        while (length < remaining) {
            nk_rune rune;
            int bytes = nk_utf_decode(text + length, &rune, remaining - length);
            if (bytes <= 0) break;
            float next = font->width(font->userdata, font->height, text, length + bytes);
            if (length && next > width) break;
            length += bytes;
            if (rune == '\n') break;
        }
        if (!length) break;
        height += leading;
        text += length;
        while (*text == ' ' || *text == '\n') text++;
    }
    return height;
}

void about_render_cost_regressions(BongoCatPreferences *value) {
    const char *texts[] = {"Open source & community contributions",
        "Every step of BongoCat comes from open source. Thank you to all our contributors.",
        "\xe6\x84\x9f\xe8\xb0\xa2\xe6\x89\x80\xe6\x9c\x89\xe8\xb4\xa1\xe7\x8c\xae\xe8\x80\x85 BongoCat",
        "First line\n  Second line", "", "A"};
    const struct nk_user_font *font = value->ui.caption_font;
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++)
        for (int width = 1; width <= 400; width += 3) {
            float actual = bongo_cat_about_paragraph(NULL, nk_rect(0, 0, (float)width, 0),
                texts[i], font, nk_rgb(0, 0, 0), false, 24);
            CHECK(actual == original_paragraph_height(texts[i], font, (float)width, 24));
        }
    SDL_GL_MakeCurrent(value->window, value->gl_context);
    BongoCatUIPaintKey key = {BONGO_CAT_UI_PAINT_SHADOW, 8, 8, 4, 2, 0, 0, 0};
    unsigned char pixels[64] = {0};
    bongo_cat_ui_paint_cache_begin_frame(&value->ui);
    BongoCatUIPaintTexture *item = bongo_cat_ui_paint_cache_get(&value->ui, &key);
    CHECK(item && bongo_cat_ui_paint_cache_upload(item, pixels, true));
    /* One unused frame and idle trimming must not discard a small hover effect. */
    bongo_cat_ui_paint_cache_begin_frame(&value->ui);
    bongo_cat_ui_paint_cache_begin_frame(&value->ui);
    bongo_cat_ui_trim_idle(&value->ui);
    item = bongo_cat_ui_paint_cache_get(&value->ui, &key);
    CHECK(bongo_cat_ui_paint_cache_ready(item));
    CHECK(bongo_cat_ui_paint_cache_usage(&value->ui, NULL) <= 4u * 1024u * 1024u);
    bongo_cat_ui_paint_destroy(&value->ui);
    CHECK(bongo_cat_ui_paint_cache_usage(&value->ui, NULL) == 0);
    SDL_GL_MakeCurrent(value->app->window, value->app->gl_context);
}

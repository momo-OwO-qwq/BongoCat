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

int bongo_cat_test_failures;

/* Implemented in test_preferences_about.c; linked into the same test. */
void about_disk_cache(BongoCatApp *app);
void about_refresh_failure(BongoCatApp *app);
void about_unchanged_contributors(BongoCatApp *app);
void hidden_about_completion(BongoCatApp *app);
void about_session_cache(BongoCatPreferences *value);
void about_render_cost_regressions(BongoCatPreferences *value);

static void busy(BongoCatImportDialog *dialog, bool open, bool importing) {
    SDL_LockMutex(dialog->mutex);
    dialog->open = open;
    dialog->busy = importing;
    SDL_UnlockMutex(dialog->mutex);
}

static void shared_context_cleanup(BongoCatPreferences *value) {
    BongoCatUIBackend ui = {0};
    ui.gl = value->ui.gl;
    BongoCatGL gl = ui.gl;
    PFNGLISVERTEXARRAYPROC is_vao =
        (PFNGLISVERTEXARRAYPROC)SDL_GL_GetProcAddress("glIsVertexArray");
    PFNGLISBUFFERPROC is_buffer =
        (PFNGLISBUFFERPROC)SDL_GL_GetProcAddress("glIsBuffer");
    PFNGLISPROGRAMPROC is_program =
        (PFNGLISPROGRAMPROC)SDL_GL_GetProcAddress("glIsProgram");
    CHECK(is_vao && is_buffer && is_program);
    if (!is_vao || !is_buffer || !is_program) return;
    CHECK(SDL_GL_MakeCurrent(value->window, value->gl_context));
    CHECK(nk_init_default(&ui.context, NULL));
    nk_buffer_init_default(&ui.commands);
    glGenTextures(1, &ui.font_texture);
    glBindTexture(GL_TEXTURE_2D, ui.font_texture);
    gl.gen_buffers(1, &ui.vbo);
    gl.bind_buffer(GL_ARRAY_BUFFER, ui.vbo);
    ui.program = gl.create_program();
    GLuint texture = ui.font_texture, buffer = ui.vbo, program = ui.program;
    glFlush();
    CHECK(SDL_GL_MakeCurrent(value->app->window, value->app->gl_context));
    GLint previous_vao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_vao);
    GLuint main_vao = 0;
    gl.gen_vertex_arrays(1, &main_vao);
    gl.bind_vertex_array(main_vao);
    /* Context-local VAO names can collide. The fallback must leave the main
       context's object alive while deleting the shared textures/buffers. */
    ui.vao = main_vao;
    CHECK(glIsTexture(texture) && is_buffer(buffer) && is_program(program));
    bongo_cat_ui_destroy_shared(&ui);
    CHECK(!glIsTexture(texture) && !is_buffer(buffer) && !is_program(program));
    CHECK(is_vao(main_vao));
    gl.bind_vertex_array((GLuint)previous_vao);
    gl.delete_vertex_arrays(1, &main_vao);
}

static void asset_retry_keeps_hidpi(BongoCatPreferences *value) {
    CHECK(SDL_GL_MakeCurrent(value->window, value->gl_context));
    struct nk_context context;
    bool initialized = nk_init_default(&context, value->ui.body_font);
    CHECK(initialized);
    if (!initialized) return;
    nk_begin(&context, "asset-retry", nk_rect(0, 0, 128, 128), 0);
    struct nk_command_buffer *canvas = nk_window_get_canvas(&context);
    bongo_cat_preferences_icon_draw(value, canvas, 0,
        nk_rect(0, 0, 32, 32), nk_rgb(255, 255, 255));
    GLuint texture = value->icon_texture_hidpi;
    CHECK(texture != 0);
    bongo_cat_preferences_assets_load(value);
    bongo_cat_preferences_icon_draw(value, canvas, 0,
        nk_rect(0, 0, 32, 32), nk_rgb(255, 255, 255));
    CHECK(value->icon_texture_hidpi == texture);
    CHECK(glIsTexture(texture));
    if (texture != value->icon_texture_hidpi) glDeleteTextures(1, &texture);
    nk_end(&context);
    nk_free(&context);
    CHECK(SDL_GL_MakeCurrent(value->app->window, value->app->gl_context));
}

static void close_over_scrolled_models(BongoCatPreferences *value) {
    if (bongo_cat_ui_native_chrome()) return;
    struct nk_context *context = &value->ui.context;
    value->page = 1;
    value->page_seen = true;
    value->last_page = 1;
    value->page_transition_ns = 0;
    for (int scroll = 0; scroll <= 800; scroll += 40) {
        nk_input_begin(context);
        nk_input_button(context, NK_BUTTON_LEFT, 746, 36, nk_false);
        nk_input_end(context);
        bongo_cat_preferences_draw_frame(value, 800, 500, false);
        nk_clear(context);
        value->scroll_ready[1] = true;
        value->scroll_current[1] = (float)scroll;
        value->scroll_target[1] = (float)scroll;
        nk_input_begin(context);
        nk_input_motion(context, 746, 36);
        nk_input_button(context, NK_BUTTON_LEFT, 746, 36, nk_true);
        nk_input_button(context, NK_BUTTON_LEFT, 746, 36, nk_false);
        nk_input_end(context);
        CHECK(bongo_cat_preferences_draw_frame(value, 800, 500, false));
        CHECK(!value->model_selection_pending);
        value->model_selection_pending = false;
        nk_clear(context);
    }
}

static void inactive_model_behaviors(BongoCatPreferences *value) {
    BongoCatApp *app = value->app;
    char active[BONGO_CAT_ID_CAP], loaded[BONGO_CAT_ID_CAP];
    snprintf(active, sizeof(active), "%s", app->session.active_model_id);
    snprintf(loaded, sizeof(loaded), "%s", app->loaded_model);
    BongoCatLive2D *live2d = app->live2d;
    const BongoCatModelEntry *model = NULL;
    for (size_t i = 0; i < app->models.count; ++i)
        if (strcmp(app->models.entries[i].id, loaded)) {
            model = &app->models.entries[i];
            break;
        }
    CHECK(model != NULL);
    if (!model) return;
    bongo_cat_preferences_behavior_dialog_open_model(value, model);
    CHECK(value->behavior_dialog);
    CHECK(!strcmp(value->behavior_model_id, model->id));
    CHECK(!bongo_cat_preferences_behavior_model_loaded(value));
    CHECK(!value->model_selection_pending);
    CHECK(!strcmp(active, app->session.active_model_id));
    CHECK(!strcmp(loaded, app->loaded_model));
    CHECK(live2d == app->live2d);
    const BongoCatBehaviorCatalog *catalog =
        bongo_cat_preferences_behavior_catalog(value);
    CHECK(catalog->count > 0);
    for (size_t i = 0; i < catalog->count; ++i)
        CHECK(!strncmp(catalog->entries[i].id, model->id, strlen(model->id)));
    bongo_cat_preferences_behavior_dialog_close(value);
}

static void shortcut_chord_capture(BongoCatPreferences *value) {
    static const struct { SDL_Keycode key; const char *binding; } punctuation[] = {
        {SDLK_EQUALS, "Alt+="}, {SDLK_MINUS, "Alt+-"},
        {SDLK_LEFTBRACKET, "Alt+BracketLeft"}, {SDLK_RIGHTBRACKET, "Alt+BracketRight"},
        {SDLK_BACKSLASH, "Alt+Backslash"}, {SDLK_SEMICOLON, "Alt+Semicolon"},
        {SDLK_APOSTROPHE, "Alt+Quote"}, {SDLK_COMMA, "Alt+Comma"},
        {SDLK_PERIOD, "Alt+Period"}, {SDLK_SLASH, "Alt+Slash"},
        {SDLK_GRAVE, "Alt+BackQuote"}, {SDLK_KP_PLUS, "Alt+KpPlus"},
        {SDLK_KP_MINUS, "Alt+KpMinus"}, {SDLK_KP_MULTIPLY, "Alt+KpMultiply"},
        {SDLK_KP_DIVIDE, "Alt+KpDivide"}, {SDLK_KP_DECIMAL, "Alt+KpDecimal"}
    };
    for (size_t i = 0; i < sizeof(punctuation) / sizeof(punctuation[0]); ++i) {
        char recorded[BONGO_CAT_SHORTCUT_CAP] = "";
        bongo_cat_preferences_shortcut_begin(value, "test-punctuation", recorded, sizeof(recorded));
        SDL_Event input = {0};
        input.type = SDL_EVENT_KEY_DOWN;
        input.key.down = true;
        input.key.key = punctuation[i].key;
        input.key.mod = SDL_KMOD_ALT;
        CHECK(bongo_cat_preferences_shortcut_event(value, &input));
        CHECK(!strcmp(recorded, punctuation[i].binding));
        bongo_cat_preferences_shortcut_cancel(value);
        CHECK(!recorded[0]);
    }
    char target[BONGO_CAT_SHORTCUT_CAP] = "";
    bongo_cat_preferences_shortcut_begin(value, "test-chord", target, sizeof(target));
    SDL_Event event = {0};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.down = true;
    event.key.key = SDLK_F23;
    event.key.mod = SDL_KMOD_CTRL;
    CHECK(bongo_cat_preferences_shortcut_event(value, &event));
    CHECK(!strcmp(target, "Control+F23"));
    event.key.key = SDLK_F24;
    CHECK(bongo_cat_preferences_shortcut_event(value, &event));
    CHECK(!strcmp(target, "Control+F23+F24"));
    event.key.repeat = true;
    CHECK(bongo_cat_preferences_shortcut_event(value, &event));
    CHECK(!strcmp(target, "Control+F23+F24"));
    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    event.key.repeat = false;
    event.key.key = SDLK_F23;
    CHECK(bongo_cat_preferences_shortcut_event(value, &event));
    CHECK(!value->shortcut_recording);
    CHECK(!strcmp(target, "Control+F23+F24"));
    bongo_cat_preferences_shortcut_begin(value, "test-chord", target, sizeof(target));
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.down = true;
    event.key.mod = 0;
    event.key.key = SDLK_F23;
    CHECK(bongo_cat_preferences_shortcut_event(value, &event));
    event.key.key = SDLK_LCTRL;
    CHECK(bongo_cat_preferences_shortcut_event(value, &event));
    CHECK(!strcmp(target, "F23+Control"));
    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    CHECK(bongo_cat_preferences_shortcut_event(value, &event));
    CHECK(!value->shortcut_recording);
    CHECK(!strcmp(target, "F23+Control"));
    char small[5] = "F1";
    bongo_cat_preferences_shortcut_begin(value, "test-small", small, sizeof(small));
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.down = true;
    event.key.mod = 0;
    event.key.key = SDLK_F23;
    CHECK(bongo_cat_preferences_shortcut_event(value, &event));
    event.key.key = SDLK_F24;
    CHECK(bongo_cat_preferences_shortcut_event(value, &event));
    CHECK(!value->shortcut_recording);
    CHECK(!strcmp(small, "F1"));
}

int main(int argc, char **argv) {
    BongoCatApp *app = calloc(1, sizeof(*app));
    BongoCatError error = {0};
    if (!app) return 1;
    if (!bongo_cat_app_initialize(app, argc, argv, &error)) {
        fprintf(stderr, "Initialization failed: %s\n", error.message);
        bongo_cat_app_shutdown(app, "test:failed", 1);
        free(app);
        return 1;
    }
    BongoCatPreferences *value = app->preferences;
    about_disk_cache(app);
    about_refresh_failure(app);
    about_unchanged_contributors(app);
    hidden_about_completion(app);
    CHECK(value != NULL);
    if (value) {
        shortcut_chord_capture(value);
        for (int cycle = 0; cycle < 3; ++cycle) {
            bongo_cat_preferences_show(value);
            CHECK(value->window && value->gl_context && value->ui_initialized);
            if (!cycle) {
                about_render_cost_regressions(value);
                shared_context_cleanup(value);
                asset_retry_keeps_hidpi(value);
            }
            about_session_cache(value);
            if (!cycle) inactive_model_behaviors(value);
            bongo_cat_preferences_close(value);
            CHECK(!value->behavior_catalog);
            CHECK(!value->about.contributors && !value->about.qr_pixels);
            CHECK(!value->about.contributors_attempted && !value->about.qr_attempted);
            CHECK(!value->about.contributors_request ||
                SDL_GetAtomicInt(&value->about.contributors_request->cancel));
            CHECK(!value->about.qr_request ||
                SDL_GetAtomicInt(&value->about.qr_request->cancel));
            CHECK(!value->window && !value->gl_context && !value->ui_initialized);
            CHECK(SDL_GL_GetCurrentContext() == app->gl_context);
        }
        bongo_cat_preferences_show(value);
        SDL_Window *window = value->window;
        busy(value->import_dialog, true, false);
        bongo_cat_preferences_close(value);
        bongo_cat_preferences_render(value);
        CHECK(value->window == window && !value->visible);
        CHECK(!value->about.contributors_attempted && !value->about.qr_attempted);
        busy(value->import_dialog, false, true);
        bongo_cat_preferences_render(value);
        CHECK(value->window == window);
        bongo_cat_preferences_show(value);
        CHECK(value->window == window && value->visible);
        about_session_cache(value);
        busy(value->import_dialog, false, false);
        bongo_cat_preferences_render(value);
        CHECK(value->window == window && value->visible);
        busy(value->import_dialog, false, true);
        bongo_cat_preferences_close(value);
        CHECK(value->window == window && !value->visible);
        busy(value->import_dialog, false, false);
        bongo_cat_preferences_render(value);
        CHECK(!value->window && !value->gl_context && !value->ui_initialized);
        bongo_cat_preferences_show(value);
        CHECK(value->visible && value->ui_initialized);
        close_over_scrolled_models(value);
        bongo_cat_preferences_close(value);
    }
    bongo_cat_app_shutdown(app, "test:complete", bongo_cat_test_failures != 0);
    free(app);
    printf("Preferences lifecycle: %d failures\n", bongo_cat_test_failures);
    return bongo_cat_test_failures != 0;
}

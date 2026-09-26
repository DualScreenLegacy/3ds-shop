#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <curl/curl.h>
#include <jansson.h>

#define SERVER_URL "http://IPHERE:5000"
#define MAX_ITEMS 64
#define MAX_PATH_LEN 256
#define JSON_BUF_SIZE (256 * 1024)
#define CHUNK_BUFFER_SIZE (128 * 1024)

typedef struct {
    char name[128];
    char path[MAX_PATH_LEN];
    char type[16];
    double size_mb;
    u64 title_id;
} ShopItem;

typedef struct {
    Handle handle;
    u64 offset;
    u32 total_bytes;
    u64 last_time;
    u64 last_bytes;
    double speed_kbs;
    u8 *buffer;
    size_t buf_fill;
} InstallContext;

static ShopItem g_items[MAX_ITEMS];
static int g_item_count = 0;
static int g_selected_index = 0;
static char g_current_path[MAX_PATH_LEN] = "";
static size_t g_json_size = 0;
static u32 *g_soc_buffer = NULL;

static size_t JsonWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    char *dest = (char *)userp;
    if (g_json_size + total >= JSON_BUF_SIZE - 1) {
        return 0;
    }
    memcpy(dest + g_json_size, contents, total);
    g_json_size += total;
    dest[g_json_size] = '\0';
    return total;
}

static size_t flush_buffer(InstallContext *ctx) {
    if (ctx->buf_fill == 0) return 0;

    u32 written = 0;
    Result res = FSFILE_Write(ctx->handle, &written, ctx->offset, (u32*)ctx->buffer, ctx->buf_fill, FS_WRITE_FLUSH);
    if (R_FAILED(res)) {
        printf("\nWrite error: 0x%08lX", res);
        return 0;
    }

    ctx->offset += written;
    ctx->total_bytes += written;
    ctx->buf_fill = 0;
    return written;
}

static size_t CiaWriteCallback(void *ptr, size_t size, size_t nmemb, void *stream) {
    InstallContext *ctx = (InstallContext *)stream;
    size_t total = size * nmemb;
    size_t processed = 0;

    while (processed < total) {
        size_t to_copy = total - processed;
        size_t space_left = CHUNK_BUFFER_SIZE - ctx->buf_fill;

        if (to_copy > space_left) {
            to_copy = space_left;
        }

        memcpy(ctx->buffer + ctx->buf_fill, (u8*)ptr + processed, to_copy);
        ctx->buf_fill += to_copy;
        processed += to_copy;

        if (ctx->buf_fill == CHUNK_BUFFER_SIZE) {
            if (flush_buffer(ctx) == 0) {
                return 0;
            }
        }
    }

    return total;
}

static int XferInfoCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal;
    (void)ulnow;

    InstallContext *ctx = (InstallContext *)clientp;
    if (dltotal <= 0) return 0;

    u64 current_time = osGetTime();
    u64 time_delta = current_time - ctx->last_time;

    if (time_delta >= 500) {
        double bytes_delta = (double)(dlnow - ctx->last_bytes);
        ctx->speed_kbs = (bytes_delta / 1024.0) / ((double)time_delta / 1000.0);
        ctx->last_bytes = dlnow;
        ctx->last_time = current_time;

        double progress = ((double)dlnow / (double)dltotal) * 100.0;
        double current_mb = (double)dlnow / (1024.0 * 1024.0);
        double total_mb = (double)dltotal / (1024.0 * 1024.0);

        const int bar_width = 24;
        int filled = (int)((progress / 100.0) * bar_width);

        printf("\x1b[5;1H");
        printf("[");
        for (int i = 0; i < bar_width; i++) {
            if (i < filled) printf("=");
            else if (i == filled) printf(">");
            else printf(" ");
        }
        printf("] %5.1f%%\n\n", progress);

        printf("Downloaded: %6.1f / %.1f MB\n", current_mb, total_mb);
        if (ctx->speed_kbs >= 1024.0) {
            printf("Speed:      %6.2f MB/s       \n", ctx->speed_kbs / 1024.0);
        } else {
            printf("Speed:      %6.1f KB/s       \n", ctx->speed_kbs);
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
    }

    return 0;
}

static bool is_title_installed(u64 title_id) {
    if (title_id == 0) return false;
    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(MEDIATYPE_SD, &count)) || count == 0) return false;

    u64 *titles = malloc(count * sizeof(u64));
    if (!titles) return false;

    bool found = false;
    if (R_SUCCEEDED(AM_GetTitleList(&count, MEDIATYPE_SD, count, titles))) {
        for (u32 i = 0; i < count; i++) {
            if (titles[i] == title_id) {
                found = true;
                break;
            }
        }
    }
    free(titles);
    return found;
}

static void purge_title(u64 title_id) {
    if (title_id == 0) return;

    if (is_title_installed(title_id)) {
        printf("\nExisting title found. Removing...");
        AM_DeleteTitle(MEDIATYPE_SD, title_id);
        AM_DeleteTicket(title_id);
    }

    AM_DeletePendingTitle(MEDIATYPE_SD, title_id);
    svcSleepThread(200000000ULL);
}

void delete_selected_title(u64 title_id) {
    if (title_id == 0) {
        printf("\nNo Title ID found for this file.");
        return;
    }

    printf("\nDeleting Title ID %016llX...", title_id);
    purge_title(title_id);
    printf("\nPurge completed.");
}

void install_cia(const char *rel_path, u64 title_id) {
    CURL *curl = curl_easy_init();
    if (!curl) return;

    char *escaped = curl_easy_escape(curl, rel_path, 0);
    char url[512];
    snprintf(url, sizeof(url), "%s/download?path=%s", SERVER_URL, escaped ? escaped : "");
    if (escaped) curl_free(escaped);

    if (title_id != 0 && is_title_installed(title_id)) {
        purge_title(title_id);
    }

    InstallContext ctx;
    ctx.offset = 0;
    ctx.total_bytes = 0;
    ctx.last_time = osGetTime();
    ctx.last_bytes = 0;
    ctx.speed_kbs = 0.0;
    ctx.buf_fill = 0;
    ctx.buffer = (u8*)memalign(0x1000, CHUNK_BUFFER_SIZE);
    if (!ctx.buffer) {
        printf("\nFailed to allocate install buffer!");
        curl_easy_cleanup(curl);
        return;
    }

    Result res = AM_StartCiaInstall(MEDIATYPE_SD, &ctx.handle);
    if (R_FAILED(res)) {
        printf("\nStartCiaInstall Failed: 0x%08lX", res);
        free(ctx.buffer);
        curl_easy_cleanup(curl);
        return;
    }

    printf("\x1b[2J\x1b[H");
    printf("Installing Title...\n");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CiaWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferInfoCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)&ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);

    CURLcode cres = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (cres == CURLE_OK) {
        if (ctx.buf_fill > 0) {
            flush_buffer(&ctx);
        }
        res = AM_FinishCiaInstall(ctx.handle);
        if (R_SUCCEEDED(res)) {
            printf("\n\nInstall Complete! Title registered.");
        } else {
            printf("\n\nFinish error: 0x%08lX", res);
        }
    } else {
        AM_CancelCIAInstall(ctx.handle);
        printf("\n\nAborted (cURL %d)", cres);
    }

    if (ctx.buffer) free(ctx.buffer);
}

void fetch_directory(const char *path) {
    g_item_count = 0;
    g_selected_index = 0;
    g_json_size = 0;
    memset(g_items, 0, sizeof(g_items));

    CURL *curl = curl_easy_init();
    if (!curl) return;

    char *escaped = curl_easy_escape(curl, path ? path : "", 0);
    char url[512];
    snprintf(url, sizeof(url), "%s/api/browse?path=%s", SERVER_URL, escaped ? escaped : "");
    if (escaped) curl_free(escaped);

    char *raw_json = (char *)malloc(JSON_BUF_SIZE);
    if (!raw_json) {
        curl_easy_cleanup(curl);
        return;
    }
    memset(raw_json, 0, JSON_BUF_SIZE);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, JsonWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)raw_json);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200 || g_json_size == 0) {
        printf("\nBrowse failed: res=%d HTTP=%ld size=%u", res, http_code, (unsigned int)g_json_size);
        free(raw_json);
        return;
    }

    raw_json[g_json_size] = '\0';

    json_error_t err;
    json_t *root = json_loads(raw_json, JSON_DISABLE_EOF_CHECK, &err);
    if (!root) {
        printf("\nJSON error L%d: %s", err.line, err.text);
        free(raw_json);
        return;
    }

    if (json_is_object(root)) {
        json_t *arr = json_object_get(root, "items");
        if (arr && json_is_array(arr)) {
            size_t idx;
            json_t *val;

            json_array_foreach(arr, idx, val) {
                if (g_item_count >= MAX_ITEMS) break;
                if (!json_is_object(val)) continue;

                json_t *j_name = json_object_get(val, "name");
                json_t *j_type = json_object_get(val, "type");
                json_t *j_path = json_object_get(val, "path");
                json_t *j_size = json_object_get(val, "size_mb");
                json_t *j_tid  = json_object_get(val, "title_id");

                const char *name = (j_name && json_is_string(j_name)) ? json_string_value(j_name) : "Unnamed";
                const char *type = (j_type && json_is_string(j_type)) ? json_string_value(j_type) : "file";
                const char *p    = (j_path && json_is_string(j_path)) ? json_string_value(j_path) : "";
                const char *tid  = (j_tid && json_is_string(j_tid))   ? json_string_value(j_tid)  : "0";

                strncpy(g_items[g_item_count].name, name, sizeof(g_items[g_item_count].name) - 1);
                strncpy(g_items[g_item_count].type, type, sizeof(g_items[g_item_count].type) - 1);
                strncpy(g_items[g_item_count].path, p, sizeof(g_items[g_item_count].path) - 1);

                g_items[g_item_count].title_id = strtoull(tid, NULL, 16);

                if (j_size && json_is_number(j_size)) {
                    g_items[g_item_count].size_mb = json_number_value(j_size);
                } else {
                    g_items[g_item_count].size_mb = 0.0;
                }

                g_item_count++;
            }
        }
    }

    json_decref(root);
    free(raw_json);
}

void render_ui() {
    printf("\x1b[2J\x1b[H");
    printf("3DS Local Shop Client\n");
    printf("Path: /%s\n", g_current_path);
    printf("----------------------------------------\n");

    if (g_item_count == 0) {
        printf("  (Directory is empty or offline)\n");
    } else {
        for (int i = 0; i < g_item_count; i++) {
            char prefix = (i == g_selected_index) ? '>' : ' ';
            if (strcmp(g_items[i].type, "directory") == 0) {
                printf("%c [%s]/\n", prefix, g_items[i].name);
            } else {
                printf("%c %s (%.1f MB)\n", prefix, g_items[i].name, g_items[i].size_mb);
            }
        }
    }

    printf("\n----------------------------------------\n");
    printf("[A] Install/Update | [X] Delete Title\n");
    printf("[B] Up             | [START] Exit\n");
}

void navigate_up() {
    if (strlen(g_current_path) == 0) return;
    char *last_slash = strrchr(g_current_path, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        g_current_path[0] = '\0';
    }
    fetch_directory(g_current_path);
}

int main(int argc, char **argv) {
    gfxInitDefault();
    PrintConsole topScreen, bottomScreen;
    consoleInit(GFX_TOP, &topScreen);
    consoleInit(GFX_BOTTOM, &bottomScreen);

    consoleSelect(&topScreen);
    printf("Starting shop...\n");
    gfxFlushBuffers();
    gfxSwapBuffers();

    g_soc_buffer = (u32*)memalign(0x1000, 0x100000);
    if (g_soc_buffer) {
        socInit(g_soc_buffer, 0x100000);
    }

    curl_global_init(CURL_GLOBAL_ALL);

    Result amRes = amInit();
    if (R_FAILED(amRes)) {
        consoleSelect(&bottomScreen);
        printf("Warning: amInit failed: 0x%08lX\n", amRes);
        printf("Launch via Title Takeover (Download Play)!\n");
    }

    fetch_directory("");
    render_ui();

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_START) break;

        if (kDown & KEY_DOWN) {
            if (g_selected_index < g_item_count - 1) {
                g_selected_index++;
                render_ui();
            }
        }

        if (kDown & KEY_UP) {
            if (g_selected_index > 0) {
                g_selected_index--;
                render_ui();
            }
        }

        if (kDown & KEY_B) {
            navigate_up();
            render_ui();
        }

        if (kDown & KEY_X && g_item_count > 0) {
            if (strcmp(g_items[g_selected_index].type, "file") == 0) {
                u64 tid = g_items[g_selected_index].title_id;
                consoleSelect(&bottomScreen);
                printf("\x1b[2J\x1b[H");
                delete_selected_title(tid);
                consoleSelect(&topScreen);
                render_ui();
            }
        }

        if (kDown & KEY_A && g_item_count > 0) {
            char target_path[MAX_PATH_LEN];
            char target_type[16];
            u64 target_tid = g_items[g_selected_index].title_id;

            snprintf(target_path, sizeof(target_path), "%s", g_items[g_selected_index].path);
            snprintf(target_type, sizeof(target_type), "%s", g_items[g_selected_index].type);

            if (strcmp(target_type, "directory") == 0) {
                snprintf(g_current_path, sizeof(g_current_path), "%s", target_path);
                fetch_directory(g_current_path);
                render_ui();
            } else {
                consoleSelect(&bottomScreen);
                install_cia(target_path, target_tid);
                consoleSelect(&topScreen);
                render_ui();
            }
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    amExit();
    curl_global_cleanup();
    socExit();
    if (g_soc_buffer) free(g_soc_buffer);
    gfxExit();
    return 0;
}

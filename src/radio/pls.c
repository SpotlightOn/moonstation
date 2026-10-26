#include "pls.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pls";

// A file name plus the directory. FAT long names reach 255 bytes, and the
// array is heap allocated once per scan rather than living in .bss.
#define PLS_PATH_LEN 288

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static bool is_pls_name(const char *name)
{
    const size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".pls") == 0;
}

static int compare_names(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

bool pls_parse_file(const char *path, pls_station_t *out)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return false;
    }
    memset(out, 0, sizeof(*out));

    // The file name is the station name, without the extension.
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out->name, sizeof(out->name), "%s", base);
    char *dot = strrchr(out->name, '.');
    if (dot) *dot = '\0';

    char found[PLS_MAX_MIRRORS][PLS_URL_LEN];
    memset(found, 0, sizeof(found));

    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *s = trim(line);
        if (*s == '\0' || *s == '[' || *s == ';' || *s == '#') continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s);
        char *value = trim(eq + 1);
        if (*value == '\0') continue;

        if (strncasecmp(key, "file", 4) == 0) {
            const int n = atoi(key + 4);
            if (n < 1 || n > PLS_MAX_MIRRORS) continue;
            // Only plain http for now; https needs TLS per connection, and a
            // relative or bare name is not an address at all.
            if (strncasecmp(value, "http://", 7) != 0) {
                ESP_LOGW(TAG, "%s: File%d is not an http:// address", path, n);
                continue;
            }
            snprintf(found[n - 1], PLS_URL_LEN, "%s", value);
        } else if (strncasecmp(key, "title", 5) == 0) {
            const int n = atoi(key + 5);
            if (n != 1) continue;
            snprintf(out->title, sizeof(out->title), "%s", value);
        }
    }
    fclose(f);

    // File2 without File1 is a mirror list with a hole; players keep the rest.
    for (int i = 0; i < PLS_MAX_MIRRORS; i++) {
        if (found[i][0] == '\0') continue;
        memcpy(out->urls[out->url_count++], found[i], PLS_URL_LEN);
    }
    return out->url_count > 0;
}

int pls_scan(const char *dir, pls_station_t *out, int max)
{
    if (!out || max <= 0) return 0;
    if (max > PLS_MAX_STATIONS) max = PLS_MAX_STATIONS;

    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "cannot open %s", dir);
        return 0;
    }

    char(*paths)[PLS_PATH_LEN] = heap_caps_malloc(
        (size_t)max * PLS_PATH_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!paths) paths = malloc((size_t)max * PLS_PATH_LEN);
    if (!paths) {
        ESP_LOGE(TAG, "no memory for the playlist scan");
        closedir(d);
        return 0;
    }
    int files = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && files < max) {
        if (!is_pls_name(e->d_name)) continue;
        snprintf(paths[files], PLS_PATH_LEN, "%s/%s", dir, e->d_name);
        files++;
    }
    closedir(d);

    qsort(paths, (size_t)files, PLS_PATH_LEN, compare_names);

    int stations = 0;
    for (int i = 0; i < files; i++) {
        if (pls_parse_file(paths[i], &out[stations])) {
            ESP_LOGI(TAG, "station %d: %s (%u address%s)", stations + 1,
                     out[stations].name, out[stations].url_count,
                     out[stations].url_count == 1 ? "" : "es");
            stations++;
        }
    }
    heap_caps_free(paths);
    return stations;
}

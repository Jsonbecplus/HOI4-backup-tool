// hoi_backup_tool_v3_debug.c  -  Debug version with English debug output
// Compatible with older GTK4 versions, supports grouped backup, filtering and directory change
#include <gtk/gtk.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <locale.h>
#include <windows.h>

#define PATH_SIZE 2048
#define NAME_SIZE 512

// ==================== Debug Output Control ====================
#define DEBUG_ENABLE 1
#if DEBUG_ENABLE
    #define DEBUG_PRINT(fmt, ...) \
        g_print("[DEBUG] %s: " fmt "\n", __func__, ##__VA_ARGS__)
#else
    #define DEBUG_PRINT(fmt, ...) ((void)0)
#endif
// =============================================================

// Temporary structure for scanning source files
typedef struct {
    char filename[NAME_SIZE];   // Original filename (e.g. "  a.hoi4")
    char basename[NAME_SIZE];   // Base name without extension and leading spaces (e.g. "a")
    time_t mtime;               // Modification time
} SourceFile;

// Configuration structure
typedef struct {
    char source_path[PATH_SIZE];
    char backup_path[PATH_SIZE];
    int auto_backup_interval;  // minutes
    int max_backup_count;
    int backup_mode;  // 0: normal, 1: non-ironman
    int auto_backup_enabled;
    int filter_game_backup;    // 1 = filter in-game auto backups
} Config;

// Global configuration
Config g_config;
GtkWidget *g_main_window = NULL;
GtkWidget *g_status_label = NULL;
GtkWidget *g_backup_btn = NULL;
GtkWidget *g_restore_btn = NULL;
GtkWidget *g_interval_spin = NULL;
GtkWidget *g_max_count_spin = NULL;
GtkWidget *g_mode_dropdown = NULL;
GtkWidget *g_auto_backup_check = NULL;
GtkWidget *g_filter_backup_check = NULL;   // New: filter in-game backups
GtkWidget *g_list_box = NULL;
GtkWidget *g_refresh_btn = NULL;
GtkWidget *g_backup_path_label = NULL;     // Display current backup directory
guint g_auto_timer_id = 0;

// Thread result structure
typedef struct {
    int backup_count;
    char status[256];
    int is_restore;      // 0=backup, 1=restore
    char filename[256];
    int success;
} ThreadResult;

// Function declarations
void init_config();
int create_backup_directory(const char *path);
int backup_file(const char *source_path, const char *filename, int is_ironman);
int restore_file(const char *backup_filename);
void process_ironman_filename(char *filename);
void refresh_file_list();
int do_perform_backup(int mode);
void setup_auto_backup();
static void show_message(const char *title, const char *message);
static void start_backup_async(int mode);
static void start_restore_async(const char *filename);
int is_game_autobackup(const char *filename);
void parse_backup_filename(const char *backup_name, char *basename, char *timestamp_str);
void manage_backup_count(const char *basename);

// Initialize configuration
void init_config() {
    char username[256];
    DWORD size = sizeof(username);
    GetUserNameA(username, &size);

    snprintf(g_config.source_path, sizeof(g_config.source_path),
             "C:\\Users\\%s\\Documents\\Paradox Interactive\\Hearts of Iron IV\\save games",
             username);
    snprintf(g_config.backup_path, sizeof(g_config.backup_path),
             "%s\\hoi_backups", g_config.source_path);

    g_config.auto_backup_interval = 20;
    g_config.max_backup_count = 10;
    g_config.backup_mode = 0;
    g_config.auto_backup_enabled = 1;
    g_config.filter_game_backup = 1;  // enabled by default

    DEBUG_PRINT("source_path = %s", g_config.source_path);
    DEBUG_PRINT("backup_path = %s", g_config.backup_path);
    DEBUG_PRINT("auto_backup_interval = %d min", g_config.auto_backup_interval);
    DEBUG_PRINT("max_backup_count = %d", g_config.max_backup_count);
    DEBUG_PRINT("backup_mode = %d (0=normal, 1=non-ironman)", g_config.backup_mode);
    DEBUG_PRINT("auto_backup_enabled = %d", g_config.auto_backup_enabled);
    DEBUG_PRINT("filter_game_backup = %d", g_config.filter_game_backup);

    create_backup_directory(g_config.backup_path);
    DEBUG_PRINT("configuration initialized");
}

int create_backup_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == -1) {
        DEBUG_PRINT("directory does not exist, creating: %s", path);
#ifdef _WIN32
        int ret = mkdir(path);
#else
        int ret = mkdir(path, 0755);
#endif
        if (ret != 0) {
            DEBUG_PRINT("failed to create directory: %s", path);
        } else {
            DEBUG_PRINT("directory created successfully: %s", path);
        }
        return ret;
    }
    DEBUG_PRINT("directory already exists: %s", path);
    return 0;
}

void process_ironman_filename(char *filename) {
    int i = 0, j = 0, space_count = 0;
    while (filename[i] == ' ' && space_count < 2) {
        space_count++;
        i++;
    }
    if (space_count >= 2) {
        while (filename[i] != '\0') {
            filename[j] = filename[i];
            i++;
            j++;
        }
        filename[j] = '\0';
    }
    DEBUG_PRINT("ironman name processed, result = '%s'", filename);
}

// Generate backup filename: basename_YYYYMMDDHHMM.hoi4
void generate_backup_filename(const char *basename, char *backup, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    snprintf(backup, size, "%s_%04d%02d%02d%02d%02d.hoi4",
             basename,
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min);
    DEBUG_PRINT("generated backup name: %s", backup);
}

// Extract original basename and 12-digit timestamp from backup filename
void parse_backup_filename(const char *backup_name, char *basename, char *timestamp_str) {
    char *p = strrchr(backup_name, '_');
    if (p) {
        int len = strlen(p + 1);
        if (len >= 16) { // 12 digits + ".hoi4"
            char *dot = strrchr(p + 1, '.');
            if (dot && (dot - (p + 1)) == 12) {
                strncpy(timestamp_str, p + 1, 12);
                timestamp_str[12] = '\0';
                size_t base_len = p - backup_name;
                if (base_len >= NAME_SIZE) base_len = NAME_SIZE - 1;
                strncpy(basename, backup_name, base_len);
                basename[base_len] = '\0';
                DEBUG_PRINT("parsed ok: basename='%s', timestamp='%s'", basename, timestamp_str);
                return;
            }
        }
    }
    // Fallback: use whole filename as basename
    strcpy(basename, backup_name);
    timestamp_str[0] = '\0';
    DEBUG_PRINT("parse failed, fallback to full basename: %s", basename);
}

// Check if filename matches in-game auto backup pattern (*_YYYY_MM_DD_HH.hoi4)
int is_game_autobackup(const char *filename) {
    static GRegex *regex = NULL;
    if (!regex) {
        GError *error = NULL;
        regex = g_regex_new(".*_[0-9]{4}_[0-9]{2}_[0-9]{2}_[0-9]{2}\\.hoi4$",
                            0, 0, &error);
        if (error) {
            DEBUG_PRINT("regex compile error: %s", error->message);
            g_error_free(error);
            return 0;
        }
        DEBUG_PRINT("game autobackup regex compiled successfully");
    }
    int match = g_regex_match(regex, filename, 0, NULL);
    DEBUG_PRINT("check '%s' -> game autobackup = %s", filename, match ? "yes" : "no");
    return match;
}

// Manage backup count per basename: delete oldest if exceeding max_backup_count
void manage_backup_count(const char *basename) {
    DEBUG_PRINT("managing backups for basename: %s", basename);

    DIR *dir = opendir(g_config.backup_path);
    if (!dir) {
        DEBUG_PRINT("failed to open backup directory: %s", g_config.backup_path);
        return;
    }

    struct dirent *entry;
    typedef struct { char name[PATH_SIZE]; time_t time; } BackupEntry;
    BackupEntry *entries = NULL;
    int count = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char fullpath[PATH_SIZE];
        snprintf(fullpath, sizeof(fullpath), "%s\\%s", g_config.backup_path, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0 || !(st.st_mode & S_IFREG)) continue;
        if (strstr(entry->d_name, ".hoi4") == NULL) continue;

        char this_basename[NAME_SIZE] = {0};
        char ts_str[16] = {0};
        parse_backup_filename(entry->d_name, this_basename, ts_str);

        if (strcmp(this_basename, basename) != 0) continue;
        if (ts_str[0] == '\0') continue;

        entries = realloc(entries, (count + 1) * sizeof(BackupEntry));
        strcpy(entries[count].name, entry->d_name);

        struct tm tm = {0};
        sscanf(ts_str, "%04d%02d%02d%02d%02d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min);
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        entries[count].time = mktime(&tm);
        count++;
    }
    closedir(dir);

    DEBUG_PRINT("found %d existing backups for '%s'", count, basename);

    // We are about to create 1 new backup, so keep (max - 1) old ones
    int to_delete = count - (g_config.max_backup_count - 1);
    if (to_delete <= 0) {
        DEBUG_PRINT("no need to delete old backups (to_delete=%d)", to_delete);
        free(entries);
        return;
    }

    DEBUG_PRINT("will delete %d oldest backup(s)", to_delete);

    // Sort by time ascending (oldest first)
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (entries[i].time > entries[j].time) {
                BackupEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    for (int i = 0; i < to_delete; i++) {
        char fullpath[PATH_SIZE];
        snprintf(fullpath, sizeof(fullpath), "%s\\%s", g_config.backup_path, entries[i].name);
        DEBUG_PRINT("deleting old backup: %s", entries[i].name);
        remove(fullpath);
    }

    free(entries);
    DEBUG_PRINT("backup count management finished");
}

// Backup a single file
int backup_file(const char *source_path, const char *filename, int is_ironman) {
    char source_file[PATH_SIZE];
    char backup_file[PATH_SIZE];
    char processed_name[NAME_SIZE];
    char basename[NAME_SIZE];
    struct stat st;

    DEBUG_PRINT("start backup: file='%s', ironman=%d", filename, is_ironman);

    strcpy(processed_name, filename);
    if (is_ironman) {
        process_ironman_filename(processed_name);
    }

    // Extract basename (without extension)
    char *dot = strrchr(processed_name, '.');
    if (dot) {
        size_t len = dot - processed_name;
        if (len >= sizeof(basename)) len = sizeof(basename) - 1;
        strncpy(basename, processed_name, len);
        basename[len] = '\0';
    } else {
        strcpy(basename, processed_name);
    }

    snprintf(source_file, sizeof(source_file), "%s\\%s", source_path, filename);
    if (stat(source_file, &st) != 0) {
        DEBUG_PRINT("source file not found: %s", source_file);
        return -1;
    }

    // Generate backup filename
    char backup_name[NAME_SIZE];
    generate_backup_filename(basename, backup_name, sizeof(backup_name));
    snprintf(backup_file, sizeof(backup_file), "%s\\%s", g_config.backup_path, backup_name);

    // Manage backup count before creating new one
    manage_backup_count(basename);

    // Copy file
    FILE *src = fopen(source_file, "rb");
    if (!src) {
        DEBUG_PRINT("failed to open source file for reading: %s", source_file);
        return -1;
    }

    FILE *dst = fopen(backup_file, "wb");
    if (!dst) {
        DEBUG_PRINT("failed to open backup file for writing: %s", backup_file);
        fclose(src);
        return -1;
    }

    char buffer[8192];
    size_t bytes;
    size_t total = 0;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dst);
        total += bytes;
    }

    fclose(src);
    fclose(dst);

    DEBUG_PRINT("backup completed: %zu bytes written -> %s", total, backup_name);
    return 0;
}

// Restore: extract basename from backup and restore as basename.hoi4
int restore_file(const char *backup_filename) {
    char backup_file[PATH_SIZE];
    char source_file[PATH_SIZE];
    char backup_source[PATH_SIZE];
    char basename[NAME_SIZE] = {0};
    char ts_str[16] = {0};

    DEBUG_PRINT("start restore: backup_file='%s'", backup_filename);

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    parse_backup_filename(backup_filename, basename, ts_str);
    if (basename[0] == '\0') {
        strcpy(basename, backup_filename);
        char *dot = strrchr(basename, '.');
        if (dot) *dot = '\0';
        DEBUG_PRINT("parse fallback, basename = %s", basename);
    }

    snprintf(backup_file, sizeof(backup_file), "%s\\%s", g_config.backup_path, backup_filename);
    snprintf(source_file, sizeof(source_file), "%s\\%s.hoi4", g_config.source_path, basename);
    snprintf(backup_source, sizeof(backup_source),
             "%s\\%s_backup.%04d%02d%02d_%02d%02d%02d.hoi4",
             g_config.source_path, basename,
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    struct stat st;
    if (stat(backup_file, &st) != 0) {
        DEBUG_PRINT("backup file not found: %s", backup_file);
        return -1;
    }

    if (stat(source_file, &st) == 0) {
        DEBUG_PRINT("existing save found, renaming to: %s", backup_source);
        rename(source_file, backup_source);
    }

    FILE *src = fopen(backup_file, "rb");
    if (!src) {
        DEBUG_PRINT("failed to open backup file for reading: %s", backup_file);
        return -1;
    }

    FILE *dst = fopen(source_file, "wb");
    if (!dst) {
        DEBUG_PRINT("failed to open target file for writing: %s", source_file);
        fclose(src);
        return -1;
    }

    char buffer[8192];
    size_t bytes;
    size_t total = 0;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dst);
        total += bytes;
    }

    fclose(src);
    fclose(dst);

    DEBUG_PRINT("restore completed: %zu bytes -> %s", total, source_file);
    return 0;
}

// Core logic: perform backup, with in-game backup filtering
int do_perform_backup(int mode) {
    int backup_count = 0;
    DEBUG_PRINT("perform backup started, mode=%d", mode);

    if (mode == 1) {
        // Non-ironman mode: only backup autosave.hoi4
        char source_file[PATH_SIZE];
        snprintf(source_file, sizeof(source_file), "%s\\autosave.hoi4", g_config.source_path);
        struct stat st;
        if (stat(source_file, &st) == 0) {
            if (!(g_config.filter_game_backup && is_game_autobackup("autosave.hoi4"))) {
                DEBUG_PRINT("non-ironman mode: backing up autosave.hoi4");
                if (backup_file(g_config.source_path, "autosave.hoi4", 0) == 0)
                    backup_count = 1;
            } else {
                DEBUG_PRINT("autosave.hoi4 filtered out by game backup filter");
            }
        } else {
            DEBUG_PRINT("autosave.hoi4 not found in source directory");
        }
        DEBUG_PRINT("non-ironman backup finished, count=%d", backup_count);
        return backup_count;
    }

    // Normal mode: scan all .hoi4, group by basename, backup latest in each group
    DIR *dir = opendir(g_config.source_path);
    if (!dir) {
        DEBUG_PRINT("failed to open source directory: %s", g_config.source_path);
        return 0;
    }

    struct dirent *entry;
    SourceFile *files = NULL;
    int file_count = 0;
    int filtered_count = 0;

    while ((entry = readdir(dir)) != NULL) {
        char fullpath[PATH_SIZE];
        snprintf(fullpath, sizeof(fullpath), "%s\\%s", g_config.source_path, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0 || !(st.st_mode & S_IFREG))
            continue;

        char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext, ".hoi4") != 0)
            continue;

        // Filter in-game auto backups if enabled
        if (g_config.filter_game_backup && is_game_autobackup(entry->d_name)) {
            filtered_count++;
            continue;
        }

        // Add to file list
        files = realloc(files, (file_count + 1) * sizeof(SourceFile));
        strcpy(files[file_count].filename, entry->d_name);

        // Compute basename: remove extension and leading two spaces if present
        char temp[NAME_SIZE];
        strcpy(temp, entry->d_name);
        char *dot = strrchr(temp, '.');
        if (dot) *dot = '\0';

        if (strlen(temp) >= 2 && temp[0] == ' ' && temp[1] == ' ')
            strcpy(files[file_count].basename, temp + 2);
        else
            strcpy(files[file_count].basename, temp);

        files[file_count].mtime = st.st_mtime;
        file_count++;
    }
    closedir(dir);

    DEBUG_PRINT("scan finished: %d valid files, %d filtered out", file_count, filtered_count);

    if (file_count == 0) {
        free(files);
        DEBUG_PRINT("no files to backup");
        return 0;
    }

    // Sort by basename to enable grouping
    for (int i = 0; i < file_count - 1; i++) {
        for (int j = i + 1; j < file_count; j++) {
            if (strcmp(files[i].basename, files[j].basename) > 0) {
                SourceFile tmp = files[i];
                files[i] = files[j];
                files[j] = tmp;
            }
        }
    }

    // Iterate groups and backup the latest file in each group
    int i = 0;
    int group_count = 0;
    while (i < file_count) {
        char current_basename[NAME_SIZE];
        strcpy(current_basename, files[i].basename);

        int best_idx = i;
        int j = i + 1;
        while (j < file_count && strcmp(files[j].basename, current_basename) == 0) {
            if (files[j].mtime > files[best_idx].mtime)
                best_idx = j;
            j++;
        }

        int is_ironman = (strlen(files[best_idx].filename) >= 2 &&
                          files[best_idx].filename[0] == ' ' &&
                          files[best_idx].filename[1] == ' ');

        DEBUG_PRINT("group '%s': selected '%s' (latest, ironman=%d)",
                    current_basename, files[best_idx].filename, is_ironman);

        if (backup_file(g_config.source_path, files[best_idx].filename, is_ironman) == 0) {
            backup_count++;
        }
        group_count++;
        i = j; // jump to next group
    }

    free(files);
    DEBUG_PRINT("backup finished: %d groups processed, %d files backed up",
                group_count, backup_count);
    return backup_count;
}

// UI update after background thread finishes
static gboolean on_thread_finished(gpointer user_data) {
    ThreadResult *res = (ThreadResult*)user_data;
    DEBUG_PRINT("thread finished: is_restore=%d, success=%d", res->is_restore, res->success);

    gtk_widget_set_sensitive(g_backup_btn, TRUE);
    gtk_widget_set_sensitive(g_restore_btn, TRUE);

    if (res->is_restore) {
        if (res->success == 0) {
            gtk_label_set_text(GTK_LABEL(g_status_label), res->status);
            show_message("Success", res->status);
        } else {
            gtk_label_set_text(GTK_LABEL(g_status_label), res->status);
            show_message("Error", res->status);
        }
    } else {
        gtk_label_set_text(GTK_LABEL(g_status_label), res->status);
    }

    refresh_file_list();
    free(res);
    return G_SOURCE_REMOVE;
}

static gpointer backup_thread_func(gpointer data) {
    int mode = GPOINTER_TO_INT(data);
    DEBUG_PRINT("backup thread started, mode=%d", mode);
    int count = do_perform_backup(mode);

    ThreadResult *res = calloc(1, sizeof(ThreadResult));
    res->backup_count = count;
    res->is_restore = 0;
    snprintf(res->status, sizeof(res->status), "Backup complete: %d file(s) backed up", count);
    g_idle_add(on_thread_finished, res);
    return NULL;
}

static gpointer restore_thread_func(gpointer data) {
    char *filename = (char*)data;
    DEBUG_PRINT("restore thread started, file=%s", filename);
    int success = restore_file(filename);

    ThreadResult *res = calloc(1, sizeof(ThreadResult));
    res->is_restore = 1;
    res->success = success;
    if (success == 0) {
        snprintf(res->status, sizeof(res->status), "Restore successful: %s", filename);
    } else {
        snprintf(res->status, sizeof(res->status), "Restore failed: %s", filename);
    }
    g_idle_add(on_thread_finished, res);
    free(filename);
    return NULL;
}

static void start_backup_async(int mode) {
    DEBUG_PRINT("starting async backup, mode=%d", mode);
    gtk_widget_set_sensitive(g_backup_btn, FALSE);
    gtk_widget_set_sensitive(g_restore_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(g_status_label), "Backing up...");
    GThread *thread = g_thread_new("backup", backup_thread_func, GINT_TO_POINTER(mode));
    g_thread_unref(thread);
}

static void start_restore_async(const char *filename) {
    DEBUG_PRINT("starting async restore: %s", filename);
    gtk_widget_set_sensitive(g_backup_btn, FALSE);
    gtk_widget_set_sensitive(g_restore_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(g_status_label), "Restoring...");
    char *copy = strdup(filename);
    GThread *thread = g_thread_new("restore", restore_thread_func, copy);
    g_thread_unref(thread);
}

// Refresh file list (descending order by filename)
void refresh_file_list() {
    DEBUG_PRINT("refreshing backup file list");

    GtkWidget *child = gtk_widget_get_first_child(g_list_box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(g_list_box), child);
        child = next;
    }

    DIR *dir = opendir(g_config.backup_path);
    struct dirent *entry;
    if (dir) {
        char **files = NULL;
        int count = 0;
        while ((entry = readdir(dir)) != NULL) {
            char filepath[PATH_SIZE];
            snprintf(filepath, sizeof(filepath), "%s\\%s", g_config.backup_path, entry->d_name);
            struct stat st;
            if (stat(filepath, &st) == 0 && (st.st_mode & S_IFREG)) {
                char *ext = strrchr(entry->d_name, '.');
                if (ext && strcmp(ext, ".hoi4") == 0) {
                    files = realloc(files, (count + 1) * sizeof(char*));
                    files[count] = strdup(entry->d_name);
                    count++;
                }
            }
        }
        closedir(dir);

        DEBUG_PRINT("found %d backup files in directory", count);

        // Sort descending (newest date first by lexicographic order)
        for (int i = 0; i < count - 1; i++) {
            for (int j = i + 1; j < count; j++) {
                if (strcmp(files[i], files[j]) < 0) {
                    char *temp = files[i];
                    files[i] = files[j];
                    files[j] = temp;
                }
            }
        }

        for (int i = 0; i < count; i++) {
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *label = gtk_label_new(files[i]);
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_widget_set_margin_start(label, 10);
            gtk_widget_set_margin_end(label, 10);
            gtk_widget_set_margin_top(label, 5);
            gtk_widget_set_margin_bottom(label, 5);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
            gtk_list_box_append(GTK_LIST_BOX(g_list_box), row);
            free(files[i]);
        }
        free(files);
    } else {
        DEBUG_PRINT("failed to open backup directory for listing: %s", g_config.backup_path);
    }
    DEBUG_PRINT("file list refresh completed");
}

gboolean auto_backup_callback(gpointer user_data) {
    (void)user_data;
    DEBUG_PRINT("auto backup timer triggered, enabled=%d", g_config.auto_backup_enabled);
    if (g_config.auto_backup_enabled) {
        start_backup_async(g_config.backup_mode);
    }
    return G_SOURCE_CONTINUE;
}

void setup_auto_backup() {
    DEBUG_PRINT("setting up auto backup, enabled=%d, interval=%d min",
                g_config.auto_backup_enabled, g_config.auto_backup_interval);

    if (g_auto_timer_id > 0) {
        DEBUG_PRINT("removing previous timer id=%u", g_auto_timer_id);
        g_source_remove(g_auto_timer_id);
        g_auto_timer_id = 0;
    }

    if (g_config.auto_backup_enabled) {
        g_auto_timer_id = g_timeout_add_seconds(g_config.auto_backup_interval * 60,
                                                auto_backup_callback, NULL);
        DEBUG_PRINT("new timer created, id=%u", g_auto_timer_id);
    }
}

// Button callback: backup
void on_backup_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    DEBUG_PRINT("backup button clicked");
    start_backup_async(g_config.backup_mode);
}

// Button callback: restore
void on_restore_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    DEBUG_PRINT("restore button clicked");

    GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_list_box));
    if (!selected) {
        DEBUG_PRINT("no backup file selected, aborting restore");
        show_message("Notice", "Please select a backup file to restore first");
        return;
    }

    GtkWidget *child = gtk_list_box_row_get_child(selected);
    if (GTK_IS_LABEL(child)) {
        const char *filename = gtk_label_get_text(GTK_LABEL(child));
        if (filename) {
            DEBUG_PRINT("selected backup: %s", filename);
            start_restore_async(filename);
        }
    }
}

// Mode switch
void on_mode_changed(GtkDropDown *dropdown, gpointer user_data) {
    (void)user_data;
    guint selected = gtk_drop_down_get_selected(dropdown);
    g_config.backup_mode = selected;
    DEBUG_PRINT("backup mode changed to %u", selected);

    gtk_widget_set_sensitive(g_backup_btn, selected == 0);
    if (selected == 1) {
        gtk_label_set_text(GTK_LABEL(g_status_label), "Non-ironman mode: only autosave.hoi4 will be backed up");
    } else {
        gtk_label_set_text(GTK_LABEL(g_status_label), "Normal mode: all .hoi4 files can be backed up");
    }
}

// Interval changed
void on_interval_changed(GtkSpinButton *spin, gpointer user_data) {
    (void)user_data;
    g_config.auto_backup_interval = gtk_spin_button_get_value_as_int(spin);
    DEBUG_PRINT("auto backup interval changed to %d minutes", g_config.auto_backup_interval);
    setup_auto_backup();
}

// Max backup count changed
void on_max_count_changed(GtkSpinButton *spin, gpointer user_data) {
    (void)user_data;
    g_config.max_backup_count = gtk_spin_button_get_value_as_int(spin);
    DEBUG_PRINT("max backup count changed to %d", g_config.max_backup_count);
}

// Auto backup toggle
void on_auto_backup_toggled(GtkCheckButton *check, gpointer user_data) {
    (void)user_data;
    g_config.auto_backup_enabled = gtk_check_button_get_active(check);
    DEBUG_PRINT("auto backup toggled: %d", g_config.auto_backup_enabled);
    setup_auto_backup();
}

// Filter in-game backups toggle
void on_filter_backup_toggled(GtkCheckButton *check, gpointer user_data) {
    (void)user_data;
    g_config.filter_game_backup = gtk_check_button_get_active(check);
    DEBUG_PRINT("filter game backups toggled: %d", g_config.filter_game_backup);
}

// Refresh list
void on_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    DEBUG_PRINT("refresh button clicked");
    refresh_file_list();
    gtk_label_set_text(GTK_LABEL(g_status_label), "File list refreshed");
}

// Choose backup directory response callback
static void choose_backup_dir_response_cb(GtkNativeDialog *native_dialog, int response_id, gpointer user_data) {
    (void)user_data;
    if (response_id == GTK_RESPONSE_ACCEPT) {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(native_dialog);
        GFile *file = gtk_file_chooser_get_file(chooser);
        if (file) {
            char *path = g_file_get_path(file);
            if (path) {
                strncpy(g_config.backup_path, path, sizeof(g_config.backup_path) - 1);
                g_config.backup_path[sizeof(g_config.backup_path) - 1] = '\0';
                DEBUG_PRINT("backup directory changed to: %s", g_config.backup_path);

                create_backup_directory(g_config.backup_path);

                char display[PATH_SIZE + 20];
                snprintf(display, sizeof(display), "Backup directory: %s", g_config.backup_path);
                gtk_label_set_text(GTK_LABEL(g_backup_path_label), display);
                refresh_file_list();
                g_free(path);
            }
            g_object_unref(file);
        }
    } else {
        DEBUG_PRINT("backup directory selection cancelled");
    }
    g_object_unref(native_dialog);
}

// "Browse..." button callback
void on_choose_backup_dir_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    DEBUG_PRINT("browse backup directory button clicked");
    GtkFileChooserNative *native = gtk_file_chooser_native_new("Select Backup Directory",
                                                               GTK_WINDOW(g_main_window),
                                                               GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                               "OK",
                                                               "Cancel");
    g_signal_connect(native, "response", G_CALLBACK(choose_backup_dir_response_cb), NULL);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(native));
}

// Custom message dialog
static void custom_dialog_close_cb(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *dialog = GTK_WINDOW(user_data);
    gtk_window_destroy(dialog);
}

static void show_message(const char *title, const char *message) {
    DEBUG_PRINT("showing message dialog: title='%s', message='%s'", title, message);

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(g_main_window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 360, 160);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(vbox, 20);
    gtk_widget_set_margin_bottom(vbox, 20);
    gtk_widget_set_margin_start(vbox, 20);
    gtk_widget_set_margin_end(vbox, 20);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);

    GtkWidget *label = gtk_label_new(message);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(vbox), label);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(vbox), btn_box);

    GtkWidget *ok_btn = gtk_button_new_with_label("OK");
    g_signal_connect(ok_btn, "clicked", G_CALLBACK(custom_dialog_close_cb), dialog);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);

    gtk_window_present(GTK_WINDOW(dialog));
}

// Create main window
static void create_main_window(void) {
    DEBUG_PRINT("creating main window");

    g_main_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(g_main_window), "HOI Save Backup Tool");
    gtk_window_set_default_size(GTK_WINDOW(g_main_window), 800, 600);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_window_set_child(GTK_WINDOW(g_main_window), main_box);
    gtk_widget_set_margin_start(main_box, 10);
    gtk_widget_set_margin_end(main_box, 10);
    gtk_widget_set_margin_top(main_box, 10);
    gtk_widget_set_margin_bottom(main_box, 10);

    // Source directory
    GtkWidget *path_label = gtk_label_new(NULL);
    char path_text[PATH_SIZE];
    snprintf(path_text, sizeof(path_text), "Source: %s", g_config.source_path);
    gtk_label_set_text(GTK_LABEL(path_label), path_text);
    gtk_label_set_xalign(GTK_LABEL(path_label), 0.0);
    gtk_box_append(GTK_BOX(main_box), path_label);

    // Backup directory (changeable)
    GtkWidget *backup_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(main_box), backup_row);

    g_backup_path_label = gtk_label_new(NULL);
    char backup_text[PATH_SIZE + 20];
    snprintf(backup_text, sizeof(backup_text), "Backup: %s", g_config.backup_path);
    gtk_label_set_text(GTK_LABEL(g_backup_path_label), backup_text);
    gtk_label_set_xalign(GTK_LABEL(g_backup_path_label), 0.0);
    gtk_widget_set_hexpand(g_backup_path_label, TRUE);
    gtk_box_append(GTK_BOX(backup_row), g_backup_path_label);

    GtkWidget *browse_btn = gtk_button_new_with_label("Browse...");
    g_signal_connect(browse_btn, "clicked", G_CALLBACK(on_choose_backup_dir_clicked), NULL);
    gtk_box_append(GTK_BOX(backup_row), browse_btn);

    // Control row 1
    GtkWidget *control_box1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(main_box), control_box1);

    GtkWidget *mode_label = gtk_label_new("Mode:");
    gtk_box_append(GTK_BOX(control_box1), mode_label);

    const char *modes[] = {"Normal Mode", "Non-ironman Mode", NULL};
    GtkStringList *mode_list = gtk_string_list_new(modes);
    g_mode_dropdown = gtk_drop_down_new(G_LIST_MODEL(mode_list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_mode_dropdown), 0);
    g_signal_connect(g_mode_dropdown, "notify::selected", G_CALLBACK(on_mode_changed), NULL);
    gtk_box_append(GTK_BOX(control_box1), g_mode_dropdown);

    g_backup_btn = gtk_button_new_with_label("Backup Now");
    g_signal_connect(g_backup_btn, "clicked", G_CALLBACK(on_backup_clicked), NULL);
    gtk_box_append(GTK_BOX(control_box1), g_backup_btn);

    g_restore_btn = gtk_button_new_with_label("Restore Selected");
    g_signal_connect(g_restore_btn, "clicked", G_CALLBACK(on_restore_clicked), NULL);
    gtk_box_append(GTK_BOX(control_box1), g_restore_btn);

    g_refresh_btn = gtk_button_new_with_label("Refresh List");
    g_signal_connect(g_refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    gtk_box_append(GTK_BOX(control_box1), g_refresh_btn);

    // Control row 2 (auto backup & filter)
    GtkWidget *control_box2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(main_box), control_box2);

    g_auto_backup_check = gtk_check_button_new_with_label("Enable Auto Backup");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_auto_backup_check), g_config.auto_backup_enabled);
    g_signal_connect(g_auto_backup_check, "toggled", G_CALLBACK(on_auto_backup_toggled), NULL);
    gtk_box_append(GTK_BOX(control_box2), g_auto_backup_check);

    GtkWidget *interval_label = gtk_label_new("Interval (min):");
    gtk_box_append(GTK_BOX(control_box2), interval_label);

    g_interval_spin = gtk_spin_button_new_with_range(1, 1440, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_interval_spin), g_config.auto_backup_interval);
    g_signal_connect(g_interval_spin, "value-changed", G_CALLBACK(on_interval_changed), NULL);
    gtk_box_append(GTK_BOX(control_box2), g_interval_spin);

    GtkWidget *max_label = gtk_label_new("Max Backups:");
    gtk_box_append(GTK_BOX(control_box2), max_label);

    g_max_count_spin = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_max_count_spin), g_config.max_backup_count);
    g_signal_connect(g_max_count_spin, "value-changed", G_CALLBACK(on_max_count_changed), NULL);
    gtk_box_append(GTK_BOX(control_box2), g_max_count_spin);

    // Filter in-game backups checkbox
    g_filter_backup_check = gtk_check_button_new_with_label("Filter In-game Backups");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_filter_backup_check), g_config.filter_game_backup);
    g_signal_connect(g_filter_backup_check, "toggled", G_CALLBACK(on_filter_backup_toggled), NULL);
    gtk_box_append(GTK_BOX(control_box2), g_filter_backup_check);

    // Backup list
    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    g_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_list_box), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(g_list_box), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), g_list_box);
    gtk_box_append(GTK_BOX(main_box), scrolled_window);

    // Status bar
    g_status_label = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(g_status_label), 0.0);
    gtk_box_append(GTK_BOX(main_box), g_status_label);

    refresh_file_list();
    DEBUG_PRINT("main window created successfully");
}

static void app_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    DEBUG_PRINT("application activated");
    init_config();
    create_main_window();
    setup_auto_backup();
    gtk_window_set_application(GTK_WINDOW(g_main_window), app);
    gtk_window_present(GTK_WINDOW(g_main_window));
}

int main(void) {
    g_setenv("GDK_PIXBUF_MODULEDIR", "lib\\gdk-pixbuf-2.0\\2.10.0\\loaders", TRUE);
    g_setenv("GDK_PIXBUF_MODULE_FILE", "lib\\gdk-pixbuf-2.0\\2.10.0\\loaders.cache", TRUE);

    setlocale(LC_ALL, "zh_CN.UTF-8");
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    DEBUG_PRINT("program started");

    GtkApplication *app = gtk_application_new("com.example.hoibackup", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(app_activate), NULL);

    int ret = g_application_run(G_APPLICATION(app), 0, NULL);
    DEBUG_PRINT("application exiting with code %d", ret);

    if (g_auto_timer_id > 0) {
        g_source_remove(g_auto_timer_id);
    }
    g_object_unref(app);
    return ret;
}

// hoi_backup_tool_v2.c 兼容旧GTK4版本，增加分组备份、过滤与目录更改
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

// 用于扫描源文件的临时结构
typedef struct {
    char filename[NAME_SIZE];   // 原始文件名（如 "  a.hoi4"）
    char basename[NAME_SIZE];   // 基础名称（去除扩展名和前导空格，如 "a"）
    time_t mtime;               // 修改时间
} SourceFile;

// 配置结构体
typedef struct {
    char source_path[PATH_SIZE];
    char backup_path[PATH_SIZE];
    int auto_backup_interval;  // 分钟
    int max_backup_count;
    int backup_mode;  // 0: 普通, 1: 非铁人
    int auto_backup_enabled;
    int filter_game_backup;    // 1=过滤游戏内备份
} Config;

// 全局配置
Config g_config;
GtkWidget *g_main_window = NULL;
GtkWidget *g_status_label = NULL;
GtkWidget *g_backup_btn = NULL;
GtkWidget *g_restore_btn = NULL;
GtkWidget *g_interval_spin = NULL;
GtkWidget *g_max_count_spin = NULL;
GtkWidget *g_mode_dropdown = NULL;
GtkWidget *g_auto_backup_check = NULL;
GtkWidget *g_filter_backup_check = NULL;   // 新增：过滤游戏内备份
GtkWidget *g_list_box = NULL;
GtkWidget *g_refresh_btn = NULL;
GtkWidget *g_backup_path_label = NULL;     // 显示当前备份目录
guint g_auto_timer_id = 0;

// 线程结果传递结构
typedef struct {
    int backup_count;
    char status[256];
    int is_restore;      // 0=backup, 1=restore
    char filename[256];
    int success;
} ThreadResult;

// 函数声明
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

// 初始化配置
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
    g_config.filter_game_backup = 1;  // 默认开启

    create_backup_directory(g_config.backup_path);
}

int create_backup_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == -1) {
#ifdef _WIN32
        return mkdir(path);
#else
        return mkdir(path, 0755);
#endif
    }
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
}

// 生成备份文件名：basename_YYYYMMDDHHMM.hoi4
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
}

// 从备份文件名中提取原始basename和时间戳字符串（12位）
void parse_backup_filename(const char *backup_name, char *basename, char *timestamp_str) {
    char *p = strrchr(backup_name, '_');
    if (p) {
        // 检查后面是否为12位数字+".hoi4"
        int len = strlen(p + 1);
        if (len >= 16) { // 12数字 + ".hoi4"
            char *dot = strrchr(p + 1, '.');
            if (dot && (dot - (p + 1)) == 12) {
                strncpy(timestamp_str, p + 1, 12);
                timestamp_str[12] = '\0';
                // 复制basename
                size_t base_len = p - backup_name;
                if (base_len >= NAME_SIZE) base_len = NAME_SIZE - 1;
                strncpy(basename, backup_name, base_len);
                basename[base_len] = '\0';
                return;
            }
        }
    }
    // 若解析失败，将整个文件名作为basename（无时间戳）
    strcpy(basename, backup_name);
    timestamp_str[0] = '\0';
}

// 判断是否为游戏内自动备份（格式：*_YYYY_MM_DD_HH.hoi4）
int is_game_autobackup(const char *filename) {
    static GRegex *regex = NULL;
    if (!regex) {
        GError *error = NULL;
        regex = g_regex_new(".*_[0-9]{4}_[0-9]{2}_[0-9]{2}_[0-9]{2}\\.hoi4$",
                            0, 0, &error);
        if (error) {
            g_printerr("Regex compile error: %s\n", error->message);
            g_error_free(error);
            return 0;
        }
    }
    return g_regex_match(regex, filename, 0, NULL);
}

// 管理同basename的备份数量：若超过max_backup_count，删除最旧的
void manage_backup_count(const char *basename) {
    DIR *dir = opendir(g_config.backup_path);
    if (!dir) return;

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
        sscanf(ts_str, "%04d%02d%02d%02d%02d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min);
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        entries[count].time = mktime(&tm);
        count++;
    }
    closedir(dir);

    // 外面马上还要创建 1 个新备份，所以最终应该只剩 max_backup_count 个
    // 需要删除的数量 = 当前数量 - (上限 - 1)
    int to_delete = count - (g_config.max_backup_count - 1);
    if (to_delete <= 0) {
        free(entries);
        return;
    }
    // =====================

    // 按时间升序排序（最旧在前）
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
        remove(fullpath);
    }
    free(entries);
}
// 备份单个文件
int backup_file(const char *source_path, const char *filename, int is_ironman) {
    char source_file[PATH_SIZE];
    char backup_file[PATH_SIZE];
    char processed_name[NAME_SIZE];
    char basename[NAME_SIZE];
    struct stat st;

    strcpy(processed_name, filename);
    if (is_ironman) {
        process_ironman_filename(processed_name);
    }

    // 提取basename（不含扩展名）
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
        return -1;
    }

    // 生成备份文件名
    char backup_name[NAME_SIZE];
    generate_backup_filename(basename, backup_name, sizeof(backup_name));
    snprintf(backup_file, sizeof(backup_file), "%s\\%s", g_config.backup_path, backup_name);

    // 管理该basename的备份数量（先删除旧备份，再创建新备份）
    manage_backup_count(basename);

    // 执行拷贝
    FILE *src = fopen(source_file, "rb");
    if (!src) return -1;
    FILE *dst = fopen(backup_file, "wb");
    if (!dst) {
        fclose(src);
        return -1;
    }
    char buffer[8192];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dst);
    }
    fclose(src);
    fclose(dst);
    return 0;
}

// 还原：从备份文件名提取basename，还原为 basename.hoi4
int restore_file(const char *backup_filename) {
    char backup_file[PATH_SIZE];
    char source_file[PATH_SIZE];
    char backup_source[PATH_SIZE];
    char basename[NAME_SIZE] = {0};
    char ts_str[16] = {0};
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    parse_backup_filename(backup_filename, basename, ts_str);
    if (basename[0] == '\0') {
        // 若解析失败，尝试直接去掉扩展名
        strcpy(basename, backup_filename);
        char *dot = strrchr(basename, '.');
        if (dot) *dot = '\0';
    }

    snprintf(backup_file, sizeof(backup_file), "%s\\%s", g_config.backup_path, backup_filename);
    snprintf(source_file, sizeof(source_file), "%s\\%s.hoi4", g_config.source_path, basename);
    snprintf(backup_source, sizeof(backup_source), "%s\\%s_backup.%04d%02d%02d_%02d%02d%02d.hoi4",
             g_config.source_path, basename,
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    struct stat st;
    if (stat(backup_file, &st) != 0) {
        return -1;
    }
    if (stat(source_file, &st) == 0) {
        rename(source_file, backup_source);
    }

    FILE *src = fopen(backup_file, "rb");
    if (!src) return -1;
    FILE *dst = fopen(source_file, "wb");
    if (!dst) {
        fclose(src);
        return -1;
    }
    char buffer[8192];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dst);
    }
    fclose(src);
    fclose(dst);
    return 0;
}

// 纯逻辑：执行备份，过滤游戏内备份
int do_perform_backup(int mode) {
    int backup_count = 0;

    if (mode == 1) {
        // 非铁人模式：只备份 autosave.hoi4
        char source_file[PATH_SIZE];
        snprintf(source_file, sizeof(source_file), "%s\\autosave.hoi4", g_config.source_path);
        struct stat st;
        if (stat(source_file, &st) == 0) {
            // 过滤游戏内备份（autosave 一般不会被过滤，但保留判断）
            if (!(g_config.filter_game_backup && is_game_autobackup("autosave.hoi4"))) {
                if (backup_file(g_config.source_path, "autosave.hoi4", 0) == 0)
                    backup_count = 1;
            }
        }
        return backup_count;
    }

    // 普通模式：扫描所有 .hoi4，分组后只备份每组最新的
    DIR *dir = opendir(g_config.source_path);
    if (!dir) return 0;

    struct dirent *entry;
    SourceFile *files = NULL;
    int file_count = 0;

    while ((entry = readdir(dir)) != NULL) {
        char fullpath[PATH_SIZE];
        snprintf(fullpath, sizeof(fullpath), "%s\\%s", g_config.source_path, entry->d_name);
        struct stat st;
        if (stat(fullpath, &st) != 0 || !(st.st_mode & S_IFREG))
            continue;

        char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext, ".hoi4") != 0)
            continue;

        // 过滤游戏内备份（如果开启）
        if (g_config.filter_game_backup && is_game_autobackup(entry->d_name))
            continue;

        // 添加到文件列表
        files = realloc(files, (file_count + 1) * sizeof(SourceFile));
        strcpy(files[file_count].filename, entry->d_name);

        // 计算 basename：去扩展名，并去除前导两个空格（如果有）
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

    if (file_count == 0) {
        free(files);
        return 0;
    }

    // 按 basename 排序（为了分组）
    for (int i = 0; i < file_count - 1; i++) {
        for (int j = i + 1; j < file_count; j++) {
            if (strcmp(files[i].basename, files[j].basename) > 0) {
                SourceFile tmp = files[i];
                files[i] = files[j];
                files[j] = tmp;
            }
        }
    }

    // 遍历每组，选择时间最新的备份
    int i = 0;
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

        // 备份选中的文件
        int is_ironman = (strlen(files[best_idx].filename) >= 2 &&
                          files[best_idx].filename[0] == ' ' &&
                          files[best_idx].filename[1] == ' ');
        if (backup_file(g_config.source_path, files[best_idx].filename, is_ironman) == 0) {
            backup_count++;
        }

        i = j; // 跳到下一组
    }

    free(files);
    return backup_count;
}

// 后台线程完成后的UI更新
static gboolean on_thread_finished(gpointer user_data) {
    ThreadResult *res = (ThreadResult*)user_data;

    gtk_widget_set_sensitive(g_backup_btn, TRUE);
    gtk_widget_set_sensitive(g_restore_btn, TRUE);

    if (res->is_restore) {
        if (res->success == 0) {
            gtk_label_set_text(GTK_LABEL(g_status_label), res->status);
            show_message("成功", res->status);
        } else {
            gtk_label_set_text(GTK_LABEL(g_status_label), res->status);
            show_message("错误", res->status);
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
    int count = do_perform_backup(mode);
    ThreadResult *res = calloc(1, sizeof(ThreadResult));
    res->backup_count = count;
    res->is_restore = 0;
    snprintf(res->status, sizeof(res->status), "备份完成：已备份 %d 个文件", count);
    g_idle_add(on_thread_finished, res);
    return NULL;
}

static gpointer restore_thread_func(gpointer data) {
    char *filename = (char*)data;
    int success = restore_file(filename);
    ThreadResult *res = calloc(1, sizeof(ThreadResult));
    res->is_restore = 1;
    res->success = success;
    if (success == 0) {
        snprintf(res->status, sizeof(res->status), "还原成功：%s", filename);
    } else {
        snprintf(res->status, sizeof(res->status), "还原失败：%s", filename);
    }
    g_idle_add(on_thread_finished, res);
    free(filename);
    return NULL;
}

static void start_backup_async(int mode) {
    gtk_widget_set_sensitive(g_backup_btn, FALSE);
    gtk_widget_set_sensitive(g_restore_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(g_status_label), "正在备份...");
    GThread *thread = g_thread_new("backup", backup_thread_func, GINT_TO_POINTER(mode));
    g_thread_unref(thread);
}

static void start_restore_async(const char *filename) {
    gtk_widget_set_sensitive(g_backup_btn, FALSE);
    gtk_widget_set_sensitive(g_restore_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(g_status_label), "正在还原...");
    char *copy = strdup(filename);
    GThread *thread = g_thread_new("restore", restore_thread_func, copy);
    g_thread_unref(thread);
}

// 刷新文件列表（按文件名降序）
void refresh_file_list() {
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

        // 按文件名降序排列（字典序，新日期在前）
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
    }
}

gboolean auto_backup_callback(gpointer user_data) {
    (void)user_data;
    if (g_config.auto_backup_enabled) {
        start_backup_async(g_config.backup_mode);
    }
    return G_SOURCE_CONTINUE;
}

void setup_auto_backup() {
    if (g_auto_timer_id > 0) {
        g_source_remove(g_auto_timer_id);
        g_auto_timer_id = 0;
    }
    if (g_config.auto_backup_enabled) {
        g_auto_timer_id = g_timeout_add_seconds(g_config.auto_backup_interval * 60,
                                                auto_backup_callback, NULL);
    }
}

// 按钮回调：备份
void on_backup_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    start_backup_async(g_config.backup_mode);
}

// 按钮回调：还原
void on_restore_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_list_box));
    if (!selected) {
        show_message("提示", "请先选择要还原的备份文件");
        return;
    }
    GtkWidget *child = gtk_list_box_row_get_child(selected);
    if (GTK_IS_LABEL(child)) {
        const char *filename = gtk_label_get_text(GTK_LABEL(child));
        if (filename) {
            start_restore_async(filename);
        }
    }
}

// 模式切换
void on_mode_changed(GtkDropDown *dropdown, gpointer user_data) {
    (void)user_data;
    guint selected = gtk_drop_down_get_selected(dropdown);
    g_config.backup_mode = selected;
    gtk_widget_set_sensitive(g_backup_btn, selected == 0);
    if (selected == 1) {
        gtk_label_set_text(GTK_LABEL(g_status_label), "非铁人模式：仅备份 autosave.hoi4");
    } else {
        gtk_label_set_text(GTK_LABEL(g_status_label), "普通模式：可备份所有 .hoi4 文件");
    }
}

// 间隔改变
void on_interval_changed(GtkSpinButton *spin, gpointer user_data) {
    (void)user_data;
    g_config.auto_backup_interval = gtk_spin_button_get_value_as_int(spin);
    setup_auto_backup();
}

// 最大备份数改变
void on_max_count_changed(GtkSpinButton *spin, gpointer user_data) {
    (void)user_data;
    g_config.max_backup_count = gtk_spin_button_get_value_as_int(spin);
}

// 自动备份开关
void on_auto_backup_toggled(GtkCheckButton *check, gpointer user_data) {
    (void)user_data;
    g_config.auto_backup_enabled = gtk_check_button_get_active(check);
    setup_auto_backup();
}

// 过滤游戏内备份开关
void on_filter_backup_toggled(GtkCheckButton *check, gpointer user_data) {
    (void)user_data;
    g_config.filter_game_backup = gtk_check_button_get_active(check);
}

// 刷新列表
void on_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    refresh_file_list();
    gtk_label_set_text(GTK_LABEL(g_status_label), "已刷新文件列表");
}

// 选择备份目录
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
                create_backup_directory(g_config.backup_path);

                char display[PATH_SIZE + 20];
                snprintf(display, sizeof(display), "备份目录：%s", g_config.backup_path);
                gtk_label_set_text(GTK_LABEL(g_backup_path_label), display);
                refresh_file_list();
                g_free(path);
            }
            g_object_unref(file);
        }
    }
    g_object_unref(native_dialog);
}

// “浏览...”按钮回调
void on_choose_backup_dir_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    GtkFileChooserNative *native = gtk_file_chooser_native_new("选择备份目录",
                                                               GTK_WINDOW(g_main_window),
                                                               GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                               "确定",
                                                               "取消");
    g_signal_connect(native, "response", G_CALLBACK(choose_backup_dir_response_cb), NULL);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(native));
}

// 自定义消息对话框
static void custom_dialog_close_cb(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *dialog = GTK_WINDOW(user_data);
    gtk_window_destroy(dialog);
}

static void show_message(const char *title, const char *message) {
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

    GtkWidget *ok_btn = gtk_button_new_with_label("确定");
    g_signal_connect(ok_btn, "clicked", G_CALLBACK(custom_dialog_close_cb), dialog);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);

    gtk_window_present(GTK_WINDOW(dialog));
}

// 创建主窗口
static void create_main_window(void) {
    g_main_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(g_main_window), "HOI存档备份工具");
    gtk_window_set_default_size(GTK_WINDOW(g_main_window), 800, 600);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_window_set_child(GTK_WINDOW(g_main_window), main_box);
    gtk_widget_set_margin_start(main_box, 10);
    gtk_widget_set_margin_end(main_box, 10);
    gtk_widget_set_margin_top(main_box, 10);
    gtk_widget_set_margin_bottom(main_box, 10);

    // 源目录
    GtkWidget *path_label = gtk_label_new(NULL);
    char path_text[PATH_SIZE];
    snprintf(path_text, sizeof(path_text), "源目录：%s", g_config.source_path);
    gtk_label_set_text(GTK_LABEL(path_label), path_text);
    gtk_label_set_xalign(GTK_LABEL(path_label), 0.0);
    gtk_box_append(GTK_BOX(main_box), path_label);

    // 备份目录（可更改）
    GtkWidget *backup_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(main_box), backup_row);
    g_backup_path_label = gtk_label_new(NULL);
    char backup_text[PATH_SIZE + 20];
    snprintf(backup_text, sizeof(backup_text), "备份目录：%s", g_config.backup_path);
    gtk_label_set_text(GTK_LABEL(g_backup_path_label), backup_text);
    gtk_label_set_xalign(GTK_LABEL(g_backup_path_label), 0.0);
    gtk_widget_set_hexpand(g_backup_path_label, TRUE);
    gtk_box_append(GTK_BOX(backup_row), g_backup_path_label);
    GtkWidget *browse_btn = gtk_button_new_with_label("浏览...");
    g_signal_connect(browse_btn, "clicked", G_CALLBACK(on_choose_backup_dir_clicked), NULL);
    gtk_box_append(GTK_BOX(backup_row), browse_btn);

    // 第一行控制
    GtkWidget *control_box1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(main_box), control_box1);

    GtkWidget *mode_label = gtk_label_new("模式：");
    gtk_box_append(GTK_BOX(control_box1), mode_label);

    const char *modes[] = {"普通模式", "非铁人模式", NULL};
    GtkStringList *mode_list = gtk_string_list_new(modes);
    g_mode_dropdown = gtk_drop_down_new(G_LIST_MODEL(mode_list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_mode_dropdown), 0);
    g_signal_connect(g_mode_dropdown, "notify::selected", G_CALLBACK(on_mode_changed), NULL);
    gtk_box_append(GTK_BOX(control_box1), g_mode_dropdown);

    g_backup_btn = gtk_button_new_with_label("立即备份");
    g_signal_connect(g_backup_btn, "clicked", G_CALLBACK(on_backup_clicked), NULL);
    gtk_box_append(GTK_BOX(control_box1), g_backup_btn);

    g_restore_btn = gtk_button_new_with_label("还原选中备份");
    g_signal_connect(g_restore_btn, "clicked", G_CALLBACK(on_restore_clicked), NULL);
    gtk_box_append(GTK_BOX(control_box1), g_restore_btn);

    g_refresh_btn = gtk_button_new_with_label("刷新列表");
    g_signal_connect(g_refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    gtk_box_append(GTK_BOX(control_box1), g_refresh_btn);

    // 第二行控制（自动备份与过滤）
    GtkWidget *control_box2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(main_box), control_box2);

    g_auto_backup_check = gtk_check_button_new_with_label("启用自动备份");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_auto_backup_check), g_config.auto_backup_enabled);
    g_signal_connect(g_auto_backup_check, "toggled", G_CALLBACK(on_auto_backup_toggled), NULL);
    gtk_box_append(GTK_BOX(control_box2), g_auto_backup_check);

    GtkWidget *interval_label = gtk_label_new("间隔(分钟)：");
    gtk_box_append(GTK_BOX(control_box2), interval_label);

    g_interval_spin = gtk_spin_button_new_with_range(1, 1440, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_interval_spin), g_config.auto_backup_interval);
    g_signal_connect(g_interval_spin, "value-changed", G_CALLBACK(on_interval_changed), NULL);
    gtk_box_append(GTK_BOX(control_box2), g_interval_spin);

    GtkWidget *max_label = gtk_label_new("最大备份数：");
    gtk_box_append(GTK_BOX(control_box2), max_label);

    g_max_count_spin = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_max_count_spin), g_config.max_backup_count);
    g_signal_connect(g_max_count_spin, "value-changed", G_CALLBACK(on_max_count_changed), NULL);
    gtk_box_append(GTK_BOX(control_box2), g_max_count_spin);

    // 过滤游戏内备份复选框
    g_filter_backup_check = gtk_check_button_new_with_label("过滤游戏内备份");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_filter_backup_check), g_config.filter_game_backup);
    g_signal_connect(g_filter_backup_check, "toggled", G_CALLBACK(on_filter_backup_toggled), NULL);
    gtk_box_append(GTK_BOX(control_box2), g_filter_backup_check);

    // 备份列表
    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    g_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_list_box), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(g_list_box), FALSE);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), g_list_box);
    gtk_box_append(GTK_BOX(main_box), scrolled_window);

    // 状态栏
    g_status_label = gtk_label_new("就绪");
    gtk_label_set_xalign(GTK_LABEL(g_status_label), 0.0);
    gtk_box_append(GTK_BOX(main_box), g_status_label);

    refresh_file_list();
}

static void app_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    init_config();
    create_main_window();
    setup_auto_backup();
    gtk_window_set_application(GTK_WINDOW(g_main_window), app);
    gtk_window_present(GTK_WINDOW(g_main_window));
}

int main(void) {
    g_setenv("GDK_PIXBUF_MODULEDIR", "lib\\gdk-pixbuf-2.0\\2.10.0\\loaders", TRUE);                  //缓存相关
    g_setenv("GDK_PIXBUF_MODULE_FILE", "lib\\gdk-pixbuf-2.0\\2.10.0\\loaders.cache", TRUE);    //缓存
    setlocale(LC_ALL, "zh_CN.UTF-8");
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    GtkApplication *app = gtk_application_new("com.example.hoibackup", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(app_activate), NULL);
    int ret = g_application_run(G_APPLICATION(app), 0, NULL);
    if (g_auto_timer_id > 0) {
        g_source_remove(g_auto_timer_id);
    }
    g_object_unref(app);
    return ret;
}
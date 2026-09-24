#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

/* Esta interfaz lanza los tres ejecutables existentes; no implementa otro codec. */
typedef struct {
    GtkWidget *window, *source_label, *status, *button, *grid;
    char *source, *root;
} App;
typedef struct {
    char *source, *root;
    guint workers;
    struct {
        gboolean ok;
        double compress_time, decompress_time;
        guint64 original, compressed;
        guint files, verified;
        char *message;
    } row[3];
    char *output;
} Run;
static const char *names[] = {"serial", "fork", "pthread"};

static gboolean sum_dir(const char *path, guint64 *total, guint *count, GError **error) {
    GDir *dir = g_dir_open(path, 0, error);
    if (!dir) return FALSE;
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        char *file = g_build_filename(path, name, NULL);
        GStatBuf st;
        if (g_stat(file, &st) == 0 && S_ISREG(st.st_mode)) {
            *total += (guint64)st.st_size;
            ++*count;
        }
        g_free(file);
    }
    g_dir_close(dir);
    return TRUE;
}
static gboolean file_equal(const char *a, const char *b, GError **error) {
    gboolean equal = FALSE;
    GFile *fa = g_file_new_for_path(a), *fb = g_file_new_for_path(b);
    GFileInputStream *ia = g_file_read(fa, NULL, error);
    GFileInputStream *ib = ia ? g_file_read(fb, NULL, error) : NULL;
    if (ia && ib) {
        equal = TRUE;
        for (;;) {
            char x[16384], y[16384];
            gssize nx = g_input_stream_read(G_INPUT_STREAM(ia), x, sizeof x, NULL, error);
            if (nx < 0) { equal = FALSE; break; }
            gssize ny = g_input_stream_read(G_INPUT_STREAM(ib), y, sizeof y, NULL, error);
            if (ny < 0 || nx != ny || (nx && memcmp(x, y, (size_t)nx))) { equal = FALSE; break; }
            if (!nx) break;
        }
    }
    if (ia) g_object_unref(ia);
    if (ib) g_object_unref(ib);
    g_object_unref(fa); g_object_unref(fb);
    return equal;
}
static gboolean compare_dirs(const char *original, const char *restored, GError **error) {
    guint64 a = 0, b = 0;
    guint na = 0, nb = 0;
    if (!sum_dir(original, &a, &na, error) || !sum_dir(restored, &b, &nb, error)) return FALSE;
    if (na != nb || a != b) return FALSE;
    GDir *dir = g_dir_open(original, 0, error);
    if (!dir) return FALSE;
    gboolean ok = TRUE;
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        char *p = g_build_filename(original, name, NULL);
        char *q = g_build_filename(restored, name, NULL);
        GStatBuf st;
        if (g_stat(p, &st) == 0 && S_ISREG(st.st_mode) && !file_equal(p, q, error)) ok = FALSE;
        g_free(p); g_free(q);
        if (!ok) break;
    }
    g_dir_close(dir);
    return ok;
}
static gboolean launch(const char *binary, const char *action, const char *input,
                       const char *output, guint workers, double *seconds, char **message) {
    GError *error = NULL;
    char number[16];
    g_snprintf(number, sizeof number, "%u", workers);
    GSubprocess *process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE, &error, binary, action, input, output,
        workers ? number : NULL, NULL);
    if (!process) { *message = g_strdup(error->message); g_clear_error(&error); return FALSE; }
    gint64 start = g_get_monotonic_time();
    char *out = NULL, *err = NULL;
    gboolean completed = g_subprocess_communicate_utf8(process, NULL, NULL, &out, &err, &error);
    *seconds = (g_get_monotonic_time() - start) / 1000000.0;
    gboolean ok = completed && g_subprocess_get_successful(process);
    if (!ok) *message = g_strdup(error ? error->message : (err && *err ? err : "El proceso terminó con error"));
    g_clear_error(&error); g_free(out); g_free(err); g_object_unref(process);
    return ok;
}
static void execute(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel) {
    (void)source; (void)cancel;
    Run *run = task_data;
    GError *error = NULL;
    guint count = 0;
    guint64 bytes = 0;
    if (!sum_dir(run->source, &bytes, &count, &error) || !count) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Directorio vacío o inaccesible: %s",
                                error ? error->message : "sin archivos regulares");
        g_clear_error(&error); return;
    }
    char *template = g_build_filename(run->root, "comparacion-XXXXXX", NULL);
    run->output = g_mkdtemp(template);
    if (!run->output) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "No se pudo crear la carpeta de resultados");
        g_free(template); return;
    }
    for (guint i = 0; i < 3; i++) {
        char *binary = g_build_filename(run->root, "bin", names[i], NULL);
        char *archive_name = g_strconcat(names[i], ".huf", NULL);
        char *archive = g_build_filename(run->output, archive_name, NULL);
        char *restored = g_build_filename(run->output, names[i], NULL);
        char *why = NULL;
        run->row[i].original = bytes;
        run->row[i].files = count;
        guint w = i == 0 ? 0 : run->workers;
        gboolean ok = launch(binary, "comprimir", run->source, archive, w,
                             &run->row[i].compress_time, &why);
        GStatBuf st;
        if (ok && g_stat(archive, &st) == 0) run->row[i].compressed = (guint64)st.st_size;
        else if (ok) { ok = FALSE; why = g_strdup("No aparece el contenedor comprimido"); }
        if (ok) ok = launch(binary, "descomprimir", archive, restored, w,
                            &run->row[i].decompress_time, &why);
        /* unpackone escribe cada archivo solo después de verificar su MD5. */
        guint64 restored_bytes = 0;
        if (g_file_test(restored, G_FILE_TEST_IS_DIR)) {
            GError *count_error = NULL;
            if (!sum_dir(restored, &restored_bytes, &run->row[i].verified, &count_error)) {
                g_clear_error(&count_error);
                run->row[i].verified = 0;
            }
        }
        if (ok && !compare_dirs(run->source, restored, &error)) {
            ok = FALSE;
            why = g_strdup(error ? error->message : "Los archivos restaurados son distintos");
            g_clear_error(&error);
        }
        run->row[i].ok = ok;
        run->row[i].message = why;
        g_free(binary); g_free(archive_name); g_free(archive); g_free(restored);
    }
    g_task_return_boolean(task, TRUE);
}
static void free_run(gpointer data) {
    Run *run = data;
    g_free(run->source); g_free(run->root);
    for (guint i = 0; i < 3; i++) g_free(run->row[i].message);
    g_free(run->output); g_free(run);
}
static void cell(GtkWidget *grid, guint column, guint row, const char *value) {
    GtkWidget *label = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_margin_start(label, 8);
    gtk_widget_set_margin_end(label, 8);
    gtk_widget_set_margin_top(label, 6);
    gtk_widget_set_margin_bottom(label, 6);
    gtk_grid_attach(GTK_GRID(grid), label, (int)column, (int)row, 1, 1);
}
static void completed(GObject *object, GAsyncResult *result, gpointer data) {
    (void)object;
    App *app = data;
    GTask *task = G_TASK(result);
    Run *run = g_task_get_task_data(task);
    GError *error = NULL;
    if (!g_task_propagate_boolean(task, &error)) {
        gtk_label_set_text(GTK_LABEL(app->status), error->message);
        g_clear_error(&error);
        gtk_widget_set_sensitive(app->button, TRUE);
        gtk_window_set_deletable(GTK_WINDOW(app->window), TRUE);
        g_application_release(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(app->window))));
        return;
    }
    GtkWidget *grid = app->grid;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(grid))) gtk_grid_remove(GTK_GRID(grid), child);
    const char *head[] = {"Versión", "Estado", "Salud MD5", "Comprimir (s)", "Descomprimir (s)",
                          "Mejora C / D", "Original (bytes)", "HUF (bytes)", "Razón original/HUF"};
    for (guint j = 0; j < G_N_ELEMENTS(head); ++j) cell(grid, j, 0, head[j]);
    for (guint i = 0; i < 3; ++i) {
        char *tmp;
        cell(grid, 0, i + 1, names[i]);
        cell(grid, 1, i + 1, run->row[i].ok ? "Correcto" : "Error");
        tmp = g_strdup_printf("%u/%u (%.1f%%)", run->row[i].verified, run->row[i].files,
            run->row[i].files ? 100.0 * run->row[i].verified / run->row[i].files : 0.0);
        cell(grid, 2, i + 1, tmp); g_free(tmp);
        tmp = g_strdup_printf("%.3f", run->row[i].compress_time); cell(grid, 3, i + 1, tmp); g_free(tmp);
        tmp = g_strdup_printf("%.3f", run->row[i].decompress_time); cell(grid, 4, i + 1, tmp); g_free(tmp);
        if (i && run->row[i].ok && run->row[0].ok && run->row[0].compress_time > 0 && run->row[0].decompress_time > 0)
            tmp = g_strdup_printf("%.1f%% / %.1f%%",
                100 * (1 - run->row[i].compress_time / run->row[0].compress_time),
                100 * (1 - run->row[i].decompress_time / run->row[0].decompress_time));
        else tmp = g_strdup("—");
        cell(grid, 5, i + 1, tmp); g_free(tmp);
        tmp = g_strdup_printf("%" G_GUINT64_FORMAT, run->row[i].original); cell(grid, 6, i + 1, tmp); g_free(tmp);
        tmp = g_strdup_printf("%" G_GUINT64_FORMAT, run->row[i].compressed); cell(grid, 7, i + 1, tmp); g_free(tmp);
        tmp = run->row[i].compressed ? g_strdup_printf("%.3f:1", (double)run->row[i].original / run->row[i].compressed) : g_strdup("—");
        cell(grid, 8, i + 1, tmp); g_free(tmp);
    }
    GString *summary = g_string_new(NULL);
    g_string_append_printf(summary, "Resultados guardados en: %s", run->output);
    for (guint i = 0; i < 3; ++i)
        if (!run->row[i].ok)
            g_string_append_printf(summary, "\n%s: %s", names[i],
                                   run->row[i].message ? run->row[i].message : "Archivos distintos");
    gtk_label_set_text(GTK_LABEL(app->status), summary->str);
    g_string_free(summary, TRUE);
    gtk_widget_set_sensitive(app->button, TRUE);
    gtk_window_set_deletable(GTK_WINDOW(app->window), TRUE);
    g_application_release(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(app->window))));
}
static void run_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (!app->source) { gtk_label_set_text(GTK_LABEL(app->status), "Elegí primero el directorio con los archivos."); return; }
    Run *run = g_new0(Run, 1);
    run->source = g_strdup(app->source);
    run->root = g_strdup(app->root);
    run->workers = 4;
    gtk_widget_set_sensitive(app->button, FALSE);
    gtk_window_set_deletable(GTK_WINDOW(app->window), FALSE);
    g_application_hold(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(app->window))));
    gtk_label_set_text(GTK_LABEL(app->status), "Trabajando: tres compresiones, tres descompresiones y comprobación byte a byte. Esperá...");
    GTask *task = g_task_new(NULL, NULL, completed, app);
    g_task_set_task_data(task, run, free_run);
    g_task_run_in_thread(task, execute);
    g_object_unref(task);
}
static void picked(GObject *object, GAsyncResult *result, gpointer data) {
    App *app = data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(object), result, &error);
    if (file) {
        g_free(app->source);
        app->source = g_file_get_path(file);
        gtk_label_set_text(GTK_LABEL(app->source_label), app->source ? app->source : "Seleccioná un directorio local");
        g_object_unref(file);
    }
    g_clear_error(&error);
}
static void choose(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Elegir directorio de archivos originales");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(app->window), NULL, picked, app);
    g_object_unref(dialog);
}
static void activate(GtkApplication *application, gpointer data) {
    App *app = data;
    app->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app->window), "Compresor Huffman — comparación");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1100, 430);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 18); gtk_widget_set_margin_bottom(box, 18);
    gtk_window_set_child(GTK_WINDOW(app->window), box);
    GtkWidget *choose_button = gtk_button_new_with_label("Elegir carpeta de archivos originales");
    g_signal_connect(choose_button, "clicked", G_CALLBACK(choose), app);
    gtk_box_append(GTK_BOX(box), choose_button);
    app->source_label = gtk_label_new("Sin carpeta seleccionada");
    gtk_label_set_xalign(GTK_LABEL(app->source_label), 0);
    gtk_label_set_wrap(GTK_LABEL(app->source_label), TRUE);
    gtk_box_append(GTK_BOX(box), app->source_label);
    app->button = gtk_button_new_with_label("Comprimir, descomprimir y comparar (4 trabajadores)");
    g_signal_connect(app->button, "clicked", G_CALLBACK(run_clicked), app);
    gtk_box_append(GTK_BOX(box), app->button);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), scroll);
    app->grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(app->grid), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app->grid);
    app->status = gtk_label_new("Los resultados se guardarán en una carpeta nueva dentro del proyecto.");
    gtk_label_set_xalign(GTK_LABEL(app->status), 0);
    gtk_label_set_wrap(GTK_LABEL(app->status), TRUE);
    gtk_box_append(GTK_BOX(box), app->status);
    gtk_window_present(GTK_WINDOW(app->window));
}
int main(int argc, char **argv) {
    App app = {0};
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (!exe) { g_printerr("No se pudo localizar el ejecutable.\n"); return 1; }
    char *dir = g_path_get_dirname(exe);
    app.root = g_path_get_dirname(dir);
    g_free(exe); g_free(dir);
    GtkApplication *application = gtk_application_new("org.proyecto1.huffman", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &app);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    g_free(app.source); g_free(app.root);
    return status;
}

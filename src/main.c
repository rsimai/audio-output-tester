#include <alsa/asoundlib.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWidget *window;
    GtkWidget *device_list;
    GtkWidget *filter_entry;
    GtkWidget *status_label;
    GtkWidget *hide_names_check;
    char *default_device;
    char *last_snapshot;
    gboolean using_pulse;
    gboolean hide_technical_names;
    guint refresh_source_id;
} AppState;

static GtkWidget *build_device_row(AppState *app,
                                   const char *device_name,
                                   const char *display_name,
                                   const char *desc,
                                   const char *backend);
static int play_test_tone(const char *device_name);
static void load_devices(AppState *app);

static void choose_visible_labels(AppState *app,
                                  const char *device_name,
                                  const char *preferred_display,
                                  const char *preferred_desc,
                                  const char *fallback_desc,
                                  const char **display_out,
                                  const char **desc_out) {
    const char *display = preferred_display;
    if (!display || display[0] == '\0') {
        display = device_name;
    }

    const char *desc = preferred_desc;
    if (!desc) {
        desc = "";
    }

    if (app->hide_technical_names) {
        if (display == device_name && preferred_desc && preferred_desc[0] != '\0') {
            display = preferred_desc;
        }

        desc = fallback_desc ? fallback_desc : "";
    }

    *display_out = display;
    *desc_out = desc;
}

static void set_small_label_markup(GtkWidget *label, const char *text) {
    gchar *escaped = g_markup_escape_text(text ? text : "", -1);
    gchar *markup = g_strdup_printf("<small>%s</small>", escaped);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    g_free(escaped);
}

static void set_status(AppState *app, const char *message) {
    gtk_label_set_text(GTK_LABEL(app->status_label), message);
}

static gboolean run_command_capture(const char *command, gchar **stdout_text) {
    gchar *stderr_text = NULL;
    gint exit_status = 0;
    gboolean ok = g_spawn_command_line_sync(command,
                                            stdout_text,
                                            &stderr_text,
                                            &exit_status,
                                            NULL);

    if (stderr_text) {
        g_free(stderr_text);
    }

    return ok && exit_status == 0;
}

static gchar *get_asoundrc_path(void) {
    return g_build_filename(g_get_home_dir(), ".asoundrc", NULL);
}

static gchar *get_default_pulse_sink(void) {
    gchar *output = NULL;

    if (run_command_capture("pactl get-default-sink", &output) && output) {
        g_strstrip(output);
        if (output[0] != '\0') {
            return output;
        }
        g_free(output);
    }

    g_free(output);
    output = NULL;

    if (!run_command_capture("pactl info", &output) || !output) {
        g_free(output);
        return NULL;
    }

    gchar **lines = g_strsplit(output, "\n", -1);
    gchar *result = NULL;

    for (int i = 0; lines[i] != NULL; i++) {
        if (g_str_has_prefix(lines[i], "Default Sink:")) {
            const char *value = lines[i] + strlen("Default Sink:");
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            if (*value != '\0') {
                result = g_strdup(value);
            }
            break;
        }
    }

    g_strfreev(lines);
    g_free(output);
    return result;
}

static gboolean set_default_pulse_sink(const char *sink_name) {
    gchar *quoted = g_shell_quote(sink_name);
    gchar *command = g_strconcat("pactl set-default-sink ", quoted, NULL);
    gboolean ok = run_command_capture(command, NULL);
    g_free(command);
    g_free(quoted);
    return ok;
}

static int play_test_tone_on_pulse_sink(const char *sink_name) {
    const gchar *old_sink = g_getenv("PULSE_SINK");
    gchar *old_sink_copy = old_sink ? g_strdup(old_sink) : NULL;

    if (sink_name && sink_name[0] != '\0') {
        g_setenv("PULSE_SINK", sink_name, TRUE);
    } else {
        g_unsetenv("PULSE_SINK");
    }

    int err = play_test_tone("pulse");

    if (old_sink_copy) {
        g_setenv("PULSE_SINK", old_sink_copy, TRUE);
    } else {
        g_unsetenv("PULSE_SINK");
    }

    g_free(old_sink_copy);
    return err;
}

static gchar *extract_default_device_from_block(const char *block) {
    if (!block) {
        return NULL;
    }

    const char *needle = strstr(block, "slave.pcm");
    if (!needle) {
        needle = strstr(block, "pcm");
        if (!needle) {
            return NULL;
        }
    }

    const char *quote_start = strchr(needle, '"');
    if (quote_start) {
        quote_start++;
        const char *quote_end = strchr(quote_start, '"');
        if (quote_end && quote_end > quote_start) {
            return g_strndup(quote_start, (gsize)(quote_end - quote_start));
        }
    }

    const char *line_end = strchr(needle, '\n');
    const char *value_start = strchr(needle, ' ');
    if (!value_start) {
        return NULL;
    }

    while (*value_start == ' ' || *value_start == '\t') {
        value_start++;
    }

    const char *value_end = line_end ? line_end : (needle + strlen(needle));
    while (value_end > value_start &&
           (value_end[-1] == ' ' || value_end[-1] == '\t' || value_end[-1] == '\r')) {
        value_end--;
    }

    if (value_end <= value_start) {
        return NULL;
    }

    return g_strndup(value_start, (gsize)(value_end - value_start));
}

static gchar *read_current_default_device(void) {
    gchar *path = get_asoundrc_path();
    gchar *contents = NULL;
    gsize length = 0;

    if (!g_file_get_contents(path, &contents, &length, NULL)) {
        g_free(path);
        return NULL;
    }

    g_free(path);

    const char *managed_start = strstr(contents, "# BEGIN audio-device-tester default");
    const char *managed_end = strstr(contents, "# END audio-device-tester default");

    if (managed_start && managed_end && managed_end > managed_start) {
        gchar *managed_block = g_strndup(managed_start, (gsize)(managed_end - managed_start));
        gchar *device = extract_default_device_from_block(managed_block);
        g_free(managed_block);
        g_free(contents);
        return device;
    }

    const char *default_start = strstr(contents, "pcm.!default");
    if (!default_start) {
        g_free(contents);
        return NULL;
    }

    const char *open_brace = strchr(default_start, '{');
    if (open_brace) {
        int depth = 0;
        const char *cursor = open_brace;
        while (*cursor) {
            if (*cursor == '{') {
                depth++;
            } else if (*cursor == '}') {
                depth--;
                if (depth == 0) {
                    break;
                }
            }
            cursor++;
        }

        if (*cursor == '}') {
            gchar *block = g_strndup(default_start, (gsize)(cursor - default_start + 1));
            gchar *device = extract_default_device_from_block(block);
            g_free(block);
            g_free(contents);
            return device;
        }
    }

    const char *line_end = strchr(default_start, '\n');
    gchar *line = line_end ? g_strndup(default_start, (gsize)(line_end - default_start)) : g_strdup(default_start);
    gchar *device = NULL;
    char *space = strrchr(line, ' ');
    if (space) {
        space++;
        while (*space == ' ' || *space == '\t') {
            space++;
        }
        if (*space) {
            device = g_strdup(space);
        }
    }

    g_free(line);
    g_free(contents);
    return device;
}

static gboolean write_default_device(const char *device_name, GError **error) {
    gchar *path = get_asoundrc_path();
    gchar *contents = NULL;
    gsize length = 0;

    if (!g_file_get_contents(path, &contents, &length, NULL)) {
        contents = g_strdup("");
    }

    const char *begin_marker = "# BEGIN audio-device-tester default";
    const char *end_marker = "# END audio-device-tester default";

    char *start = strstr(contents, begin_marker);
    char *end = strstr(contents, end_marker);
    gchar *cleaned = NULL;

    if (start && end && end > start) {
        end += strlen(end_marker);
        while (*end == '\n' || *end == '\r') {
            end++;
        }

        gchar *prefix = g_strndup(contents, (gsize)(start - contents));
        gchar *suffix = g_strdup(end);
        cleaned = g_strconcat(prefix, suffix, NULL);
        g_free(prefix);
        g_free(suffix);
    } else {
        cleaned = g_strdup(contents);
    }

    g_free(contents);

    gchar *escaped_device = g_strescape(device_name, NULL);
    gchar *managed_block = g_strdup_printf(
        "%s\n"
        "pcm.!default {\n"
        "    type plug\n"
        "    slave.pcm \"%s\"\n"
        "}\n"
        "%s\n",
        begin_marker,
        escaped_device,
        end_marker);

    gchar *final_contents = NULL;
    if (cleaned[0] == '\0') {
        final_contents = g_strdup(managed_block);
    } else {
        final_contents = g_strconcat(cleaned, "\n", managed_block, NULL);
    }

    gboolean ok = g_file_set_contents(path, final_contents, -1, error);

    g_free(path);
    g_free(cleaned);
    g_free(escaped_device);
    g_free(managed_block);
    g_free(final_contents);

    return ok;
}

static void update_default_buttons(AppState *app) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->device_list));
    for (GList *iter = children; iter != NULL; iter = iter->next) {
        GtkWidget *row = GTK_WIDGET(iter->data);
        const char *name = (const char *)g_object_get_data(G_OBJECT(row), "device-name");
        GtkWidget *button = GTK_WIDGET(g_object_get_data(G_OBJECT(row), "set-default-button"));

        if (!button || !name) {
            continue;
        }

        gboolean is_default = app->default_device && strcmp(app->default_device, name) == 0;
        gtk_widget_set_sensitive(button, !is_default);
        gtk_button_set_label(GTK_BUTTON(button), is_default ? "Default" : "Set Default");
    }
    g_list_free(children);
}

static int load_pulse_sinks(AppState *app) {
    gchar *output = NULL;

    if (!run_command_capture("pactl list sinks", &output) || !output) {
        g_free(output);
        return -1;
    }

    gchar **lines = g_strsplit(output, "\n", -1);
    int added = 0;
    gchar *current_name = NULL;
    gchar *current_desc = NULL;

    for (int i = 0; lines[i] != NULL; i++) {
        char *line = lines[i];
        while (*line == ' ' || *line == '\t') {
            line++;
        }

        if (g_str_has_prefix(line, "Sink #")) {
            if (current_name) {
                const char *display_name =
                    (current_desc && current_desc[0] != '\0') ? current_desc : current_name;
                const char *subtitle =
                    (current_desc && current_desc[0] != '\0') ? current_name : "PipeWire/Pulse sink";
                const char *visible_name = NULL;
                const char *visible_desc = NULL;
                choose_visible_labels(app,
                                      current_name,
                                      display_name,
                                      subtitle,
                                      "PipeWire/Pulse sink",
                                      &visible_name,
                                      &visible_desc);
                GtkWidget *row = build_device_row(app,
                                                  current_name,
                                                  visible_name,
                                                  visible_desc,
                                                  "pulse");
                gtk_container_add(GTK_CONTAINER(app->device_list), row);
                added++;
            }

            g_clear_pointer(&current_name, g_free);
            g_clear_pointer(&current_desc, g_free);
            continue;
        }

        if (g_str_has_prefix(line, "Name:")) {
            const char *value = line + strlen("Name:");
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            g_free(current_name);
            current_name = g_strdup(value);
            continue;
        }

        if (g_str_has_prefix(line, "Description:")) {
            const char *value = line + strlen("Description:");
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            g_free(current_desc);
            current_desc = g_strdup(value);
        }
    }

    if (current_name) {
        const char *display_name =
            (current_desc && current_desc[0] != '\0') ? current_desc : current_name;
        const char *subtitle =
            (current_desc && current_desc[0] != '\0') ? current_name : "PipeWire/Pulse sink";
        const char *visible_name = NULL;
        const char *visible_desc = NULL;
        choose_visible_labels(app,
                              current_name,
                              display_name,
                              subtitle,
                              "PipeWire/Pulse sink",
                              &visible_name,
                              &visible_desc);
        GtkWidget *row = build_device_row(app,
                                          current_name,
                                          visible_name,
                                          visible_desc,
                                          "pulse");
        gtk_container_add(GTK_CONTAINER(app->device_list), row);
        added++;
    }

    g_free(current_name);
    g_free(current_desc);
    g_strfreev(lines);
    g_free(output);

    return added;
}

static int load_alsa_output_devices(AppState *app) {
    int card = -1;
    int added = 0;

    if (snd_card_next(&card) < 0 || card < 0) {
        return 0;
    }

    while (card >= 0) {
        char hw_name[32];
        snprintf(hw_name, sizeof(hw_name), "hw:%d", card);

        snd_ctl_t *ctl = NULL;
        if (snd_ctl_open(&ctl, hw_name, 0) < 0) {
            if (snd_card_next(&card) < 0) {
                break;
            }
            continue;
        }

        snd_ctl_card_info_t *card_info = NULL;
        snd_ctl_card_info_alloca(&card_info);
        if (snd_ctl_card_info(ctl, card_info) < 0) {
            snd_ctl_close(ctl);
            if (snd_card_next(&card) < 0) {
                break;
            }
            continue;
        }

        int dev = -1;
        while (snd_ctl_pcm_next_device(ctl, &dev) >= 0 && dev >= 0) {
            snd_pcm_info_t *pcminfo = NULL;
            snd_pcm_info_alloca(&pcminfo);
            snd_pcm_info_set_device(pcminfo, dev);
            snd_pcm_info_set_subdevice(pcminfo, 0);
            snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_PLAYBACK);

            if (snd_ctl_pcm_info(ctl, pcminfo) < 0) {
                continue;
            }

            char name[64];
            snprintf(name, sizeof(name), "hw:%d,%d", card, dev);

            char desc[256];
            snprintf(desc,
                     sizeof(desc),
                     "%s - %s",
                     snd_ctl_card_info_get_name(card_info),
                     snd_pcm_info_get_name(pcminfo));

            const char *visible_name = NULL;
            const char *visible_desc = NULL;
            choose_visible_labels(app,
                                  name,
                                  name,
                                  desc,
                                  "ALSA output device",
                                  &visible_name,
                                  &visible_desc);

            GtkWidget *row = build_device_row(app, name, visible_name, visible_desc, "alsa");
            gtk_container_add(GTK_CONTAINER(app->device_list), row);
            added++;
        }

        snd_ctl_close(ctl);

        if (snd_card_next(&card) < 0) {
            break;
        }
    }

    return added;
}

static gchar *build_pulse_snapshot(gchar **default_out) {
    gchar *output = NULL;
    if (!run_command_capture("pactl list short sinks", &output) || !output) {
        g_free(output);
        return NULL;
    }

    gchar *default_sink = get_default_pulse_sink();
    GString *snapshot = g_string_new("pulse\n");
    g_string_append_printf(snapshot, "default=%s\n", default_sink ? default_sink : "");

    gchar **lines = g_strsplit(output, "\n", -1);
    for (int i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] == '\0') {
            continue;
        }

        gchar **cols = g_strsplit(lines[i], "\t", 0);
        if (cols[1] && cols[1][0] != '\0') {
            g_string_append(snapshot, cols[1]);
            g_string_append_c(snapshot, '\n');
        }
        g_strfreev(cols);
    }

    g_strfreev(lines);
    g_free(output);

    if (default_out) {
        *default_out = default_sink;
    } else {
        g_free(default_sink);
    }

    return g_string_free(snapshot, FALSE);
}

static gchar *build_alsa_snapshot(gchar **default_out) {
    gchar *default_name = read_current_default_device();
    GString *snapshot = g_string_new("alsa\n");
    g_string_append_printf(snapshot, "default=%s\n", default_name ? default_name : "");

    int card = -1;
    if (snd_card_next(&card) >= 0) {
        while (card >= 0) {
            char hw_name[32];
            snprintf(hw_name, sizeof(hw_name), "hw:%d", card);

            snd_ctl_t *ctl = NULL;
            if (snd_ctl_open(&ctl, hw_name, 0) >= 0) {
                int dev = -1;
                while (snd_ctl_pcm_next_device(ctl, &dev) >= 0 && dev >= 0) {
                    snd_pcm_info_t *pcminfo = NULL;
                    snd_pcm_info_alloca(&pcminfo);
                    snd_pcm_info_set_device(pcminfo, dev);
                    snd_pcm_info_set_subdevice(pcminfo, 0);
                    snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_PLAYBACK);

                    if (snd_ctl_pcm_info(ctl, pcminfo) >= 0) {
                        g_string_append_printf(snapshot, "hw:%d,%d\n", card, dev);
                    }
                }
                snd_ctl_close(ctl);
            }

            if (snd_card_next(&card) < 0) {
                break;
            }
        }
    }

    if (default_out) {
        *default_out = default_name;
    } else {
        g_free(default_name);
    }

    return g_string_free(snapshot, FALSE);
}

static int play_test_tone(const char *device_name) {
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *params = NULL;

    const unsigned int sample_rate = 44100;
    const double frequency_hz = 880.0;
    const double duration_seconds = 1.0;
    const int channels = 1;
    const snd_pcm_uframes_t frames_per_chunk = 512;
    const int total_frames = (int)(sample_rate * duration_seconds);

    int err = snd_pcm_open(&pcm, device_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        return err;
    }

    snd_pcm_hw_params_alloca(&params);
    err = snd_pcm_hw_params_any(pcm, params);
    if (err < 0) {
        snd_pcm_close(pcm);
        return err;
    }

    err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        snd_pcm_close(pcm);
        return err;
    }

    err = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    if (err < 0) {
        snd_pcm_close(pcm);
        return err;
    }

    err = snd_pcm_hw_params_set_channels(pcm, params, channels);
    if (err < 0) {
        snd_pcm_close(pcm);
        return err;
    }

    unsigned int exact_rate = sample_rate;
    err = snd_pcm_hw_params_set_rate_near(pcm, params, &exact_rate, 0);
    if (err < 0) {
        snd_pcm_close(pcm);
        return err;
    }

    err = snd_pcm_hw_params(pcm, params);
    if (err < 0) {
        snd_pcm_close(pcm);
        return err;
    }

    int16_t *buffer = malloc(sizeof(int16_t) * frames_per_chunk * channels);
    if (!buffer) {
        snd_pcm_close(pcm);
        return -ENOMEM;
    }

    int frame_index = 0;
    while (frame_index < total_frames) {
        int chunk_frames = (total_frames - frame_index) > (int)frames_per_chunk
                               ? (int)frames_per_chunk
                               : (total_frames - frame_index);

        for (int i = 0; i < chunk_frames; i++) {
            double t = (double)(frame_index + i) / (double)sample_rate;
            double sample = sin(2.0 * M_PI * frequency_hz * t);
            buffer[i] = (int16_t)(sample * 22000.0);
        }

        snd_pcm_sframes_t written = snd_pcm_writei(pcm, buffer, (snd_pcm_uframes_t)chunk_frames);
        if (written == -EPIPE) {
            snd_pcm_prepare(pcm);
            continue;
        }
        if (written < 0) {
            free(buffer);
            snd_pcm_drain(pcm);
            snd_pcm_close(pcm);
            return (int)written;
        }

        frame_index += (int)written;
    }

    free(buffer);
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    return 0;
}

static void on_play_clicked(GtkButton *button, gpointer user_data) {
    AppState *app = (AppState *)user_data;
    const char *device = (const char *)g_object_get_data(G_OBJECT(button), "device-name");
    const char *backend = (const char *)g_object_get_data(G_OBJECT(button), "device-backend");

    if (!device) {
        set_status(app, "No device assigned to this button.");
        return;
    }

    char status[512];
    snprintf(status, sizeof(status), "Playing test tone on: %s", device);
    set_status(app, status);

    while (gtk_events_pending()) {
        gtk_main_iteration();
    }

    int err = 0;
    if (backend && strcmp(backend, "pulse") == 0) {
        err = play_test_tone_on_pulse_sink(device);
    } else {
        err = play_test_tone(device);
    }
    if (err < 0) {
        snprintf(status, sizeof(status), "Failed on %s: %s", device, snd_strerror(err));
        set_status(app, status);
        return;
    }

    snprintf(status, sizeof(status), "Test tone finished on: %s", device);
    set_status(app, status);
}

static void on_set_default_clicked(GtkButton *button, gpointer user_data) {
    AppState *app = (AppState *)user_data;
    const char *device = (const char *)g_object_get_data(G_OBJECT(button), "device-name");
    const char *backend = (const char *)g_object_get_data(G_OBJECT(button), "device-backend");

    if (!device || device[0] == '\0') {
        set_status(app, "No device assigned to this button.");
        return;
    }

    if (backend && strcmp(backend, "pulse") == 0) {
        if (!set_default_pulse_sink(device)) {
            char status[512];
            snprintf(status,
                     sizeof(status),
                     "Failed to set PipeWire/Pulse default sink to %s",
                     device);
            set_status(app, status);
            return;
        }
    } else {
        GError *error = NULL;
        if (!write_default_device(device, &error)) {
            char status[512];
            snprintf(status,
                     sizeof(status),
                     "Failed to write ~/.asoundrc for %s: %s",
                     device,
                     error ? error->message : "unknown error");
            set_status(app, status);
            if (error) {
                g_error_free(error);
            }
            return;
        }
    }

    g_free(app->default_device);
    app->default_device = g_strdup(device);
    update_default_buttons(app);

    char status[512];
    snprintf(status,
             sizeof(status),
             "Set %s default output to: %s",
             (backend && strcmp(backend, "pulse") == 0) ? "PipeWire/Pulse" : "ALSA",
             device);
    set_status(app, status);
}

static void clear_device_list(GtkWidget *list) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *iter = children; iter != NULL; iter = iter->next) {
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    }
    g_list_free(children);
}

static gboolean row_matches_filter(GtkListBoxRow *row, gpointer user_data) {
    AppState *app = (AppState *)user_data;
    const char *filter = gtk_entry_get_text(GTK_ENTRY(app->filter_entry));
    const char *name = (const char *)g_object_get_data(G_OBJECT(row), "device-name");
    const char *desc = (const char *)g_object_get_data(G_OBJECT(row), "device-desc");

    if (!filter || filter[0] == '\0') {
        return TRUE;
    }

    if (!name) {
        name = "";
    }
    if (!desc) {
        desc = "";
    }

    gchar *needle = g_utf8_strdown(filter, -1);
    gchar *name_lower = g_utf8_strdown(name, -1);
    gchar *desc_lower = g_utf8_strdown(desc, -1);

    gboolean matches = (strstr(name_lower, needle) != NULL) || (strstr(desc_lower, needle) != NULL);

    g_free(needle);
    g_free(name_lower);
    g_free(desc_lower);
    return matches;
}

static GtkWidget *build_device_row(AppState *app,
                                   const char *device_name,
                                   const char *display_name,
                                   const char *desc,
                                   const char *backend) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *name_label = gtk_label_new(display_name ? display_name : device_name);
    GtkWidget *desc_label = gtk_label_new(NULL);
    GtkWidget *default_button = gtk_button_new_with_label("Set Default");
    GtkWidget *play_button = gtk_button_new_with_label("Play Test Sound");

    gtk_widget_set_hexpand(name_label, TRUE);
    gtk_widget_set_halign(name_label, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0f);

    gtk_widget_set_hexpand(desc_label, TRUE);
    gtk_widget_set_halign(desc_label, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(desc_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(desc_label), PANGO_ELLIPSIZE_END);
    set_small_label_markup(desc_label, desc ? desc : "");

    GtkWidget *labels_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(labels_box), name_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(labels_box), desc_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), labels_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), default_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), play_button, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row), box);

    g_object_set_data_full(G_OBJECT(row), "device-name", g_strdup(device_name), g_free);
    g_object_set_data_full(G_OBJECT(row), "device-desc", g_strdup(desc ? desc : ""), g_free);
    g_object_set_data(G_OBJECT(row), "set-default-button", default_button);
    g_object_set_data(G_OBJECT(row), "play-button", play_button);
    g_object_set_data_full(G_OBJECT(default_button), "device-name", g_strdup(device_name), g_free);
    g_object_set_data_full(G_OBJECT(play_button), "device-name", g_strdup(device_name), g_free);
    g_object_set_data_full(G_OBJECT(default_button), "device-backend", g_strdup(backend ? backend : "alsa"), g_free);
    g_object_set_data_full(G_OBJECT(play_button), "device-backend", g_strdup(backend ? backend : "alsa"), g_free);
    g_signal_connect(default_button, "clicked", G_CALLBACK(on_set_default_clicked), app);
    g_signal_connect(play_button, "clicked", G_CALLBACK(on_play_clicked), app);

    gboolean is_default = app->default_device && strcmp(app->default_device, device_name) == 0;
    gtk_widget_set_sensitive(default_button, !is_default);
    if (is_default) {
        gtk_button_set_label(GTK_BUTTON(default_button), "Default");
    }

    return row;
}

static void load_devices(AppState *app) {
    gchar *next_default = NULL;
    gboolean next_using_pulse = TRUE;
    gchar *next_snapshot = build_pulse_snapshot(&next_default);

    if (!next_snapshot) {
        next_using_pulse = FALSE;
        next_snapshot = build_alsa_snapshot(&next_default);
    }

    if (!next_snapshot) {
        set_status(app, "Could not query audio outputs.");
        return;
    }

    if (app->last_snapshot && strcmp(app->last_snapshot, next_snapshot) == 0) {
        g_free(next_default);
        g_free(next_snapshot);
        return;
    }

    g_free(app->last_snapshot);
    app->last_snapshot = next_snapshot;

    g_free(app->default_device);
    app->default_device = next_default;
    app->using_pulse = next_using_pulse;

    clear_device_list(app->device_list);

    int added = app->using_pulse ? load_pulse_sinks(app) : load_alsa_output_devices(app);
    if (added < 0) {
        added = 0;
    }

    gtk_widget_show_all(app->device_list);
    update_default_buttons(app);

    char status[256];
    if (app->using_pulse) {
        snprintf(status, sizeof(status), "Loaded %d PipeWire/Pulse output sink(s).", added);
    } else {
        snprintf(status, sizeof(status), "Loaded %d ALSA hardware output device(s).", added);
    }
    set_status(app, status);
}

static gboolean on_auto_refresh(gpointer user_data) {
    AppState *app = (AppState *)user_data;
    load_devices(app);
    return G_SOURCE_CONTINUE;
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *app = (AppState *)user_data;
    load_devices(app);
}

static void on_filter_changed(GtkEditable *editable, gpointer user_data) {
    (void)editable;
    AppState *app = (AppState *)user_data;
    const char *text = gtk_entry_get_text(GTK_ENTRY(app->filter_entry));

    if (text && text[0] != '\0') {
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(app->filter_entry),
                                          GTK_ENTRY_ICON_SECONDARY,
                                          "edit-clear-symbolic");
    } else {
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(app->filter_entry),
                                          GTK_ENTRY_ICON_SECONDARY,
                                          NULL);
    }

    gtk_list_box_invalidate_filter(GTK_LIST_BOX(app->device_list));
}

static void on_filter_icon_press(GtkEntry *entry,
                                 GtkEntryIconPosition icon_pos,
                                 GdkEvent *event,
                                 gpointer user_data) {
    (void)event;
    (void)user_data;

    if (icon_pos == GTK_ENTRY_ICON_SECONDARY) {
        gtk_entry_set_text(entry, "");
    }
}

static void on_hide_names_toggled(GtkToggleButton *toggle, gpointer user_data) {
    AppState *app = (AppState *)user_data;
    app->hide_technical_names = gtk_toggle_button_get_active(toggle);

    /* Force a redraw even if device snapshot itself did not change. */
    g_clear_pointer(&app->last_snapshot, g_free);
    load_devices(app);
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    AppState app = {0};
    app.hide_technical_names = TRUE;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Audio Device Tester");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1230, 480);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(root), 10);
    gtk_container_add(GTK_CONTAINER(app.window), root);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title = gtk_label_new("Playback Devices");
    app.hide_names_check = gtk_check_button_new_with_label("Hide technical device names");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.hide_names_check), TRUE);

    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);

    gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(header), app.hide_names_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);

    app.filter_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.filter_entry), "Filter devices by name or description...");
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(app.filter_entry),
                                    GTK_ENTRY_ICON_SECONDARY,
                                    "Clear filter");
    gtk_box_pack_start(GTK_BOX(root), app.filter_entry, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    app.device_list = gtk_list_box_new();
    gtk_list_box_set_filter_func(GTK_LIST_BOX(app.device_list), row_matches_filter, &app, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), app.device_list);

    app.status_label = gtk_label_new("Ready");
    gtk_widget_set_halign(app.status_label, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(app.status_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app.status_label, FALSE, FALSE, 0);

    g_signal_connect(app.filter_entry, "changed", G_CALLBACK(on_filter_changed), &app);
    g_signal_connect(app.filter_entry, "icon-press", G_CALLBACK(on_filter_icon_press), &app);
    g_signal_connect(app.hide_names_check, "toggled", G_CALLBACK(on_hide_names_toggled), &app);

    load_devices(&app);
    app.refresh_source_id = g_timeout_add_seconds(2, on_auto_refresh, &app);

    gtk_widget_show_all(app.window);
    gtk_main();
    if (app.refresh_source_id != 0) {
        g_source_remove(app.refresh_source_id);
    }
    g_free(app.default_device);
    g_free(app.last_snapshot);
    return 0;
}

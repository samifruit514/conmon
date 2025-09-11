#define _GNU_SOURCE

#include "healthcheck.h"
#include "utils.h"
#include "ctr_logging.h"
#include "parent_pipe_fd.h"
#include "globals.h"
#include "cli.h"
#include "ctr_exit.h"

#include <glib.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/* Global hash table to store active healthcheck timers */
GHashTable *active_healthcheck_timers = NULL;

/* Check if systemd is available and running */
gboolean healthcheck_is_systemd_available(void)
{
    /* Check if we're running under systemd by looking for NOTIFY_SOCKET */
    const gchar *notify_socket = g_getenv("NOTIFY_SOCKET");
    if (notify_socket != NULL && g_str_has_prefix(notify_socket, "/")) {
        return TRUE;
    }
    
    /* Check if systemd is the init process (PID 1) */
    gchar *proc_1_cmdline = NULL;
    gsize length = 0;
    if (g_file_get_contents("/proc/1/cmdline", &proc_1_cmdline, &length, NULL)) {
        gboolean is_systemd = g_str_has_prefix(proc_1_cmdline, "systemd");
        g_free(proc_1_cmdline);
        return is_systemd;
    }
    
    return FALSE;
}

/* Initialize healthcheck subsystem */
gboolean healthcheck_init(void)
{
    if (active_healthcheck_timers != NULL) {
        nwarn("Healthcheck subsystem already initialized");
        return TRUE;
    }
    
    active_healthcheck_timers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)healthcheck_timer_free);
    if (active_healthcheck_timers == NULL) {
        nwarn("Failed to create healthcheck timers hash table");
        return FALSE;
    }
    
    return TRUE;
}

/* Cleanup healthcheck subsystem */
void healthcheck_cleanup(void)
{
    if (active_healthcheck_timers != NULL) {
        g_hash_table_destroy(active_healthcheck_timers);
        active_healthcheck_timers = NULL;
    }
}

/* Create a new healthcheck configuration */
healthcheck_config_t *healthcheck_config_new(void)
{
    healthcheck_config_t *config = g_malloc0(sizeof(healthcheck_config_t));
    if (config == NULL) {
        nwarn("Failed to allocate memory for healthcheck config");
        return NULL;
    }
    
    config->interval = 30;      /* Default 30 seconds */
    config->timeout = 30;       /* Default 30 seconds */
    config->start_period = 0;   /* Default no grace period */
    config->retries = 3;        /* Default 3 retries */
    config->enabled = FALSE;    /* Default disabled */
    
    return config;
}

/* Free healthcheck configuration */
void healthcheck_config_free(healthcheck_config_t *config)
{
    if (config == NULL) {
        return;
    }
    
    if (config->test != NULL) {
        g_strfreev(config->test);
    }
    g_free(config);
}

/* Create a new healthcheck timer */
healthcheck_timer_t *healthcheck_timer_new(const gchar *container_id, const healthcheck_config_t *config)
{
    if (container_id == NULL || config == NULL) {
        nwarn("Invalid parameters for healthcheck timer creation");
        return NULL;
    }
    
    healthcheck_timer_t *timer = g_malloc0(sizeof(healthcheck_timer_t));
    if (timer == NULL) {
        nwarn("Failed to allocate memory for healthcheck timer");
        return NULL;
    }
    
    timer->container_id = g_strdup(container_id);
    timer->config = *config;
    timer->status = HEALTHCHECK_NONE;
    timer->consecutive_failures = 0;
    timer->start_period_remaining = config->start_period;
    timer->timer_active = FALSE;
    timer->timer_source_id = 0;
    timer->last_check_time = 0;
    
    /* Copy the test command array */
    if (config->test != NULL) {
        timer->config.test = g_strdupv((gchar **)config->test);
    }
    
    return timer;
}

/* Free healthcheck timer */
void healthcheck_timer_free(healthcheck_timer_t *timer)
{
    if (timer == NULL) {
        return;
    }
    
    if (timer->timer_active) {
        healthcheck_timer_stop(timer);
    }
    
    g_free(timer->container_id);
    if (timer->config.test != NULL) {
        g_strfreev(timer->config.test);
    }
    g_free(timer);
}

/* Start healthcheck timer */
gboolean healthcheck_timer_start(healthcheck_timer_t *timer)
{
    if (timer == NULL || timer->timer_active) {
        nwarn("Cannot start healthcheck timer: invalid timer or already active");
        return FALSE;
    }
    
    if (!timer->config.enabled || timer->config.test == NULL) {
        nwarn("Cannot start healthcheck timer: disabled or no test command");
        return FALSE;
    }
    
    /* Start the timer with the configured interval */
    timer->timer_source_id = g_timeout_add_seconds(timer->config.interval, healthcheck_timer_callback, timer);
    if (timer->timer_source_id == 0) {
        nwarn("Failed to create healthcheck timer");
        return FALSE;
    }
    
    timer->timer_active = TRUE;
    timer->status = HEALTHCHECK_STARTING;
    timer->last_check_time = time(NULL);
    
    
    return TRUE;
}

/* Stop healthcheck timer */
void healthcheck_timer_stop(healthcheck_timer_t *timer)
{
    if (timer == NULL || !timer->timer_active) {
        return;
    }
    
    if (timer->timer_source_id > 0) {
        g_source_remove(timer->timer_source_id);
        timer->timer_source_id = 0;
    }
    
    timer->timer_active = FALSE;
    timer->status = HEALTHCHECK_NONE;
    
}

/* Check if timer is active */
gboolean healthcheck_timer_is_active(const healthcheck_timer_t *timer)
{
    return timer != NULL && timer->timer_active;
}

/* Get current healthcheck status */
healthcheck_status_t healthcheck_timer_get_status(const healthcheck_timer_t *timer)
{
    return timer != NULL ? timer->status : HEALTHCHECK_NONE;
}

/* Execute healthcheck command */
gboolean healthcheck_execute_command(const healthcheck_config_t *config, gint *exit_code)
{
    if (config == NULL || config->test == NULL || exit_code == NULL) {
        nwarn("Invalid parameters for healthcheck command execution");
        return FALSE;
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        nwarn("Failed to fork for healthcheck command");
        return FALSE;
    }
    
    if (pid == 0) {
        /* Child process - execute the healthcheck command */
        execvp(config->test[0], config->test);
        _exit(127); /* Command not found */
    }
    
    /* Parent process - wait for child to complete */
    int status;
    pid_t result = waitpid(pid, &status, 0);
    if (result < 0) {
        nwarn("Failed to wait for healthcheck command");
        return FALSE;
    }
    
    *exit_code = get_exit_status(status);
    return TRUE;
}


/* Convert healthcheck status to string */
gchar *healthcheck_status_to_string(healthcheck_status_t status)
{
    switch (status) {
        case HEALTHCHECK_NONE:
            return g_strdup("none");
        case HEALTHCHECK_STARTING:
            return g_strdup("starting");
        case HEALTHCHECK_HEALTHY:
            return g_strdup("healthy");
        case HEALTHCHECK_UNHEALTHY:
            return g_strdup("unhealthy");
        default:
            return g_strdup("unknown");
    }
}


/* Send healthcheck status update to Podman */
gboolean healthcheck_send_status_update(const gchar *container_id, healthcheck_status_t status, gint exit_code)
{
    if (container_id == NULL) {
        nwarn("Cannot send healthcheck status update: invalid container ID");
        return FALSE;
    }
    
    _cleanup_free_ gchar *status_str = healthcheck_status_to_string(status);
    if (status_str == NULL) {
        nwarn("Failed to convert healthcheck status to string");
        return FALSE;
    }
    
    /* Create JSON message for status update */
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "healthcheck_status");
    
    json_builder_set_member_name(builder, "container_id");
    json_builder_add_string_value(builder, container_id);
    
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, status_str);
    
    json_builder_set_member_name(builder, "exit_code");
    json_builder_add_int_value(builder, exit_code);
    
    json_builder_set_member_name(builder, "timestamp");
    json_builder_add_int_value(builder, time(NULL));
    
    json_builder_end_object(builder);
    
    JsonNode *root = json_builder_get_root(builder);
    _cleanup_free_ gchar *json_str = json_to_string(root, TRUE);
    
    g_object_unref(builder);
    
    if (json_str == NULL) {
        nwarn("Failed to create healthcheck status update JSON");
        return FALSE;
    }
    
    /* Send via sync pipe to Podman */
    write_or_close_sync_fd(&sync_pipe_fd, HEALTHCHECK_MSG_STATUS_UPDATE, json_str);
    
    
    return TRUE;
}


/* Discover healthcheck configuration from OCI config.json */
gboolean healthcheck_discover_from_oci_config(const gchar *bundle_path, healthcheck_config_t *config)
{
    if (bundle_path == NULL || config == NULL) {
        nwarn("Invalid parameters for healthcheck discovery");
        return FALSE;
    }
    
    _cleanup_free_ gchar *config_path = g_build_filename(bundle_path, "config.json", NULL);
    if (config_path == NULL) {
        nwarn("Failed to build config.json path");
        return FALSE;
    }
    
    if (!g_file_test(config_path, G_FILE_TEST_EXISTS)) {
        ndebugf("OCI config file not found: %s", config_path);
        return FALSE;
    }
    
    JsonParser *parser = json_parser_new();
    if (parser == NULL) {
        nwarn("Failed to create JSON parser for OCI config");
        return FALSE;
    }
    
    GError *error = NULL;
    if (!json_parser_load_from_file(parser, config_path, &error)) {
        nwarnf("Failed to load OCI config: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return FALSE;
    }
    
    JsonNode *root = json_parser_get_root(parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
        nwarn("Invalid OCI config structure");
        g_object_unref(parser);
        return FALSE;
    }
    
    JsonObject *obj = json_node_get_object(root);
    JsonObject *annotations = json_object_get_object_member(obj, "annotations");
    
    gboolean result = FALSE;
    if (annotations != NULL) {
        result = healthcheck_parse_oci_annotations(annotations, config);
        if (!result) {
            nwarn("Failed to parse healthcheck annotations from OCI config");
        }
    } else {
        ndebug("No annotations found in OCI config");
    }
    
    g_object_unref(parser);
    return result;
}

/* Parse healthcheck configuration from OCI annotations */
gboolean healthcheck_parse_oci_annotations(JsonObject *annotations, healthcheck_config_t *config)
{
    if (annotations == NULL || config == NULL) {
        nwarn("Invalid parameters for annotation parsing");
        return FALSE;
    }
    
    /* Look for healthcheck annotations */
    const gchar *healthcheck_cmd = json_object_get_string_member(annotations, "io.containers.healthcheck.cmd");
    const gchar *healthcheck_interval = json_object_get_string_member(annotations, "io.containers.healthcheck.interval");
    const gchar *healthcheck_timeout = json_object_get_string_member(annotations, "io.containers.healthcheck.timeout");
    const gchar *healthcheck_start_period = json_object_get_string_member(annotations, "io.containers.healthcheck.start-period");
    const gchar *healthcheck_retries = json_object_get_string_member(annotations, "io.containers.healthcheck.retries");
    const gchar *healthcheck_enabled = json_object_get_string_member(annotations, "io.containers.healthcheck.enabled");
    
    /* Check if healthcheck is enabled */
    if (healthcheck_enabled == NULL || g_strcmp0(healthcheck_enabled, "true") != 0) {
        ndebug("Healthcheck not enabled in annotations");
        return FALSE;
    }
    
    /* Parse healthcheck command */
    if (healthcheck_cmd == NULL) {
        nwarn("Healthcheck command not specified in annotations");
        return FALSE;
    }
    
    /* Parse command string into array */
    gchar **cmd_parts = g_strsplit(healthcheck_cmd, " ", -1);
    if (cmd_parts == NULL || cmd_parts[0] == NULL) {
        nwarn("Failed to parse healthcheck command");
        g_strfreev(cmd_parts);
        return FALSE;
    }
    
    config->test = cmd_parts; /* Transfer ownership */
    config->enabled = TRUE;
    
    /* Parse interval */
    if (healthcheck_interval != NULL) {
        config->interval = atoi(healthcheck_interval);
        if (config->interval <= 0) {
            nwarnf("Invalid healthcheck interval: %s", healthcheck_interval);
            config->interval = 30; /* Default */
        }
    } else {
        config->interval = 30; /* Default */
    }
    
    /* Parse timeout */
    if (healthcheck_timeout != NULL) {
        config->timeout = atoi(healthcheck_timeout);
        if (config->timeout <= 0) {
            nwarnf("Invalid healthcheck timeout: %s", healthcheck_timeout);
            config->timeout = 30; /* Default */
        }
    } else {
        config->timeout = 30; /* Default */
    }
    
    /* Parse start period */
    if (healthcheck_start_period != NULL) {
        config->start_period = atoi(healthcheck_start_period);
        if (config->start_period < 0) {
            nwarnf("Invalid healthcheck start period: %s", healthcheck_start_period);
            config->start_period = 0; /* Default */
        }
    } else {
        config->start_period = 0; /* Default */
    }
    
    /* Parse retries */
    if (healthcheck_retries != NULL) {
        config->retries = atoi(healthcheck_retries);
        if (config->retries < 0) {
            nwarnf("Invalid healthcheck retries: %s", healthcheck_retries);
            config->retries = 3; /* Default */
        }
    } else {
        config->retries = 3; /* Default */
    }
    
    
    return TRUE;
}

/* Timer callback function */
gboolean healthcheck_timer_callback(gpointer user_data)
{
    healthcheck_timer_t *timer = (healthcheck_timer_t *)user_data;
    if (timer == NULL || !timer->timer_active) {
        return G_SOURCE_REMOVE;
    }
    
    /* Check if we're still in start period */
    if (timer->start_period_remaining > 0) {
        timer->start_period_remaining -= timer->config.interval;
        if (timer->start_period_remaining > 0) {
            return G_SOURCE_CONTINUE;
        }
    }
    
    /* Execute healthcheck command */
    gint exit_code;
    gboolean success = healthcheck_execute_command(&timer->config, &exit_code);
    
    if (!success) {
        nwarnf("Failed to execute healthcheck command for container %s", timer->container_id);
        timer->consecutive_failures++;
        timer->status = HEALTHCHECK_UNHEALTHY;
        healthcheck_send_status_update(timer->container_id, timer->status, exit_code);
        return G_SOURCE_CONTINUE;
    }
    
    /* Check if healthcheck passed */
    if (exit_code == 0) {
        /* Healthcheck passed */
        timer->consecutive_failures = 0;
        if (timer->status != HEALTHCHECK_HEALTHY) {
            timer->status = HEALTHCHECK_HEALTHY;
            healthcheck_send_status_update(timer->container_id, timer->status, exit_code);
        }
    } else {
        /* Healthcheck failed */
        timer->consecutive_failures++;
        if (timer->consecutive_failures >= timer->config.retries) {
            timer->status = HEALTHCHECK_UNHEALTHY;
            healthcheck_send_status_update(timer->container_id, timer->status, exit_code);
        }
    }
    
    timer->last_check_time = time(NULL);
    return G_SOURCE_CONTINUE;
}

#ifndef HEALTHCHECK_H
#define HEALTHCHECK_H

#include <glib.h>
#include <time.h>

/* Forward declarations */
typedef struct _JsonObject JsonObject;

/* Healthcheck status values */
typedef enum {
    HEALTHCHECK_NONE = 0,
    HEALTHCHECK_STARTING = 1,
    HEALTHCHECK_HEALTHY = 2,
    HEALTHCHECK_UNHEALTHY = 3
} healthcheck_status_t;

/* Healthcheck configuration structure */
typedef struct {
    gchar **test;           /* Healthcheck command and arguments */
    gint interval;          /* Interval between healthchecks in seconds */
    gint timeout;           /* Timeout for each healthcheck in seconds */
    gint start_period;      /* Grace period before healthchecks start in seconds */
    gint retries;           /* Number of consecutive failures before marking unhealthy */
    gboolean enabled;       /* Whether healthcheck is enabled */
} healthcheck_config_t;

/* Healthcheck timer structure */
typedef struct {
    gchar *container_id;    /* Container ID this timer belongs to */
    healthcheck_config_t config;
    healthcheck_status_t status;
    gint consecutive_failures;
    gint start_period_remaining;
    gboolean timer_active;
    guint timer_source_id;  /* GLib timer source ID */
    time_t last_check_time;
} healthcheck_timer_t;

/* Healthcheck message types for communication with Podman */
#define HEALTHCHECK_MSG_STATUS_UPDATE 2

/* Function declarations */
healthcheck_config_t *healthcheck_config_new(void);
void healthcheck_config_free(healthcheck_config_t *config);
healthcheck_timer_t *healthcheck_timer_new(const gchar *container_id, const healthcheck_config_t *config);
void healthcheck_timer_free(healthcheck_timer_t *timer);
gboolean healthcheck_timer_start(healthcheck_timer_t *timer);
void healthcheck_timer_stop(healthcheck_timer_t *timer);
gboolean healthcheck_timer_is_active(const healthcheck_timer_t *timer);
healthcheck_status_t healthcheck_timer_get_status(const healthcheck_timer_t *timer);
gboolean healthcheck_execute_command(const healthcheck_config_t *config, gint *exit_code);
gchar *healthcheck_status_to_string(healthcheck_status_t status);
gboolean healthcheck_send_status_update(const gchar *container_id, healthcheck_status_t status, gint exit_code);

/* Global healthcheck timer management */
extern GHashTable *active_healthcheck_timers;

/* Initialize healthcheck subsystem */
gboolean healthcheck_init(void);
void healthcheck_cleanup(void);

/* Systemd detection */
gboolean healthcheck_is_systemd_available(void);

/* Automatic healthcheck discovery from OCI config */
gboolean healthcheck_discover_from_oci_config(const gchar *bundle_path, healthcheck_config_t *config);
gboolean healthcheck_parse_oci_annotations(JsonObject *annotations, healthcheck_config_t *config);

/* Timer callback function */
gboolean healthcheck_timer_callback(gpointer user_data);

#endif /* HEALTHCHECK_H */

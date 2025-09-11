#define _GNU_SOURCE

#include "healthcheck.h"
#include "utils.h"
#include "ctr_logging.h"
#include "parent_pipe_fd.h"
#include "globals.h"
#include "cli.h"
#include "ctr_exit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <cJSON.h>

/* Simple hash table implementation */
struct hash_entry {
    char *key;
    void *value;
    struct hash_entry *next;
};

struct hash_table {
    struct hash_entry **buckets;
    size_t size;
    size_t count;
};

static unsigned int hash_string(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

struct hash_table *hash_table_new(size_t size) {
    struct hash_table *ht = calloc(1, sizeof(struct hash_table));
    if (!ht) {
        return NULL;
    }
    
    ht->buckets = calloc(size, sizeof(struct hash_entry*));
    if (!ht->buckets) {
        free(ht);
        return NULL;
    }
    
    ht->size = size;
    return ht;
}

void hash_table_free(struct hash_table *ht) {
    if (!ht) return;
    
    for (size_t i = 0; i < ht->size; i++) {
        struct hash_entry *entry = ht->buckets[i];
        while (entry) {
            struct hash_entry *next = entry->next;
            free(entry->key);
            free(entry);
            entry = next;
        }
    }
    
    free(ht->buckets);
    free(ht);
}

void *hash_table_get(struct hash_table *ht, const char *key) {
    if (!ht || !key) return NULL;
    
    unsigned int hash = hash_string(key) % ht->size;
    struct hash_entry *entry = ht->buckets[hash];
    
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            return entry->value;
        }
        entry = entry->next;
    }
    
    return NULL;
}

bool hash_table_put(struct hash_table *ht, const char *key, void *value) {
    if (!ht || !key) return false;
    
    unsigned int hash = hash_string(key) % ht->size;
    struct hash_entry *entry = ht->buckets[hash];
    
    /* Check if key already exists */
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            entry->value = value;
            return true;
        }
        entry = entry->next;
    }
    
    /* Create new entry */
    entry = malloc(sizeof(struct hash_entry));
    if (!entry) return false;
    
    entry->key = strdup(key);
    if (!entry->key) {
        free(entry);
        return false;
    }
    
    entry->value = value;
    entry->next = ht->buckets[hash];
    ht->buckets[hash] = entry;
    ht->count++;
    
    return true;
}

/* Global healthcheck timers hash table */
struct hash_table *active_healthcheck_timers = NULL;

/* Check if systemd is available and running */
bool healthcheck_is_systemd_available(void) {
    /* Check if we're running under systemd by looking for NOTIFY_SOCKET */
    const char *notify_socket = getenv("NOTIFY_SOCKET");
    if (notify_socket != NULL && notify_socket[0] == '/') {
        return true;
    }
    
    /* Check if systemd is the init process (PID 1) */
    FILE *fp = fopen("/proc/1/cmdline", "r");
    if (fp) {
        char cmdline[256];
        if (fgets(cmdline, sizeof(cmdline), fp)) {
            fclose(fp);
            return strncmp(cmdline, "systemd", 7) == 0;
        }
        fclose(fp);
    }
    
    return false;
}

/* Initialize healthcheck subsystem */
bool healthcheck_init(void) {
    if (active_healthcheck_timers != NULL) {
        return true;
    }
    
    active_healthcheck_timers = hash_table_new(16);
    if (active_healthcheck_timers == NULL) {
        return false;
    }
    
    return true;
}

/* Cleanup healthcheck subsystem */
void healthcheck_cleanup(void) {
    if (active_healthcheck_timers != NULL) {
        /* Free all timers */
        for (size_t i = 0; i < active_healthcheck_timers->size; i++) {
            struct hash_entry *entry = active_healthcheck_timers->buckets[i];
            while (entry) {
                struct hash_entry *next = entry->next;
                healthcheck_timer_free((healthcheck_timer_t*)entry->value);
                free(entry->key);
                free(entry);
                entry = next;
            }
        }
        
        hash_table_free(active_healthcheck_timers);
        active_healthcheck_timers = NULL;
    }
}

/* Create a new healthcheck configuration */
healthcheck_config_t *healthcheck_config_new(void) {
    healthcheck_config_t *config = calloc(1, sizeof(healthcheck_config_t));
    if (config == NULL) {
        nwarn("Failed to allocate memory for healthcheck config");
        return NULL;
    }
    
    config->interval = 30;      /* Default 30 seconds */
    config->timeout = 30;       /* Default 30 seconds */
    config->start_period = 0;   /* Default no grace period */
    config->retries = 3;        /* Default 3 retries */
    config->enabled = false;    /* Default disabled */
    
    return config;
}

/* Free healthcheck configuration */
void healthcheck_config_free(healthcheck_config_t *config) {
    if (config == NULL) {
        return;
    }

    if (config->test != NULL) {
        for (int i = 0; config->test[i] != NULL; i++) {
            free(config->test[i]);
        }
        free(config->test);
    }
    // Don't free config itself - it's a local variable on the stack
}

/* Create a new healthcheck timer */
healthcheck_timer_t *healthcheck_timer_new(const char *container_id, const healthcheck_config_t *config) {
    if (container_id == NULL || config == NULL) {
        nwarn("Invalid parameters for healthcheck timer creation");
        return NULL;
    }
    
    healthcheck_timer_t *timer = calloc(1, sizeof(healthcheck_timer_t));
    if (timer == NULL) {
        nwarn("Failed to allocate memory for healthcheck timer");
        return NULL;
    }
    
    timer->container_id = strdup(container_id);
    if (timer->container_id == NULL) {
        free(timer);
        return NULL;
    }
    
    timer->config = *config;
    timer->status = HEALTHCHECK_NONE;
    timer->consecutive_failures = 0;
    timer->start_period_remaining = config->start_period;
    timer->timer_active = false;
    timer->last_check_time = 0;
    
    /* Copy the test command array */
    if (config->test != NULL) {
        int argc = 0;
        while (config->test[argc] != NULL) argc++;
        
        timer->config.test = calloc(argc + 1, sizeof(char*));
        if (timer->config.test == NULL) {
            free(timer->container_id);
            free(timer);
            return NULL;
        }
        
        for (int i = 0; i < argc; i++) {
            timer->config.test[i] = strdup(config->test[i]);
            if (timer->config.test[i] == NULL) {
                for (int j = 0; j < i; j++) {
                    free(timer->config.test[j]);
                }
                free(timer->config.test);
                free(timer->container_id);
                free(timer);
                return NULL;
            }
        }
    }
    
    return timer;
}

/* Free healthcheck timer */
void healthcheck_timer_free(healthcheck_timer_t *timer) {
    if (timer == NULL) {
        return;
    }
    
    if (timer->timer_active) {
        healthcheck_timer_stop(timer);
    }
    
    free(timer->container_id);
    if (timer->config.test != NULL) {
        for (int i = 0; timer->config.test[i] != NULL; i++) {
            free(timer->config.test[i]);
        }
        free(timer->config.test);
    }
    free(timer);
}

/* Start healthcheck timer */
bool healthcheck_timer_start(healthcheck_timer_t *timer) {
    if (timer == NULL || timer->timer_active) {
        return false;
    }
    
    if (!timer->config.enabled || timer->config.test == NULL) {
        return false;
    }
    /* Create a timer thread */
    int result = pthread_create(&timer->timer_thread, NULL, healthcheck_timer_thread, timer);
    if (result != 0) {
        nwarnf("Failed to create healthcheck timer thread: %s", strerror(result));
        return false;
    }
    
    timer->timer_active = true;
    timer->status = HEALTHCHECK_STARTING;
    timer->last_check_time = time(NULL);
    return true;
}

/* Stop healthcheck timer */
void healthcheck_timer_stop(healthcheck_timer_t *timer) {
    if (timer == NULL || !timer->timer_active) {
        return;
    }
    
    timer->timer_active = false;
    timer->status = HEALTHCHECK_NONE;
    
    /* Wait for the timer thread to finish */
    pthread_join(timer->timer_thread, NULL);
}

/* Check if timer is active */
bool healthcheck_timer_is_active(const healthcheck_timer_t *timer) {
    return timer != NULL && timer->timer_active;
}

/* Get current healthcheck status */
healthcheck_status_t healthcheck_timer_get_status(const healthcheck_timer_t *timer) {
    return timer != NULL ? timer->status : HEALTHCHECK_NONE;
}

/* Execute healthcheck command */
bool healthcheck_execute_command(const healthcheck_config_t *config, int *exit_code) {
    if (config == NULL || config->test == NULL || exit_code == NULL) {
        nwarn("Invalid parameters for healthcheck command execution");
        return false;
    }
    
    /* For now, we'll use a simple approach: check if the container is running */
    /* This is a placeholder implementation - in a real implementation, we would */
    /* need to execute the command inside the container using the container runtime API */
    
    /* Simple healthcheck: just return success for now */
    /* TODO: Implement proper container command execution */
    *exit_code = 0;
    return true;
}

/* Convert healthcheck status to string */
char *healthcheck_status_to_string(healthcheck_status_t status) {
    switch (status) {
        case HEALTHCHECK_NONE:
            return strdup("none");
        case HEALTHCHECK_STARTING:
            return strdup("starting");
        case HEALTHCHECK_HEALTHY:
            return strdup("healthy");
        case HEALTHCHECK_UNHEALTHY:
            return strdup("unhealthy");
        default:
            return strdup("unknown");
    }
}

/* Send healthcheck status update to Podman */
bool healthcheck_send_status_update(const char *container_id, healthcheck_status_t status, int exit_code) {
    if (container_id == NULL) {
        nwarn("Cannot send healthcheck status update: invalid container ID");
        return false;
    }
    
    char *status_str = healthcheck_status_to_string(status);
    if (status_str == NULL) {
        nwarn("Failed to convert healthcheck status to string");
        return false;
    }
    
    /* Create simple JSON message for status update */
    char json_msg[1024];
    snprintf(json_msg, sizeof(json_msg),
        "{\"type\":\"healthcheck_status\",\"container_id\":\"%s\",\"status\":\"%s\",\"exit_code\":%d,\"timestamp\":%ld}",
        container_id, status_str, exit_code, time(NULL));
    
    free(status_str);
    
    /* Send via sync pipe to Podman */
    /* Temporarily disabled for debugging */
    /* write_or_close_sync_fd(&sync_pipe_fd, HEALTHCHECK_MSG_STATUS_UPDATE, json_msg); */
    ninfof("Healthcheck status update: %s", json_msg);
    
    return true;
}

/* Discover healthcheck configuration from OCI config.json */
bool healthcheck_discover_from_oci_config(const char *bundle_path, healthcheck_config_t *config) {
    if (bundle_path == NULL || config == NULL) {
        nwarn("Invalid parameters for healthcheck discovery");
        return false;
    }
    
    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/config.json", bundle_path);
    
    struct stat st;
    if (stat(config_path, &st) != 0) {
        ndebugf("OCI config file not found: %s", config_path);
        return false;
    }
    
    /* Read the config file */
    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        nwarnf("Failed to open OCI config: %s", strerror(errno));
        return false;
    }
    
    /* Read entire file */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char *file_content = malloc(file_size + 1);
    if (!file_content) {
        fclose(fp);
        return false;
    }
    
    size_t bytes_read = fread(file_content, 1, file_size, fp);
    if (bytes_read != (size_t)file_size) {
        nwarn("Failed to read entire config file");
        free(file_content);
        fclose(fp);
        return false;
    }
    file_content[file_size] = '\0';
    fclose(fp);
    
    /* Parse JSON using cJSON */
    cJSON *json = cJSON_Parse(file_content);
    if (json == NULL) {
        nwarn("Failed to parse OCI config JSON");
        free(file_content);
        return false;
    }
    
    /* Look for annotations */
    cJSON *annotations = cJSON_GetObjectItem(json, "annotations");
    if (cJSON_IsObject(annotations)) {
        cJSON *healthcheck = cJSON_GetObjectItem(annotations, "io.podman.healthcheck");
        if (cJSON_IsString(healthcheck)) {
            /* Parse the healthcheck JSON */
            bool result = healthcheck_parse_oci_annotations(healthcheck->valuestring, config);
            cJSON_Delete(json);
            free(file_content);
            return result;
        }
    }
    
    cJSON_Delete(json);
    free(file_content);
    return false;
}

/* Parse healthcheck configuration from OCI annotations */
bool healthcheck_parse_oci_annotations(const char *annotations_json, healthcheck_config_t *config) {
    if (annotations_json == NULL || config == NULL) {
        nwarn("Invalid parameters for annotation parsing");
        return false;
    }
    
    /* Parse the JSON using cJSON */
    cJSON *json = cJSON_Parse(annotations_json);
    if (json == NULL) {
        nwarn("Failed to parse healthcheck JSON");
        return false;
    }
    
    /* Set defaults */
    config->enabled = true;
    config->interval = 30;      // Default 30 seconds
    config->timeout = 10;       // Default 10 seconds
    config->start_period = 0;   // Default 0 seconds
    config->retries = 3;        // Default 3 retries
    
    /* Parse Test command */
    cJSON *test_array = cJSON_GetObjectItem(json, "test");
    if (cJSON_IsArray(test_array) && cJSON_GetArraySize(test_array) >= 2) {
        cJSON *cmd_type = cJSON_GetArrayItem(test_array, 0);
        cJSON *cmd_value = cJSON_GetArrayItem(test_array, 1);
        
        if (cJSON_IsString(cmd_type) && cJSON_IsString(cmd_value)) {
            if (strcmp(cmd_type->valuestring, "CMD") == 0 || strcmp(cmd_type->valuestring, "CMD-SHELL") == 0) {
                /* Create test command array */
                config->test = calloc(2, sizeof(char*));
                config->test[0] = strdup(cmd_value->valuestring);
                config->test[1] = NULL;
            }
        }
    }
    
    /* Parse Interval (now in seconds) */
    cJSON *interval = cJSON_GetObjectItem(json, "interval");
    if (cJSON_IsNumber(interval)) {
        config->interval = (int)interval->valuedouble;
    }
    
    /* Parse Timeout (now in seconds) */
    cJSON *timeout = cJSON_GetObjectItem(json, "timeout");
    if (cJSON_IsNumber(timeout)) {
        config->timeout = (int)timeout->valuedouble;
    }
    
    /* Parse StartPeriod (now in seconds) */
    cJSON *start_period = cJSON_GetObjectItem(json, "start_period");
    if (cJSON_IsNumber(start_period)) {
        config->start_period = (int)start_period->valuedouble;
    }
    
    /* Parse Retries */
    cJSON *retries = cJSON_GetObjectItem(json, "retries");
    if (cJSON_IsNumber(retries)) {
        config->retries = (int)retries->valuedouble;
    }
    
    /* Clean up */
    cJSON_Delete(json);
    
    return true;
}

/* Timer thread function */
void *healthcheck_timer_thread(void *user_data) {
    healthcheck_timer_t *timer = (healthcheck_timer_t *)user_data;
    if (timer == NULL) {
        return NULL;
    }
    
    while (timer->timer_active) {
        /* Sleep for the interval */
        sleep(timer->config.interval);
        
        if (!timer->timer_active) {
            break;
        }
        
        /* Check if we're still in start period */
        if (timer->start_period_remaining > 0) {
            timer->start_period_remaining -= timer->config.interval;
            if (timer->start_period_remaining > 0) {
                continue;
            }
        }
        
        /* Execute healthcheck command */
        int exit_code;
        bool success = healthcheck_execute_command(&timer->config, &exit_code);
        
        if (!success) {
            nwarnf("Failed to execute healthcheck command for container %s", timer->container_id);
            timer->consecutive_failures++;
            timer->status = HEALTHCHECK_UNHEALTHY;
            healthcheck_send_status_update(timer->container_id, timer->status, exit_code);
            continue;
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
    }
    
    return NULL;
}
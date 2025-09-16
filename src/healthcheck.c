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
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <cJSON.h>

/* Healthcheck validation constants */
#define HEALTHCHECK_INTERVAL_MIN 1
#define HEALTHCHECK_INTERVAL_MAX 3600
#define HEALTHCHECK_TIMEOUT_MIN 1
#define HEALTHCHECK_TIMEOUT_MAX 300
#define HEALTHCHECK_START_PERIOD_MIN 0
#define HEALTHCHECK_START_PERIOD_MAX 3600
#define HEALTHCHECK_RETRIES_MIN 0
#define HEALTHCHECK_RETRIES_MAX 100

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
        /* Free all timers first */
        for (size_t i = 0; i < active_healthcheck_timers->size; i++) {
            struct hash_entry *entry = active_healthcheck_timers->buckets[i];
            while (entry) {
                if (entry->value != NULL) {
                    healthcheck_timer_free((healthcheck_timer_t*)entry->value);
                    entry->value = NULL;  /* Prevent double-free */
                }
                entry = entry->next;
            }
        }
        
        /* Now free the hash table structure */
        hash_table_free(active_healthcheck_timers);
        active_healthcheck_timers = NULL;
    }
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
    
    /* Stop the timer if it's still active */
    if (timer->timer_active) {
        healthcheck_timer_stop(timer);
    }
    
    /* Free container ID */
    if (timer->container_id != NULL) {
        free(timer->container_id);
        timer->container_id = NULL;
    }
    
    /* Free test command array */
    if (timer->config.test != NULL) {
        for (int i = 0; timer->config.test[i] != NULL; i++) {
            free(timer->config.test[i]);
        }
        free(timer->config.test);
        timer->config.test = NULL;
    }
    
    /* Clear the timer structure to prevent double-free */
    memset(timer, 0, sizeof(healthcheck_timer_t));
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
    
    /* Wait for the timer thread to finish with a timeout */
    void *thread_result;
    int join_result = pthread_join(timer->timer_thread, &thread_result);
    if (join_result != 0) {
        nwarnf("Failed to join healthcheck timer thread: %s", strerror(join_result));
    }
}

/* Execute healthcheck command */
bool healthcheck_execute_command(const healthcheck_config_t *config, int *exit_code) {
    if (config == NULL || config->test == NULL || exit_code == NULL) {
        nwarn("Invalid parameters for healthcheck command execution");
        return false;
    }
    
    /* Initialize exit code to failure */
    *exit_code = -1;
    
    /* Fork a child process to execute the healthcheck command */
    pid_t pid = fork();
    if (pid == -1) {
        nwarnf("Failed to fork process for healthcheck command: %s", strerror(errno));
        return false;
    }
    
    if (pid == 0) {
        /* Child process - execute the healthcheck command */
        /* Redirect stdout and stderr to /dev/null to avoid cluttering logs */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        
        /* Execute the command */
        if (execvp(config->test[0], config->test) == -1) {
            /* If execvp fails, exit with error code */
            _exit(127); /* Command not found */
        }
    } else {
        /* Parent process - wait for child to complete */
        int status;
        pid_t wait_result = waitpid(pid, &status, 0);
        
        if (wait_result == -1) {
            nwarnf("Failed to wait for healthcheck command: %s", strerror(errno));
            return false;
        }
        
        if (WIFEXITED(status)) {
            *exit_code = WEXITSTATUS(status);
            return true;
        } else if (WIFSIGNALED(status)) {
            nwarnf("Healthcheck command terminated by signal %d", WTERMSIG(status));
            *exit_code = 128 + WTERMSIG(status); /* Standard convention for signal termination */
            return true;
        } else {
            nwarn("Healthcheck command did not terminate normally");
            *exit_code = -1;
            return false;
        }
    }
    
    /* This should never be reached */
    return false;
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
    write_or_close_sync_fd(&sync_pipe_fd, HEALTHCHECK_MSG_STATUS_UPDATE, json_msg);
    
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
    
    /* Initialize config - no defaults, all values must be provided by Podman */
    config->enabled = true;
    config->interval = 0;
    config->timeout = 0;
    config->start_period = 0;
    config->retries = 0;
    config->test = NULL;
    
    /* Parse Test command - REQUIRED */
    cJSON *test_array = cJSON_GetObjectItem(json, "test");
    if (!cJSON_IsArray(test_array) || cJSON_GetArraySize(test_array) < 2) {
        nwarn("Healthcheck configuration missing required 'test' command");
        cJSON_Delete(json);
        return false;
    }
    
    cJSON *cmd_type = cJSON_GetArrayItem(test_array, 0);
    
    if (!cJSON_IsString(cmd_type)) {
        nwarn("Healthcheck command type must be a string");
        cJSON_Delete(json);
        return false;
    }
    
    if (strcmp(cmd_type->valuestring, "CMD") == 0) {
        /* CMD (Exec Form) - Array of command and arguments */
        int array_size = cJSON_GetArraySize(test_array);
        if (array_size < 2) {
            nwarn("CMD healthcheck requires at least one command argument");
            cJSON_Delete(json);
            return false;
        }
        
        /* Allocate memory for command array (excluding the "CMD" type) */
        config->test = calloc(array_size, sizeof(char*));
        if (config->test == NULL) {
            nwarn("Failed to allocate memory for healthcheck test command");
            cJSON_Delete(json);
            return false;
        }
        
        /* Copy all arguments except the first one (which is "CMD") */
        for (int i = 1; i < array_size; i++) {
            cJSON *arg = cJSON_GetArrayItem(test_array, i);
            if (!cJSON_IsString(arg)) {
                nwarnf("CMD healthcheck argument %d must be a string", i);
                for (int j = 0; j < i - 1; j++) {
                    free(config->test[j]);
                }
                free(config->test);
                cJSON_Delete(json);
                return false;
            }
            
            config->test[i-1] = strdup(arg->valuestring);
            if (config->test[i-1] == NULL) {
                nwarn("Failed to duplicate healthcheck command argument");
                for (int j = 0; j < i - 1; j++) {
                    free(config->test[j]);
                }
                free(config->test);
                cJSON_Delete(json);
                return false;
            }
        }
        config->test[array_size-1] = NULL;
        
    } else if (strcmp(cmd_type->valuestring, "CMD-SHELL") == 0) {
        /* CMD-SHELL (Shell Form) - Single string executed via /bin/sh -c */
        if (cJSON_GetArraySize(test_array) != 2) {
            nwarn("CMD-SHELL healthcheck requires exactly one command string");
            cJSON_Delete(json);
            return false;
        }
        
        cJSON *cmd_value = cJSON_GetArrayItem(test_array, 1);
        if (!cJSON_IsString(cmd_value)) {
            nwarn("CMD-SHELL healthcheck command must be a string");
            cJSON_Delete(json);
            return false;
        }
        
        size_t cmd_len = strlen(cmd_value->valuestring);
        const size_t MAX_HEALTHCHECK_CMD_LEN = 4096;
        
        if (cmd_len == 0) {
            nwarn("Healthcheck command cannot be empty");
            cJSON_Delete(json);
            return false;
        }
        
        if (cmd_len > MAX_HEALTHCHECK_CMD_LEN) {
            nwarnf("Healthcheck command too long (%zu chars, max %zu)", cmd_len, MAX_HEALTHCHECK_CMD_LEN);
            cJSON_Delete(json);
            return false;
        }
        
        /* Create test command array for shell execution */
        config->test = calloc(3, sizeof(char*));
        if (config->test == NULL) {
            nwarn("Failed to allocate memory for healthcheck test command");
            cJSON_Delete(json);
            return false;
        }
        
        config->test[0] = strdup("/bin/sh");
        config->test[1] = strdup("-c");
        config->test[2] = strdup(cmd_value->valuestring);
        
        if (config->test[0] == NULL || config->test[1] == NULL || config->test[2] == NULL) {
            nwarn("Failed to duplicate healthcheck test command strings");
            for (int i = 0; i < 3; i++) {
                if (config->test[i] != NULL) {
                    free(config->test[i]);
                }
            }
            free(config->test);
            cJSON_Delete(json);
            return false;
        }
        config->test[3] = NULL;
        
    } else {
        nwarnf("Unsupported healthcheck command type: %s (only CMD and CMD-SHELL supported)", cmd_type->valuestring);
        cJSON_Delete(json);
        return false;
    }
    
    /* Parse Interval (now in seconds) - REQUIRED */
    cJSON *interval = cJSON_GetObjectItem(json, "interval");
    if (!cJSON_IsNumber(interval) || interval->valuedouble < HEALTHCHECK_INTERVAL_MIN || interval->valuedouble > HEALTHCHECK_INTERVAL_MAX) {
        nwarnf("Healthcheck interval must be between %d and %d seconds, got: %.0f", HEALTHCHECK_INTERVAL_MIN, HEALTHCHECK_INTERVAL_MAX, interval->valuedouble);
        cJSON_Delete(json);
        return false;
    }
    config->interval = (int)interval->valuedouble;
    
    /* Parse Timeout (now in seconds) - REQUIRED */
    cJSON *timeout = cJSON_GetObjectItem(json, "timeout");
    if (!cJSON_IsNumber(timeout) || timeout->valuedouble < HEALTHCHECK_TIMEOUT_MIN || timeout->valuedouble > HEALTHCHECK_TIMEOUT_MAX) {
        nwarnf("Healthcheck timeout must be between %d and %d seconds, got: %.0f", HEALTHCHECK_TIMEOUT_MIN, HEALTHCHECK_TIMEOUT_MAX, timeout->valuedouble);
        cJSON_Delete(json);
        return false;
    }
    config->timeout = (int)timeout->valuedouble;
    
    /* Parse StartPeriod (now in seconds) - REQUIRED (can be 0) */
    cJSON *start_period = cJSON_GetObjectItem(json, "start_period");
    if (!cJSON_IsNumber(start_period) || start_period->valuedouble < HEALTHCHECK_START_PERIOD_MIN || start_period->valuedouble > HEALTHCHECK_START_PERIOD_MAX) {
        nwarnf("Healthcheck start_period must be between %d and %d seconds, got: %.0f", HEALTHCHECK_START_PERIOD_MIN, HEALTHCHECK_START_PERIOD_MAX, start_period->valuedouble);
        cJSON_Delete(json);
        return false;
    }
    config->start_period = (int)start_period->valuedouble;
    
    /* Parse Retries - REQUIRED */
    cJSON *retries = cJSON_GetObjectItem(json, "retries");
    if (!cJSON_IsNumber(retries) || retries->valuedouble < HEALTHCHECK_RETRIES_MIN || retries->valuedouble > HEALTHCHECK_RETRIES_MAX) {
        nwarnf("Healthcheck retries must be between %d and %d, got: %.0f", HEALTHCHECK_RETRIES_MIN, HEALTHCHECK_RETRIES_MAX, retries->valuedouble);
        cJSON_Delete(json);
        return false;
    }
    config->retries = (int)retries->valuedouble;
    
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
    
    /* Make a local copy of the timer pointer to avoid race conditions */
    healthcheck_timer_t *local_timer = timer;
    
    ninfof("Healthcheck timer started for container %s", local_timer->container_id);
    
    while (local_timer->timer_active) {
        /* Sleep for the interval, but check timer_active periodically */
        int sleep_time = local_timer->config.interval;
        while (sleep_time > 0 && local_timer->timer_active) {
            if (sleep_time > 1) {
                sleep(1);  /* Sleep 1 second at a time */
                sleep_time -= 1;
            } else {
                sleep(sleep_time);
                sleep_time = 0;
            }
        }
        
        if (!local_timer->timer_active) {
            break;
        }
        
        /* Check if we're still in start period */
        if (local_timer->start_period_remaining > 0) {
            local_timer->start_period_remaining -= local_timer->config.interval;
            if (local_timer->start_period_remaining > 0) {
                /* Still in startup period - send "starting" status */
                if (local_timer->status != HEALTHCHECK_STARTING) {
                    local_timer->status = HEALTHCHECK_STARTING;
                    ninfof("Healthcheck status changed to: starting (startup period: %d seconds remaining)", local_timer->start_period_remaining);
                    healthcheck_send_status_update(local_timer->container_id, local_timer->status, 0);
                }
                continue;
            } else {
                /* Startup period just ended - transition to regular healthchecks */
                ninfof("Startup period ended, transitioning to regular healthchecks");
            }
        }
        
        /* Execute healthcheck command */
        int exit_code;
        bool success = healthcheck_execute_command(&local_timer->config, &exit_code);
        
        if (!success) {
            nwarnf("Failed to execute healthcheck command for container %s", local_timer->container_id);
            local_timer->consecutive_failures++;
            local_timer->status = HEALTHCHECK_UNHEALTHY;
            healthcheck_send_status_update(local_timer->container_id, local_timer->status, exit_code);
            continue;
        }
        
        /* Check if healthcheck passed */
        if (exit_code == 0) {
            /* Healthcheck passed */
            local_timer->consecutive_failures = 0;
            if (local_timer->status != HEALTHCHECK_HEALTHY) {
                local_timer->status = HEALTHCHECK_HEALTHY;
                ninfof("Healthcheck status changed to: healthy");
            }
            /* Always send status update to keep Podman informed */
            healthcheck_send_status_update(local_timer->container_id, local_timer->status, exit_code);
        } else {
            /* Healthcheck failed */
            local_timer->consecutive_failures++;
            
            /* During startup period, failures don't count against retry limit */
            if (local_timer->start_period_remaining > 0) {
                ninfof("Healthcheck failed during startup period (exit code: %d) - not counting against retry limit", exit_code);
                /* Still send status update to show we're trying */
                healthcheck_send_status_update(local_timer->container_id, local_timer->status, exit_code);
            } else {
                /* Regular healthcheck failure - count against retry limit */
                if (local_timer->consecutive_failures >= local_timer->config.retries) {
                    local_timer->status = HEALTHCHECK_UNHEALTHY;
                    ninfof("Healthcheck status changed to: unhealthy (exit code: %d, retries: %d)", exit_code, local_timer->consecutive_failures);
                    healthcheck_send_status_update(local_timer->container_id, local_timer->status, exit_code);
                } else {
                    ninfof("Healthcheck failed (exit code: %d), consecutive failures: %d/%d", exit_code, local_timer->consecutive_failures, local_timer->config.retries);
                }
            }
        }
        
        local_timer->last_check_time = time(NULL);
    }
    
    ninfof("Healthcheck timer stopped for container %s", local_timer->container_id);
    return NULL;
}
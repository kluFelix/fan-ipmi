#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <nvml.h>

#include <stdarg.h>
#include <stdbool.h>
#include "tomlc17.h"

// Configuration defaults
#define CONFIG_FILE "/etc/fan-ipmi/fan.toml"
#define SEND_DELAY_MS 500
#define RETRY_DELAY_MS 7500
#define MAX_CURVE_POINTS 10
#define MAX_CURVES 16
#define MAX_SOURCES_PER_CURVE 16
#define HISTORY_SIZE_SEC 60
#define MAX_SOURCE_PATH 256

// Source types
typedef enum {
    SOURCE_FILE,
    SOURCE_GPU
} SourceType;

// Profile point structure
typedef struct {
    double temp;
    double pct;
} ProfilePoint;

// Curve structure
typedef struct {
    char name[32];
    char sources[MAX_SOURCES_PER_CURVE][MAX_SOURCE_PATH];
    int source_types[MAX_SOURCES_PER_CURVE];
    int num_sources;
    ProfilePoint profile[MAX_CURVE_POINTS];
    int profile_count;
} Curve;

// Fan threshold structure
typedef struct {
    char fan_name[32];
    int lower_min, lower_warn, lower_crit;
    int upper_min, upper_warn, upper_crit;
    bool has_upper;
} FanThreshold;

// Global data
Curve curves[MAX_CURVES];
int num_curves = 0;
int history_sec = 10;

FanThreshold fan_thresholds[MAX_CURVES];
int num_fan_thresholds = 0;

char user[20] = "ADMIN";
char password[20] = "ADMIN";
char address[64];
char commandBase[256];

// History tracking
typedef struct {
    int size;
    int index;
    double *temps;
} TempHistory;

// Per-source data
typedef struct {
    int fd;
    nvmlDevice_t device;
    SourceType type;
    TempHistory history;
} SourceData;

SourceData source_data[MAX_CURVES * MAX_SOURCES_PER_CURVE];
int num_sources_total = 0;

// Cleanup resources
void close_resources(void) {
    for (int i = 0; i < num_sources_total; i++) {
        if (source_data[i].type == SOURCE_FILE && source_data[i].fd != -1) {
            close(source_data[i].fd);
            source_data[i].fd = -1;
        }
        free(source_data[i].history.temps);
    }
    nvmlShutdown();
}

// Signal handler
void handle_signal(int sig) {
    (void)sig;
    exit(0);
}

// Read temperature from a specific source index
double read_source_temperature(int source_idx) {
    if (source_idx < 0 || source_idx >= num_sources_total) return 999;
    
    SourceData *src = &source_data[source_idx];

    // GPU source
    if (src->type == SOURCE_GPU) {
        unsigned int temp;
        nvmlReturn_t result = nvmlDeviceGetTemperature(src->device, NVML_TEMPERATURE_GPU, &temp);
        if (result == NVML_SUCCESS) {
            return (double)temp;
        } else {
            fprintf(stderr, "NVML Error reading GPU %d: %s\n", source_idx, nvmlErrorString(result));
            return 999;
        }
    }

    // File source
    if (src->fd == -1) return 999;

    char buf[32];
    lseek(src->fd, 0, SEEK_SET);
    ssize_t bytes = read(src->fd, buf, sizeof(buf)-1);
    if (bytes <= 0) {
        return 999;
    }

    buf[bytes] = '\0';
    return strtod(buf, NULL) / 1000.0;
}

// Calculate fan speed from temperature using curve profile
int calculate_fan_speed(double temp, ProfilePoint *points, int count) {
    if (count == 0 || isnan(temp)) return 0;

    if (temp <= points[0].temp) return 0;
    if (temp >= points[count-1].temp) {
        return (int)(points[count-1].pct * 100);
    }

    for (int i = 0; i < count - 1; i++) {
        if (temp >= points[i].temp && temp < points[i+1].temp) {
            double temp_range = points[i+1].temp - points[i].temp;
            double pct_range = points[i+1].pct - points[i].pct;
            double slope = pct_range / temp_range;
            double pct = points[i].pct + slope * (temp - points[i].temp);
            return (int)(pct * 100);
        }
    }

    return 0;
}

// Helper to get string value from toml_datum_t
static const char* get_string(toml_datum_t val) {
    if (val.type != TOML_STRING || !val.u.str.ptr) return NULL;
    return val.u.str.ptr;
}

// Helper to get int64 value from toml_datum_t
static int64_t get_int64(toml_datum_t val) {
    if (val.type != TOML_INT64) return 0;
    return val.u.int64;
}

// Helper to get fp64 value from toml_datum_t
static double get_fp64(toml_datum_t val) {
    if (val.type != TOML_FP64) return 0.0;
    return val.u.fp64;
}

// Parse the TOML configuration file
void parse_config() {
    // Parse TOML file
    toml_result_t result = toml_parse_file_ex(CONFIG_FILE);
    
    if (!result.ok) {
        fprintf(stderr, "Error: Failed to parse config file: %s\n", result.errmsg);
        exit(EXIT_FAILURE);
    }

    toml_datum_t conf_table = result.toptab;

    // Parse IPMI settings
    toml_datum_t address_val = toml_get(conf_table, "address");
    toml_datum_t user_val = toml_get(conf_table, "user");
    toml_datum_t password_val = toml_get(conf_table, "password");
    toml_datum_t history_sec_val = toml_get(conf_table, "history_sec");

    const char* addr_str = get_string(address_val);
    if (addr_str) {
        snprintf(address, sizeof(address), "%s", addr_str);
    }
    const char* user_str = get_string(user_val);
    if (user_str) {
        snprintf(user, sizeof(user), "%s", user_str);
    }
    const char* pass_str = get_string(password_val);
    if (pass_str) {
        snprintf(password, sizeof(password), "%s", pass_str);
    }
    if (history_sec_val.type == TOML_INT64) {
        history_sec = (int)get_int64(history_sec_val);
    }

    // Parse curves array
    toml_datum_t curves_array = toml_get(conf_table, "curves");
    if (curves_array.type != TOML_ARRAY || curves_array.u.arr.size == 0) {
        fprintf(stderr, "Error: No curves array found in config\n");
        toml_free(result);
        exit(EXIT_FAILURE);
    }

    if (curves_array.u.arr.size > MAX_CURVES) {
        fprintf(stderr, "Error: Too many curves configured (max %d)\n", MAX_CURVES);
        toml_free(result);
        exit(EXIT_FAILURE);
    }

    num_curves = 0;
    num_sources_total = 0;

    for (int i = 0; i < curves_array.u.arr.size; i++) {
        toml_datum_t curve_tbl = curves_array.u.arr.elem[i];
        
        Curve *curve = &curves[num_curves];
        curve->num_sources = 0;
        curve->profile_count = 0;

        // Get curve name
        toml_datum_t name_val = toml_get(curve_tbl, "name");
        const char* name_str = get_string(name_val);
        if (name_str) {
            strncpy(curve->name, name_str, sizeof(curve->name) - 1);
            curve->name[sizeof(curve->name) - 1] = '\0';
        } else {
            snprintf(curve->name, sizeof(curve->name), "curve%d", num_curves + 1);
        }

        // Get sources array
        toml_datum_t sources_array = toml_get(curve_tbl, "sources");
        if (sources_array.type != TOML_ARRAY || sources_array.u.arr.size == 0) {
            fprintf(stderr, "Error: No sources array in curve '%s'\n", curve->name);
            toml_free(result);
            exit(EXIT_FAILURE);
        }

        int num_sources = sources_array.u.arr.size;
        if (curve->num_sources + num_sources > MAX_SOURCES_PER_CURVE) {
            fprintf(stderr, "Error: Too many sources in curve '%s'\n", curve->name);
            toml_free(result);
            exit(EXIT_FAILURE);
        }

        // Parse sources
        for (int j = 0; j < num_sources; j++) {
            toml_datum_t source_val = sources_array.u.arr.elem[j];
            const char* src_str = get_string(source_val);
            if (src_str) {
                strncpy(curve->sources[curve->num_sources], src_str, 
                        MAX_SOURCE_PATH - 1);
                curve->sources[curve->num_sources][MAX_SOURCE_PATH - 1] = '\0';
                curve->num_sources++;
            }
        }

      // Get profile array (array of inline tables with temp and pct)
        toml_datum_t profile_array = toml_get(curve_tbl, "profile");
        
        if (profile_array.type != TOML_ARRAY || profile_array.u.arr.size == 0) {
            fprintf(stderr, "Error: No profile array in curve '%s'\n", curve->name);
            toml_free(result);
            exit(EXIT_FAILURE);
        }

        int num_profile_points = profile_array.u.arr.size;
        if (num_profile_points > MAX_CURVE_POINTS) {
            fprintf(stderr, "Error: Too many profile points in curve '%s' (max %d)\n", 
                    curve->name, MAX_CURVE_POINTS);
            toml_free(result);
            exit(EXIT_FAILURE);
        }

        // Parse profile points from inline tables
        double last_temp = -INFINITY;
        for (int j = 0; j < num_profile_points; j++) {
            toml_datum_t point_tbl = profile_array.u.arr.elem[j];
            
          // Inline tables are stored as TOML_TABLE with key/value arrays
            if (point_tbl.type != TOML_TABLE) {
                fprintf(stderr, "Error: profile point %d is not a table in curve '%s'\n", 
                        j, curve->name);
                toml_free(result);
                exit(EXIT_FAILURE);
            }

            // Search for 'temp' and 'pct' fields in the inline table
            double temp = 0.0;
            double pct = 0.0;
            bool found_temp = false;
            bool found_pct = false;

            for (int k = 0; k < point_tbl.u.tab.size; k++) {
                const char* key_name = point_tbl.u.tab.key[k];
                toml_datum_t* val = &point_tbl.u.tab.value[k];

                if (strcmp(key_name, "temp") == 0) {
                    if (val->type == TOML_FP64) {
                        temp = val->u.fp64;
                    } else if (val->type == TOML_INT64) {
                        temp = (double)val->u.int64;
                    } else {
                        fprintf(stderr, "Error: 'temp' field must be number in curve '%s'\n", 
                                curve->name);
                        toml_free(result);
                        exit(EXIT_FAILURE);
                    }
                    found_temp = true;
                } else if (strcmp(key_name, "pct") == 0) {
                    if (val->type == TOML_FP64) {
                        pct = val->u.fp64;
                    } else if (val->type == TOML_INT64) {
                        pct = (double)val->u.int64;
                    } else {
                        fprintf(stderr, "Error: 'pct' field must be number in curve '%s'\n", 
                                curve->name);
                        toml_free(result);
                        exit(EXIT_FAILURE);
                    }
                    found_pct = true;
                }
            }

            if (!found_temp) {
                fprintf(stderr, "Error: profile point %d missing 'temp' field in curve '%s'\n", 
                        j, curve->name);
                toml_free(result);
                exit(EXIT_FAILURE);
            }

            if (!found_pct) {
                fprintf(stderr, "Error: profile point %d missing 'pct' field in curve '%s'\n", 
                        j, curve->name);
                toml_free(result);
                exit(EXIT_FAILURE);
            }

            // Validate temperature is sorted ascending
            if (temp < last_temp) {
                fprintf(stderr, "Error: profile points must be sorted by temp ascending in curve '%s'\n", 
                        curve->name);
                toml_free(result);
                exit(EXIT_FAILURE);
            }
            last_temp = temp;

            // Validate percentage is in range [0.0, 1.0]
            if (pct < 0.0 || pct > 1.0) {
                fprintf(stderr, "Error: profile pct values must be between 0.0 and 1.0 in curve '%s'\n", 
                        curve->name);
                toml_free(result);
                exit(EXIT_FAILURE);
            }

            curve->profile[curve->profile_count].temp = temp;
            curve->profile[curve->profile_count].pct = pct;
            curve->profile_count++;
        }

        num_curves++;
    }

    // Parse fan_thresholds array
    toml_datum_t thresholds_array = toml_get(conf_table, "fan_thresholds");
    if (thresholds_array.type == TOML_ARRAY && thresholds_array.u.arr.size > 0) {
        if (thresholds_array.u.arr.size > MAX_CURVES) {
            fprintf(stderr, "Error: Too many fan thresholds configured\n");
            toml_free(result);
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < thresholds_array.u.arr.size; i++) {
            toml_datum_t tbl = thresholds_array.u.arr.elem[i];
            FanThreshold *ft = &fan_thresholds[num_fan_thresholds];
            
            // Parse fan name
            toml_datum_t fan_val = toml_get(tbl, "fan");
            const char *fan_str = get_string(fan_val);
            if (!fan_str) {
                fprintf(stderr, "Error: 'fan' field required in threshold %d\n", i);
                toml_free(result);
                exit(EXIT_FAILURE);
            }
            strncpy(ft->fan_name, fan_str, sizeof(ft->fan_name) - 1);
            ft->fan_name[sizeof(ft->fan_name) - 1] = '\0';
            
            // Parse lower array [min, warn, crit]
            toml_datum_t lower_arr = toml_get(tbl, "lower");
            if (lower_arr.type != TOML_ARRAY || lower_arr.u.arr.size != 3) {
                fprintf(stderr, "Error: 'lower' must be array of 3 values\n");
                toml_free(result);
                exit(EXIT_FAILURE);
            }
            ft->lower_min = (int)get_int64(lower_arr.u.arr.elem[0]);
            ft->lower_warn = (int)get_int64(lower_arr.u.arr.elem[1]);
            ft->lower_crit = (int)get_int64(lower_arr.u.arr.elem[2]);
            
            // Parse upper array (optional)
            toml_datum_t upper_arr = toml_get(tbl, "upper");
            if (upper_arr.type == TOML_ARRAY && upper_arr.u.arr.size == 3) {
                ft->upper_min = (int)get_int64(upper_arr.u.arr.elem[0]);
                ft->upper_warn = (int)get_int64(upper_arr.u.arr.elem[1]);
                ft->upper_crit = (int)get_int64(upper_arr.u.arr.elem[2]);
                ft->has_upper = true;
            } else {
                ft->has_upper = false;
            }
            
            num_fan_thresholds++;
        }
    }

    toml_free(result);

    if (num_curves == 0) {
        fprintf(stderr, "Error: No curves configured\n");
        exit(EXIT_FAILURE);
    }

    // Allocate history arrays for all sources
    int total_sources = 0;
    for (int c = 0; c < num_curves; c++) {
        total_sources += curves[c].num_sources;
    }

    int history_size = (history_sec * 1000 + SEND_DELAY_MS - 1) / SEND_DELAY_MS;
    for (int i = 0; i < total_sources; i++) {
        source_data[i].history.size = history_size;
        source_data[i].history.index = 0;
        source_data[i].history.temps = calloc(history_size, sizeof(double));
        if (!source_data[i].history.temps) {
            fprintf(stderr, "Error: Failed to allocate memory for history\n");
            exit(EXIT_FAILURE);
        }
        source_data[i].fd = -1;
    }
    num_sources_total = total_sources;
}

// Initialize NVML and open temperature sources
void open_temp_sources() {
    // Initialize NVML
    nvmlReturn_t result = nvmlInit();
    if (result != NVML_SUCCESS) {
        fprintf(stderr, "Error: NVML initialization failed: %s\n", nvmlErrorString(result));
        exit(EXIT_FAILURE);
    }

    int source_idx = 0;

    // Process each curve and its sources
    for (int c = 0; c < num_curves; c++) {
        Curve *curve = &curves[c];
        printf("Curve '%s' (%d sources):\n", curve->name, curve->num_sources);

        for (int s = 0; s < curve->num_sources; s++) {
            SourceData *src = &source_data[source_idx];

            // Check if this is a GPU source (format: gpu:0)
            if (strncmp(curve->sources[s], "gpu:", 4) == 0) {
                src->type = SOURCE_GPU;
                int gpu_index = atoi(curve->sources[s] + 4);

                result = nvmlDeviceGetHandleByIndex(gpu_index, &src->device);
                if (result != NVML_SUCCESS) {
                    fprintf(stderr, "Error: Cannot get GPU handle for index %d: %s\n", 
                            gpu_index, nvmlErrorString(result));
                    exit(EXIT_FAILURE);
                }

                // Get and print GPU name for verification
                char name[NVML_DEVICE_NAME_BUFFER_SIZE];
                nvmlDeviceGetName(src->device, name, sizeof(name));
                printf("  Source %d: GPU %d - %s\n", source_idx, gpu_index, name);

            } else {
                // File source
                src->type = SOURCE_FILE;
                src->fd = open(curve->sources[s], O_RDONLY);
                if (src->fd == -1) {
                    fprintf(stderr, "Error: Could not open temperature file '%s': %s\n", 
                            curve->sources[s], strerror(errno));
                    exit(EXIT_FAILURE);
                }
                printf("  Source %d: File - %s\n", source_idx, curve->sources[s]);
            }

            source_idx++;
        }
    }
}

// Update temperature history for a source
void update_history(SourceData *src, double temp) {
    src->history.temps[src->history.index] = temp;
    src->history.index = (src->history.index + 1) % src->history.size;
}

// Get maximum temperature from history
double get_max_temp(SourceData *src) {
    double max = -INFINITY;
    for (int i = 0; i < src->history.size; i++) {
        if (src->history.temps[i] > max) max = src->history.temps[i];
    }
    return max;
}

// Find maximum value in array
int findMax(int arr[], int size) {
    if (size <= 0) return 0;
    int max = arr[0];
    for (int i = 1; i < size; i++) {
        if (arr[i] > max) {
            max = arr[i];
        }
    }
    return max;
}

// Execute a command
void runCommand(const char *format, ...) {
    va_list args;
    char commandArguments[256];
    char command[512];
    
    // Format the command string
    va_start(args, format);
    vsnprintf(commandArguments, sizeof(commandArguments), format, args);
    va_end(args);

    snprintf(command, sizeof(command), "%s %s", commandBase, commandArguments);

    // Execute the command
    int ret = system(command);
    (void)ret;
}

int main() {
    atexit(close_resources);
    signal(SIGINT, handle_signal);

    parse_config();
    open_temp_sources();

    snprintf(commandBase, sizeof(commandBase), "ipmitool -H %s -U %s -P %s", address, user, password);

    // Apply fan thresholds if configured
    if (num_fan_thresholds > 0) {
        for (int i = 0; i < num_fan_thresholds; i++) {
            FanThreshold *ft = &fan_thresholds[i];
            printf("Setting threshold for %s: lower [%d, %d, %d]\n", 
                   ft->fan_name, ft->lower_min, ft->lower_warn, ft->lower_crit);
            
            runCommand("sensor thresh %s lower %d %d %d",
                       ft->fan_name, ft->lower_min, ft->lower_warn, ft->lower_crit);
            
            if (ft->has_upper) {
                printf("Setting threshold for %s: upper [%d, %d, %d]\n", 
                       ft->fan_name, ft->upper_min, ft->upper_warn, ft->upper_crit);
                runCommand("sensor thresh %s upper %d %d %d",
                           ft->fan_name, ft->upper_min, ft->upper_warn, ft->upper_crit);
            }
        }
    } else {
        printf("No fan thresholds configured, skipping threshold setup\n");
    }

    // Enable fan control
    runCommand("raw 0x30 0x45 0x01 0x01");

    usleep(5000 * 1000); // give the commands above enough time to apply before setting fan speed

    int lastSetMax = 0;

    while (1) {
        int fan_speeds[MAX_CURVES] = {0};

        // Process each curve
        for (int c = 0; c < num_curves; c++) {
            Curve *curve = &curves[c];
            double max_temp = -INFINITY;

            // Read all sources in this curve and find max temperature
            for (int s = 0; s < curve->num_sources; s++) {
                int source_idx = 0;
                for (int prev_c = 0; prev_c < c; prev_c++) {
                    source_idx += curves[prev_c].num_sources;
                }
                source_idx += s;

                SourceData *src = &source_data[source_idx];
                double temp = read_source_temperature(source_idx);
                
                if (!isnan(temp)) {
                    update_history(src, temp);
                    double curve_max = get_max_temp(src);
                    if (curve_max > max_temp) {
                        max_temp = curve_max;
                    }
                }
            }

            // Calculate fan speed using curve's profile
            if (!isnan(max_temp) && max_temp > -INFINITY) {
                fan_speeds[c] = calculate_fan_speed(max_temp, 
                    curve->profile, 
                    curve->profile_count);
            }
        }

        // Find maximum fan speed across all curves
        int max = findMax(fan_speeds, num_curves);

        if (lastSetMax != max) {
            lastSetMax = max;
            runCommand("raw 0x30 0x70 0x66 0x01 0x00 0x%X", max);
        }

        usleep(SEND_DELAY_MS * 1000);
    }
    return 0;
}

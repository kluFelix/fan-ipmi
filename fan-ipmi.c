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

// Configuration defaults
#define CONFIG_FILE "/etc/fan-control/fan.conf"
#define SEND_DELAY_MS 500
#define RETRY_DELAY_MS 7500
#define MAX_CURVE_POINTS 10
#define MAX_TEMP_FILES 16
#define HISTORY_SIZE_SEC 60

// source types
typedef enum {
    SOURCE_FILE,
    SOURCE_GPU
} SourceType;

char *temp_files[MAX_TEMP_FILES] = {NULL};
int temp_fds[MAX_TEMP_FILES] = {-1};
nvmlDevice_t gpu_devices[MAX_TEMP_FILES] = {0};
SourceType source_types[MAX_TEMP_FILES] = {SOURCE_FILE};
int curve_ids[MAX_TEMP_FILES] = {0};
double curve_temps[MAX_TEMP_FILES][MAX_CURVE_POINTS];
double curve_pcts[MAX_TEMP_FILES][MAX_CURVE_POINTS];
int curve_counts[MAX_TEMP_FILES] = {0};
int num_sensors = 0;
int history_sec = 10;

char user[20] = "ADMIN";
char password[20] = "ADMIN";
char address[20];
char commandBase[128];

// History tracking
typedef struct {
    int size;
    int index;
    double *temps;
} TempHistory;

TempHistory histories[MAX_TEMP_FILES];

// Cleanup resources
void close_resources(void) {
    for (int i = 0; i < num_sensors; i++) {
        if (temp_fds[i] != -1) {
            close(temp_fds[i]);
            temp_fds[i] = -1;
        }
        free(histories[i].temps);
        free(temp_files[i]);
    }
    nvmlShutdown();
}

// Signal handler
void handle_signal(int sig) {
    (void)sig;
    exit(0);
}

// Read temperature from file OR GPU
double read_temperature(int index) {
    if (index < 0 || index >= MAX_TEMP_FILES) return 999;

    // GPU source
    if (source_types[index] == SOURCE_GPU) {
        unsigned int temp;
        nvmlReturn_t result = nvmlDeviceGetTemperature(gpu_devices[index], NVML_TEMPERATURE_GPU, &temp);
        if (result == NVML_SUCCESS) {
            return (double)temp;
        } else {
            fprintf(stderr, "NVML Error reading GPU %d: %s\n", index, nvmlErrorString(result));
            return 999;
        }
    }

    // File source
    if (temp_fds[index] == -1) return 999;

    char buf[32];
    lseek(temp_fds[index], 0, SEEK_SET);
    ssize_t bytes = read(temp_fds[index], buf, sizeof(buf)-1);
    if (bytes <= 0) {
        return 999;
    }

    buf[bytes] = '\0';
    return strtod(buf, NULL) / 1000.0;
}

// Initialize NVML and open files
void open_temp_sources() {
    // Initialize NVML
    nvmlReturn_t result = nvmlInit();
    if (result != NVML_SUCCESS) {
        fprintf(stderr, "Error: NVML initialization failed: %s\n", nvmlErrorString(result));
        exit(EXIT_FAILURE);
    }

    // Process each configured source
    for (int i = 0; i < num_sensors; i++) {
        if (!temp_files[i]) continue;

        // Check if this is a GPU source (format: gpu:0)
        if (strncmp(temp_files[i], "gpu:", 4) == 0) {
            int gpu_index = atoi(temp_files[i] + 4);
            source_types[i] = SOURCE_GPU;

            nvmlReturn_t result = nvmlDeviceGetHandleByIndex(gpu_index, &gpu_devices[i]);
            if (result != NVML_SUCCESS) {
                fprintf(stderr, "Error: Cannot get GPU handle for index %d: %s\n", 
                        gpu_index, nvmlErrorString(result));
                exit(EXIT_FAILURE);
            }

            // Get and print GPU name for verification
            char name[NVML_DEVICE_NAME_BUFFER_SIZE];
            nvmlDeviceGetName(gpu_devices[i], name, sizeof(name));
            printf("Source %d: GPU %d - %s\n", i, gpu_index, name);

        } else {
            // File source
            source_types[i] = SOURCE_FILE;
            temp_fds[i] = open(temp_files[i], O_RDONLY);
            if (temp_fds[i] == -1) {
                fprintf(stderr, "Error: Could not open temperature file '%s': %s\n", 
                        temp_files[i], strerror(errno));
                exit(EXIT_FAILURE);
            }
            printf("Source %d: File - %s\n", i, temp_files[i]);
        }
    }
}

// Calculate fan speed from temperature using specified curve
int calculate_fan_speed(double temp, double *temps, double *pcts, int count) {
    if (count == 0 || isnan(temp)) return 0;

    if (temp <= temps[0]) return 0;
    if (temp >= temps[count-1]) {
        return (int)(pcts[count-1] * 100);
    }

    for (int i = 0; i < count - 1; i++) {
        if (temp >= temps[i] && temp < temps[i+1]) {
            double temp_range = temps[i+1] - temps[i];
            double pct_range = pcts[i+1] - pcts[i];
            double slope = pct_range / temp_range;
            double pct = pcts[i] + slope * (temp - temps[i]);
            return (int)(pct * 100);
        }
    }

    return 0;
}

// Parse configuration file
void parse_config() {
    FILE* conf = fopen(CONFIG_FILE, "r");
    if (!conf) {
        fprintf(stderr, "Error: Could not open config file '%s': %s\n", CONFIG_FILE, strerror(errno));
        exit(EXIT_FAILURE);
    }

    char line[256];
    int temp_file_idx = 0;
    int current_curve = 0;

    while (fgets(line, sizeof(line), conf)) {
        char* comment = strchr(line, '#');
        if (comment) *comment = '\0';
        char* key = strtok(line, " \t:");
        if (!key) continue;
        char* value = strtok(NULL, " \t\r\n");

        // Handle sensorX_curve_id mapping
        if (strncmp(key, "sensor", 6) == 0) {
            char* underscore = strchr(key, '_');
            if (underscore && strncmp(underscore, "_curve_id", 9) == 0) {
                int sensor_idx = atoi(key + 6);
                if (sensor_idx >= 1 && sensor_idx <= MAX_TEMP_FILES && value) {
                    curve_ids[sensor_idx - 1] = atoi(value);
                }
                continue;
            }
        }

        // Handle curve sections
        if (strncmp(key, "curve", 5) == 0) {
            int curve_idx = atoi(key + 5);
            if (curve_idx >= 1 && curve_idx <= MAX_TEMP_FILES) {
                current_curve = curve_idx;
            }
            continue;
        }

        if (current_curve > 0 && current_curve <= MAX_TEMP_FILES && value) {
            double temp = strtod(key, NULL);
            double pct = strtod(value, NULL);
            if (curve_counts[current_curve] < MAX_CURVE_POINTS) {
                curve_temps[current_curve][curve_counts[current_curve]] = temp;
                curve_pcts[current_curve][curve_counts[current_curve]] = pct;
                curve_counts[current_curve]++;
            }
        } else {
            // Handle temp_fileX
            if (strncmp(key, "temp_file", 9) == 0) {
                int sensor_idx = atoi(key + 9);
                if (sensor_idx >= 1 && sensor_idx <= MAX_TEMP_FILES && value && temp_file_idx < MAX_TEMP_FILES) {
                    temp_files[temp_file_idx++] = strdup(value);
                    if (sensor_idx > num_sensors) {
                        num_sensors = sensor_idx;
                    }
                }
            } else if (strcmp(key, "history_sec") == 0 && value) {
                history_sec = atoi(value);
            } else if (strcmp(key, "address") == 0 && value) {
                snprintf(address, sizeof(address), "%s", value);
            } else if (strcmp(key, "user") == 0 && value) {
                snprintf(user, sizeof(user), "%s", value);
            } else if (strcmp(key, "password") == 0 && value) {
                snprintf(password, sizeof(password), "%s", value);
            }
        }
    }
    fclose(conf);

    if (num_sensors == 0) {
        fprintf(stderr, "Error: No sensors configured\n");
        exit(EXIT_FAILURE);
    }

    int history_size = (history_sec * 1000 + SEND_DELAY_MS - 1) / SEND_DELAY_MS;
    for (int i = 0; i < num_sensors; i++) {
        histories[i].size = history_size;
        histories[i].index = 0;
        histories[i].temps = calloc(history_size, sizeof(double));
        if (!histories[i].temps) {
            fprintf(stderr, "Error: Failed to allocate memory for history\n");
            exit(EXIT_FAILURE);
        }
    }
}

// Update temperature history
void update_history(TempHistory* hist, double temp) {
    hist->temps[hist->index] = temp;
    hist->index = (hist->index + 1) % hist->size;
}

// Get maximum temperature from history
double get_max_temp(TempHistory* hist) {
    double max = -INFINITY;
    for (int i = 0; i < hist->size; i++) {
        if (hist->temps[i] > max) max = hist->temps[i];
    }
    return max;
}

int findMax(int arr[], int size) {
    int max = arr[0];
    for (int i = 1; i < size; i++) {
        if (arr[i] > max) {
            max = arr[i];
        }
    }
    return max;
}

void runCommand(const char *format, ...) {
    va_list args;
    char commandArguments[128];
    char command[256];
    
    // Format the command string
    va_start(args, format);
    vsnprintf(commandArguments, sizeof(commandArguments), format, args);
    va_end(args);

    snprintf(command, sizeof(command), "%s %s", commandBase, commandArguments);

    // Execute the command and get its return value
    //printf("%s\n", commandArguments);
    int ret = system(command);
}

int main() {
    atexit(close_resources);
    signal(SIGINT, handle_signal);

    parse_config();
    open_temp_sources();

    snprintf(commandBase, sizeof(commandBase), "ipmitool -H %s -U %s -P %s", address, user, password);

    runCommand("sensor thresh CPU_FAN2 lower 0 0 100");
    runCommand("sensor thresh SYS_FAN1 lower 0 0 100");
    runCommand("sensor thresh SYS_FAN2 lower 0 0 100");
    runCommand("sensor thresh SYS_FAN3 lower 0 0 100");

    runCommand("raw 0x30 0x45 0x01 0x01");

    usleep(5000 * 1000); // give the commands above enough time to apply before setting fan speed

    int lastSetMax = 0;

    while (1) {
        int fan_speeds[MAX_TEMP_FILES] = {0};

        for (int i = 0; i < num_sensors; i++) {
            if (!temp_files[i]) continue;  // Skip empty sources

            // Read temperature using index (handles both GPU and file)
            double temp = read_temperature(i);
            if (!isnan(temp)) {
                update_history(&histories[i], temp);
                double max_temp = get_max_temp(&histories[i]);

                int curve_id = curve_ids[i];
                fan_speeds[i] = calculate_fan_speed(max_temp, curve_temps[curve_id], curve_pcts[curve_id], curve_counts[curve_id]);
            }
        }

        char percentages[32];
        char maxAsHex[5];
        int max = findMax(fan_speeds, num_sensors);

        if (lastSetMax != max) {
            lastSetMax = max;
            runCommand("raw 0x30 0x70 0x66 0x01 0x00 0x%X", max);
        }

        usleep(SEND_DELAY_MS * 1000);
    }
    return 0;
}


#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <nvml.h>

// Configuration defaults
#define CONFIG_FILE "/etc/fan-control/fan.conf"
#define SEND_DELAY_MS 3000
#define RETRY_DELAY_MS 7500
#define MAX_CURVE_POINTS 10
#define MAX_TEMP_FILES 3
#define HISTORY_SIZE_SEC 60

// source types
typedef enum {
    SOURCE_FILE,
    SOURCE_GPU
} SourceType;

char *temp_files[MAX_TEMP_FILES] = {NULL};
int temp_fds[MAX_TEMP_FILES] = {-1, -1, -1};
nvmlDevice_t gpu_devices[MAX_TEMP_FILES] = {0};
SourceType source_types[MAX_TEMP_FILES] = {SOURCE_FILE}; // Track source type
double curve_temps1[MAX_CURVE_POINTS];
double curve_pcts1[MAX_CURVE_POINTS];
double curve_temps2[MAX_CURVE_POINTS];
double curve_pcts2[MAX_CURVE_POINTS];
double curve_temps3[MAX_CURVE_POINTS];
double curve_pcts3[MAX_CURVE_POINTS];
int curve_count1 = 0;
int curve_count2 = 0;
int curve_count3 = 0;
int history_sec = 10;

char user[20] = "ADMIN";
char password[20] = "ADMIN";
char address[20];

// History tracking
typedef struct {
    int size;
    int index;
    double *temps;
} TempHistory;

TempHistory histories[MAX_TEMP_FILES];

// Cleanup resources
void close_resources(void) {
    for (int i = 0; i < MAX_TEMP_FILES; i++) {
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
    for (int i = 0; i < MAX_TEMP_FILES; i++) {
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

        // Handle curve sections
        if (strcmp(key, "curve1") == 0) {
            current_curve = 1;
            continue;
        } else if (strcmp(key, "curve2") == 0) {
            current_curve = 2;
            continue;
        } else if (strcmp(key, "curve3") == 0) {
            current_curve = 3;
            continue;
        }

        if (current_curve == 1 && value) {
            double temp = strtod(key, NULL);
            double pct = strtod(value, NULL);
            if (curve_count1 < MAX_CURVE_POINTS) {
                curve_temps1[curve_count1] = temp;
                curve_pcts1[curve_count1] = pct;
                curve_count1++;
            }
        } else if (current_curve == 2 && value) {
            double temp = strtod(key, NULL);
            double pct = strtod(value, NULL);
            if (curve_count2 < MAX_CURVE_POINTS) {
                curve_temps2[curve_count2] = temp;
                curve_pcts2[curve_count2] = pct;
                curve_count2++;
            }
        } else if (current_curve == 3 && value) {
            double temp = strtod(key, NULL);
            double pct = strtod(value, NULL);
            if (curve_count3 < MAX_CURVE_POINTS) {
                curve_temps3[curve_count3] = temp;
                curve_pcts3[curve_count3] = pct;
                curve_count3++;
            }
        } else {
            if (strcmp(key, "temp_file1") == 0 && value && temp_file_idx < MAX_TEMP_FILES) {
                temp_files[temp_file_idx++] = strdup(value);
            } else if (strcmp(key, "temp_file2") == 0 && value && temp_file_idx < MAX_TEMP_FILES) {
                temp_files[temp_file_idx++] = strdup(value);
            } else if (strcmp(key, "temp_file3") == 0 && value && temp_file_idx < MAX_TEMP_FILES) {
                temp_files[temp_file_idx++] = strdup(value);
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

    int history_size = (history_sec * 1000 + SEND_DELAY_MS - 1) / SEND_DELAY_MS;
    for (int i = 0; i < MAX_TEMP_FILES; i++) {
        histories[i].size = history_size;
        histories[i].index = 0;
        histories[i].temps = calloc(history_size, sizeof(double));
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

int main() {
    atexit(close_resources);
    signal(SIGINT, handle_signal);
    
    parse_config();
    open_temp_sources();

    char commandBase[200];
    char command[200];
    snprintf(commandBase, sizeof(commandBase), "ipmitool -H %s -U %s -P %s", address, user, password);

    snprintf(command, sizeof(command), "%s sensor thresh CPU_FAN2 lower 200 300 300", commandBase);
    system(command);
    snprintf(command, sizeof(command), "%s sensor thresh SYS_FAN1 lower 200 300 300", commandBase);
    system(command);
    snprintf(command, sizeof(command), "%s sensor thresh SYS_FAN2 lower 0 0 0", commandBase);
    system(command);

    snprintf(command, sizeof(command), "%s raw 0x30 0x45 0x01 0x01", commandBase);
    system(command);

    usleep(500 * 1000); // give the commands above enough time to apply before setting fan speed

    int lastSetMax = 0;

    while (1) {
        int fan_speeds[MAX_TEMP_FILES] = {0};
        
        for (int i = 0; i < MAX_TEMP_FILES; i++) {
            if (!temp_files[i]) continue;  // Skip empty sources
            
            // Read temperature using index (handles both GPU and file)
            double temp = read_temperature(i);
            if (!isnan(temp)) {
                update_history(&histories[i], temp);
                double max_temp = get_max_temp(&histories[i]);
                
                switch (i) {
                    case 0:
                        fan_speeds[i] = calculate_fan_speed(max_temp, curve_temps1, curve_pcts1, curve_count1);
                        break;
                    case 1:
                        fan_speeds[i] = calculate_fan_speed(max_temp, curve_temps2, curve_pcts2, curve_count2);
                        break;
                    case 2:
                        fan_speeds[i] = calculate_fan_speed(max_temp, curve_temps3, curve_pcts3, curve_count3);
                        break;
                }
            }
        }

        char percentages[32];
        char maxAsHex[5];
        int max = findMax(fan_speeds, sizeof(fan_speeds) / sizeof(fan_speeds[0]));

        // ToDo: This if statement does not seem to work as expected!
        if (lastSetMax != max) {
            lastSetMax = max;

            snprintf(command, sizeof(command), "%s raw 0x30 0x70 0x66 0x01 0x00 0x%X",commandBase, max);
            system(command);

            snprintf(percentages, sizeof(percentages), "%d,%d,%d", fan_speeds[0], fan_speeds[1], fan_speeds[2]);
            printf("Percentages: %s | Max: %d | Max as hex: 0x%X | %s\n", percentages, max, max, command);
        }

        usleep(SEND_DELAY_MS * 1000);
    }
    return 0;
}

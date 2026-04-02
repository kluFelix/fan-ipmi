## IPMI Fan Control

### Configuration

The configuration file needs to be at `/etc/fan-ipmi/fan.toml`.
A sample configuration is included in the repository.

**Structure**
- `[[curves]]` - Array of curve tables (use `[[curves]]` for multiple curves)
- `curves.name` - Optional curve identifier (used for logging)
- `curves.sources` - Array of sensor identifiers to monitor for this curve
- `curves.profile` - Array of profile points, each with:
  - `temp` - Temperature threshold in Celsius
  - `pct` - Fan percentage (0.0 to 1.0) at that temperature

**Example**
```toml
# IPMI connection settings
address = "123.123.123.123"
user = "ADMIN"
password = "ADMIN"

# History window in seconds (how far back to look for max temperature)
history_sec = 30

[[curves]]
name = "gpu"
sources = ["gpu:0", "gpu:1"]
profile = [
    { temp = 25, pct = 0.0 },
    { temp = 55, pct = 0.5 },
    { temp = 85, pct = 1.0 }
]

[[curves]]
name = "cpu"
sources = ["/sys/class/thermal/thermal_zone0/temp"]
profile = [
    { temp = 30, pct = 0.0 },
    { temp = 40, pct = 0.3 },
    { temp = 50, pct = 0.5 },
    { temp = 60, pct = 0.7 },
    { temp = 70, pct = 1.0 }
]
```

### Dependencies

The package includes:
- [ipmitool](https://github.com/ipmitool/ipmitool)
- [tomlc17](https://github.com/cktan/tomlc17)
- NVIDIA NVML

### Disclaimer

Assited by:
- Qwen3.5 35B
- Devstral 24B
- Kimi K2


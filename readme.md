## Nix Integration Guide

**Written by Devstral 24b!**

### Overview

This project is now fully integrated with Nix/NixOS. You can:
1. Build the package using `nix build`
2. Use it as a development environment with `nix develop`
3. Install it as a system service in NixOS

### Usage

### Building the Package

```bash
# Build the package
nix build .#fan-ipmi

# Run the binary (note: needs config file at /etc/fan-control/fan.conf)
./result/bin/fan-ipmi
```

#### Development Environment

```bash
# Enter development shell with all dependencies
nix develop
```

#### Using in NixOS Configuration

Add this to your `configuration.nix`:

```nix
{ config, pkgs, ... }:
  {
    imports = [ 
      inputs.fan-ipmi.nixosModules.default
    ];
    
    # Enable the fan-ipmi service
    systemd.services.fan-ipmi.enable = true;
  }
```

Or use it directly from your flake:

```nix
{
  inputs.fan-ipmi.url = "git+file:/path/to/fan-ipmi";
  
  outputs = { self, nixpkgs, fan-ipmi, ... } @ inputs: {
    nixosConfigurations.your-hostname = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        ./configuration.nix
        fan-ipmi.nixosModules.default
      ];
    };
  };
}
```

### Configuration File

The configuration file is located at `/etc/fan-control/fan.conf` when installed.
A sample configuration is included in the repository.

### Package Outputs

- `.#fan-ipmi` - The main package
- `.#packages.x86_64-linux.default` - Alias to fan-ipmi
- `.#nixosModules.default` - NixOS module for the service
- `.#devShells.x86_64-linux.default` - Development shell

### Dependencies

The package includes:
- ipmitool (for IPMI commands)
- NVIDIA NVML (for GPU temperature monitoring)
- GCC (for compilation)

### Service Configuration

The systemd service is configured with:
- Automatic restart on failure
- Runs as root
- Starts at boot (multi-user.target)


{  
  description = "A basic flake for c development";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
  
  outputs = { self, nixpkgs, ... }: let
    system = "x86_64-linux";
    pkgs = import nixpkgs {
      inherit system;
      config.allowUnfree = true;
    };

    # NVIDIA driver providing libnvidia-ml.so
    nvidiaDriver = pkgs.linuxPackages_latest.nvidia_x11;

    fanIpmiPackage = pkgs.stdenv.mkDerivation {
      pname = "fan-ipmi";
      version = "1.0";
      src = ./.;
      
      nativeBuildInputs = with pkgs; [
        gcc
        pkg-config
      ];
      
      buildInputs = with pkgs; [
        ipmitool
        cudaPackages.cuda_nvml_dev
        nvidiaDriver
      ];
      
      # Tell the linker where to find libnvidia-ml.so
      NIX_LDFLAGS = pkgs.lib.concatStringsSep " " [
        "-L${pkgs.cudaPackages.cuda_nvml_dev}/lib"
        "-L${nvidiaDriver}/lib"
      ];
      
      # Optional but helpful so the binary finds the library at runtime
      LD_LIBRARY_PATH = "${nvidiaDriver}/lib"
        + (let prev = builtins.getEnv "LD_LIBRARY_PATH"; in
          if prev == "" then "" else ":${prev}");
      
      buildPhase = ''
        gcc -o fan-ipmi fan-ipmi.c -lnvidia-ml -lm
      '';
      
      installPhase = ''
        mkdir -p $out/bin
        cp fan-ipmi $out/bin/
        mkdir -p $out/etc/fan-control
        cp ${./fan.conf} $out/etc/fan-control/fan.conf
      '';
    };

    fanIpmiService = { config, lib, pkgs, ... }: {
      systemd.services.fan-ipmi = {
        description = "Fan IPMI Control Service";
        wantedBy = [ "multi-user.target" ];
        after = [ "multi-user.target" ];
        path = with pkgs; [ ipmitool ]; # this is required to actually change the fan speed
        
        serviceConfig = {
          Type = "simple";
          Restart = "on-failure";
          ExecStart = "${fanIpmiPackage}/bin/fan-ipmi";
          User = "root";
          Group = "root";
        };
      };
    };

  in {
    packages.${system} = {
      default = fanIpmiPackage;
      fan-ipmi = fanIpmiPackage;
    };

    nixosModules.default = fanIpmiService;

    devShells."${system}".default = pkgs.mkShell {
        buildInputs = with pkgs; [
          cudaPackages.cuda_nvml_dev
        ];

        nativeBuildInputs = with pkgs; [
          gcc
          clang
          clang-tools # for LSP support
          gdb

          cudaPackages.cuda_nvml_dev
          nvidiaDriver
        ];

        # Tell the linker where to find libnvidia-ml.so
        NIX_LDFLAGS = pkgs.lib.concatStringsSep " " [
          "-L${pkgs.cudaPackages.cuda_nvml_dev}/lib"
          "-L${nvidiaDriver}/lib"
        ];

        # Optional but helpful so the binary finds the library at runtime
        LD_LIBRARY_PATH = "${nvidiaDriver}/lib"
          + (let prev = builtins.getEnv "LD_LIBRARY_PATH"; in
            if prev == "" then "" else ":${prev}");
      };
  };
}


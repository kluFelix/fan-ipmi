{
  description = "IPMI Fan Control";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        config.allowUnfree = true;
      };
    in {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "fan-ipmi";
        version = "0.1.0";
        src = ./.;
        nativeBuildInputs = with pkgs; [ gcc pkg-config patchelf makeWrapper ];

        buildInputs = with pkgs; [ 
          linuxPackages.nvidia_x11
          cudaPackages.cuda_nvml_dev
        ];

        buildCommand = ''
          mkdir -p $out/bin
          gcc -o $out/bin/fan-ipmi $src/fan-ipmi.c $src/tomlc17.c \
            -I${pkgs.linuxPackages.nvidia_x11}/include \
            -L${pkgs.linuxPackages.nvidia_x11}/lib \
            -lnvidia-ml -lm

          # Runtime link only against system driver
          patchelf --set-rpath '/run/opengl-driver/lib' $out/bin/fan-ipmi

          # Ensure wrapper sets library path
          wrapProgram $out/bin/fan-ipmi \
            --prefix LD_LIBRARY_PATH : /run/opengl-driver/lib
        '';
      };

      apps.${system}.default = {
        type = "app";
        program = "${self.packages.${system}.default}/bin/fan-ipmi";
      };

      devShells.${system}.default = pkgs.mkShell {
        buildInputs = with pkgs; [
          cudaPackages.cuda_nvml_dev
          linuxPackages.nvidia_x11
        ];

        nativeBuildInputs = with pkgs; [
          gcc
          clang
          clang-tools
          gdb
        ];

        NIX_LDFLAGS = "-L${pkgs.linuxPackages.nvidia_x11}/lib";
      };

      nixosModules.fan-ipmi = { config, lib, pkgs, ... }: let
        cfg = config.services.fan-ipmi;
        fanIpmiPackage = self.packages.${system}.default;
      in {
        options.services.fan-ipmi = {
          enable = lib.mkEnableOption "fan-ipmi";

          address = lib.mkOption {
            type = lib.types.str;
            description = "IPMI address";
          };

          user = lib.mkOption {
            type = lib.types.str;
            default = "ADMIN";
            description = "IPMI user";
          };

          password = lib.mkOption {
            type = lib.types.str;
            default = "ADMIN";
            description = "IPMI password";
          };

          historySec = lib.mkOption {
            type = lib.types.int;
            default = 30;
            description = "Temperature history window in seconds";
          };

          configPath = lib.mkOption {
            type = lib.types.path;
            default = /etc/fan-ipmi/fan.toml;
            description = "Path to configuration file";
          };
        };

        config = lib.mkIf cfg.enable {
          systemd.services.fan-ipmi = {
            description = "IPMI Fan Control Service";
            wantedBy = [ "multi-user.target" ];
            after = [ "multi-user.target" ];
            path = with pkgs; [ ipmitool ];

            serviceConfig = {
              Type = "simple";
              ExecStart = "${fanIpmiPackage}/bin/fan-ipmi";
              Restart = "on-failure";
              User = "root";
              Group = "root";

              DynamicUser = true;
              NoNewPrivileges = true;
              ProtectSystem = "strict";
              PrivateTmp = true;
              ProtectKernelTunables = true;
              ProtectKernelModules = true;
              ProtectControlGroups = true;
              MemoryDenyWriteExecute = true;
            };
          };

          environment.etc."fan-ipmi/fan.toml".source = cfg.configPath;
        };
      };
    };
}

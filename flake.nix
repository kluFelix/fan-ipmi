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
        
        formatTomlValue = v:
          if builtins.isBool v then builtins.toString v
          else if builtins.isInt v then builtins.toString v
          else if builtins.isFloat v then
            let s = builtins.toString v; in
            if builtins.match "^-?[0-9]+$" s != null then s + ".0" else s
          else if builtins.isString v then "\"" + v + "\""
          else if builtins.isList v then
            let nonNulls = builtins.filter (x: x != null) v; in
            if nonNulls == [] then "[]"
            else "[" + lib.concatStringsSep ", " (lib.map formatTomlValue nonNulls) + "]"
          else if builtins.isAttrs v then
            let pairs = lib.mapAttrsToList (k: v': "${k} = ${formatTomlValue v'}") v; in
            if pairs == [] then "{}"
            else "{ " + lib.concatStringsSep ", " pairs + " }"
          else if v == null then "null"
          else builtins.toString v;
        
        formatTomlTableArray = name: list:
          let entries = lib.map (entry:
            let entryPairs = lib.mapAttrsToList (k: v': "${k} = ${formatTomlValue v'}") entry;
            in "[[${name}]]\n" + lib.concatStringsSep "\n" entryPairs
          ) list;
          in lib.concatStringsSep "\n\n" entries;
        
        generateTomlContent = cfg: ''
address = "${cfg.address}"
history_sec = ${builtins.toString cfg.historySec}
password = "${cfg.password}"
user = "${cfg.user}"
        '';
        
        generateFanThresholds = thresholds:
          if thresholds == [] then ""
          else "\n" + formatTomlTableArray "fan_thresholds" (lib.map (ft: {
            fan = ft.fan;
            lower = ft.lower;
          } // lib.optionalAttrs (ft.upper != null) {
            upper = ft.upper;
          }) thresholds);
        
        generateCurves = curves:
          if curves == [] then ""
          else "\n" + formatTomlTableArray "curves" (lib.map (curve: {
            name = curve.name;
            sources = curve.sources;
            profile = curve.profile;
          }) curves);
        
        configFile = pkgs.writeText "fan.toml" (
          generateTomlContent cfg +
          generateFanThresholds cfg.fanThresholds +
          generateCurves cfg.curves
        );
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
            description = "History window in seconds";
          };
          
          fanThresholds = lib.mkOption {
            type = lib.types.listOf (lib.types.submodule {
              options = {
                fan = lib.mkOption { 
                  type = lib.types.str;
                  description = "Fan sensor name";
                };
                lower = lib.mkOption { 
                  type = lib.types.listOf lib.types.int;
                  description = "[min_rpm, warning_rpm, critical_rpm]";
                };
                upper = lib.mkOption { 
                  type = lib.types.nullOr (lib.types.listOf lib.types.int);
                  default = null;
                  description = "[min_rpm, warning_rpm, critical_rpm] (optional)";
                };
              };
            });
            default = [];
            description = "Fan sensor thresholds";
          };
          
          curves = lib.mkOption {
            type = lib.types.listOf (lib.types.submodule {
              options = {
                name = lib.mkOption { 
                  type = lib.types.str;
                  description = "Curve name";
                };
                sources = lib.mkOption { 
                  type = lib.types.listOf lib.types.str;
                  description = "Temperature sources (file paths or gpu:N)";
                };
                profile = lib.mkOption {
                  type = lib.types.listOf (lib.types.submodule {
                    options = {
                      temp = lib.mkOption { 
                        type = lib.types.float;
                        description = "Temperature value";
                      };
                      pct = lib.mkOption { 
                        type = lib.types.float;
                        description = "Fan percentage (0.0 to 1.0)";
                      };
                    };
                  });
                  description = "Profile points with temp and pct";
                };
              };
            });
            default = [];
            description = "Temperature curves";
          };
        };

        config = lib.mkIf cfg.enable {
          environment.etc."fan-ipmi/fan.toml".source = configFile;
          
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
        };
      };
    };
}

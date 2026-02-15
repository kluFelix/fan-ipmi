{
  description = "A basic flake for c development";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";

  outputs = { self, nixpkgs, ... }: let
    system = "x86_64-linux";
    pkgs = import nixpkgs {
      inherit system;
      config.allowUnfree = true;
    };
  in {
    devShells."${system}".default = let
      # NVIDIA driver providing libnvidia-ml.so
      nvidiaDriver = pkgs.linuxPackages_latest.nvidia_x11;

    in pkgs.mkShell {
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


{ config, lib, pkgs, ... }:

let
  inherit (lib)
    escapeShellArgs
    literalExpression
    mkEnableOption
    mkIf
    mkOption
    optional
    optionalAttrs
    types
    ;

  cfg = config.services.koe;

  # Re-import nixpkgs with the CUDA settings needed to build llama.cpp. This
  # keeps the global nixpkgs configuration untouched.
  nixpkgsForLlama = import pkgs.path {
    inherit (pkgs.stdenv.hostPlatform) system;
    config =
      pkgs.config
      // { allowUnfree = true; }
      // optionalAttrs (cfg.llamaServer.cudaCapabilities != null) {
        inherit (cfg.llamaServer) cudaCapabilities;
      };
  };

  defaultLlamaPackage = nixpkgsForLlama.llama-cpp.override { cudaSupport = true; };
in
{
  options.services.koe = {
    enable = mkEnableOption "koe voice input for fcitx5";

    package = mkOption {
      type = types.package;
      default = pkgs.callPackage ../package.nix { };
      defaultText = literalExpression "pkgs.callPackage ../package.nix { }";
      description = ''
        The koe package providing the fcitx5 addon and the koe-daemon binary.
      '';
    };

    llamaServer = {
      enable = mkOption {
        type = types.bool;
        default = true;
        description = ''
          Run a local llama-server with Qwen3-ASR as a user service. Disable
          this when the daemon should only use a cloud profile.
        '';
      };

      package = mkOption {
        type = types.package;
        default = defaultLlamaPackage;
        defaultText = literalExpression "llama-cpp built with CUDA support";
        description = ''
          The llama.cpp package used for the local server. The default is
          built from a copy of nixpkgs re-imported with this module's CUDA
          settings, so the global nixpkgs configuration is not changed. It
          needs cudaSupport = true, which the default sets, and a matching
          cudaCapabilities value.
        '';
      };

      cudaCapabilities = mkOption {
        type = types.nullOr (types.listOf types.str);
        default = null;
        example = [ "8.9" ];
        description = ''
          CUDA compute capabilities to build llama.cpp for. Set this to your
          GPU's capability, for example [ "8.9" ] for an RTX 40 series card,
          when using the default llamaServer.package. When null, the nixpkgs
          default is used.
        '';
      };

      model = mkOption {
        type = types.str;
        default = "%h/.local/share/koe/models/Qwen3-ASR-1.7B-Q8_0.gguf";
        description = ''
          Path to the Qwen3-ASR GGUF model. The %h specifier is expanded by
          systemd to the user's home directory. Keep large model files outside
          the Nix store.
        '';
      };

      mmproj = mkOption {
        type = types.str;
        default = "%h/.local/share/koe/models/mmproj-Qwen3-ASR-1.7B-Q8_0.gguf";
        description = ''
          Path to the Qwen3-ASR multimodal projector GGUF file. The %h
          specifier is expanded by systemd to the user's home directory.
        '';
      };

      port = mkOption {
        type = types.port;
        default = 8178;
        description = "TCP port the local llama-server listens on.";
      };

      extraArgs = mkOption {
        type = types.listOf types.str;
        default = [ "-np" "1" "-c" "4096" ];
        description = ''
          Extra arguments appended to the llama-server command line. The
          defaults allow a single client and a 4096 token context to keep VRAM
          use low.
        '';
      };
    };
  };

  config = mkIf cfg.enable {
    i18n.inputMethod.fcitx5.addons = [ cfg.package ];

    environment.systemPackages = [ cfg.package ];

    systemd.user.services.koe-daemon = {
      description = "koe speech-to-text daemon for fcitx5";
      wantedBy = [ "default.target" ];
      partOf = [ "default.target" ];
      after = [ "pipewire.service" ]
        ++ optional cfg.llamaServer.enable "koe-llama-server.service";
      wants = [ "pipewire.service" ]
        ++ optional cfg.llamaServer.enable "koe-llama-server.service";
      serviceConfig = {
        ExecStart = "${cfg.package}/bin/koe-daemon";
        Restart = "on-failure";
        RestartSec = 2;
      };
    };

    systemd.user.services.koe-llama-server = mkIf cfg.llamaServer.enable {
      description = "llama-server running Qwen3-ASR for koe";
      wantedBy = [ "default.target" ];
      partOf = [ "default.target" ];
      serviceConfig = {
        ExecStart = "${cfg.llamaServer.package}/bin/llama-server "
          + escapeShellArgs (
            [
              "-m"
              cfg.llamaServer.model
              "--mmproj"
              cfg.llamaServer.mmproj
              "-ngl"
              "99"
              "--host"
              "127.0.0.1"
              "--port"
              (toString cfg.llamaServer.port)
            ]
            ++ cfg.llamaServer.extraArgs
          );
        Restart = "on-failure";
        RestartSec = 5;
      };
    };
  };
}

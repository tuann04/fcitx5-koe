{
  lib,
  stdenv,
  kdePackages,
  cmake,
  pkg-config,
  fcitx5,
  pipewire,
  curl,
  tomlplusplus,
  nlohmann_json,
}:

stdenv.mkDerivation {
  pname = "fcitx5-koe";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./CMakeLists.txt
      ./LICENSE
      ./common
      ./addon
      ./daemon
      ./packaging
      ./nix/config.example.toml
    ];
  };

  nativeBuildInputs = [
    cmake
    kdePackages.extra-cmake-modules
    pkg-config
  ];

  buildInputs = [
    fcitx5
    pipewire
    curl
    tomlplusplus
    nlohmann_json
  ];

  meta = {
    description = "Hold-to-talk speech-to-text addon for fcitx5";
    platforms = lib.platforms.linux;
  };
}

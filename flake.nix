{
  description = "Linux Bias Lighting";
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        mkBlight =
          {
            useWifi ? false,
            useGpu ? false,
          }:
          pkgs.stdenv.mkDerivation {
            pname = "blight";
            version = "0.1.0";
            src = ./.;

            nativeBuildInputs = with pkgs; [
              pkg-config
              gcc
              makeWrapper
            ];

            buildInputs = with pkgs; [
              gst_all_1.gstreamer
              gst_all_1.gst-plugins-base
              gst_all_1.gst-plugins-good
              gst_all_1.gst-plugins-bad
              gst_all_1.gst-vaapi
              libglvnd
              pipewire
              glib
              libportal
            ];

            buildPhase = ''
              gcc ${if useWifi then "-DWIFI" else "-DSERIAL"} ${if useGpu then "-DUSE_GPU" else ""} -g -O2 \
                -o main src/main.c src/serial.c ${if useWifi then "src/wifi.c" else ""} \
                $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 libportal glib-2.0) \
                -lm
            '';

            installPhase = ''
              mkdir -p $out/bin
              cp main $out/bin/blight
            '';

            postFixup = ''
              wrapProgram $out/bin/blight \
                --prefix GST_PLUGIN_SYSTEM_PATH_1_0 : "$GST_PLUGIN_SYSTEM_PATH_1_0"
            '';
          };
      in
      {
        packages = {
          default = mkBlight { useWifi = false; };
          blight_serial = mkBlight { useWifi = false; };
          blight_wifi = mkBlight { useWifi = true; };
          blight_wifi_gpu = mkBlight {
            useWifi = true;
            useGpu = true;
          };
          blight_serial_gpu = mkBlight {
            useWifi = false;
            useGpu = true;
          };
        };

        devShells.default = pkgs.mkShell {
          nativeBuildInputs = with pkgs; [
            pkg-config
            gcc
            clang-tools
          ];

          buildInputs = with pkgs; [
            gst_all_1.gstreamer
            gst_all_1.gst-plugins-base
            gst_all_1.gst-plugins-good
            gst_all_1.gst-plugins-bad
            gst_all_1.gst-vaapi
            arduino-cli
            python3
            pipewire
            glib
            libportal
            perf
          ];

          shellHook = ''
            export GST_PLUGIN_PATH="${pkgs.gst_all_1.gst-plugins-base}/lib/gstreamer-1.0:${pkgs.gst_all_1.gst-plugins-good}/lib/gstreamer-1.0:${pkgs.gst_all_1.gst-plugins-bad}/lib/gstreamer-1.0:${pkgs.gst_all_1.gst-vaapi}/lib/gstreamer-1.0:${pkgs.pipewire}/lib/gstreamer-1.0"
            pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 libportal glib-2.0 | tr ' ' '\n' > compile_flags.txt
            export CPATH="$(pkg-config --cflags-only-I gstreamer-1.0 gstreamer-app-1.0 libportal glib-2.0 | sed 's/-I//g' | tr ' ' ':')"
            if command -v zsh &> /dev/null; then exec zsh; fi
          '';
        };
      }
    );
}

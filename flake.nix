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
      in
      {
        devShells.default = pkgs.mkShell {
          nativeBuildInputs = with pkgs; [
            pkg-config
            gcc
            clang-tools
          ];

          buildInputs = with pkgs; [
            arduino-cli
            python3
            bear
            pipewire
            glib
            libportal
          ];

          shellHook = ''
            export ARDUINO_DIRECTORIES_DATA="$PWD/esp32/.arduino/data"
            export ARDUINO_DIRECTORIES_DOWNLOADS="$PWD/esp32/.arduino/downloads"
            export ARDUINO_DIRECTORIES_USER="$PWD/esp32/.arduino/user"
            export ARDUINO_CONFIG_FILE="$PWD/esp32/.arduino/arduino-cli.yaml"

            echo "Checking ESP32 Arduino Core..."

            if [ ! -f "$ARDUINO_CONFIG_FILE" ]; then
              echo "Initializing local arduino-cli configuration..."
              arduino-cli config init --dest-dir "$PWD/esp32/.arduino" > /dev/null
              arduino-cli config set board_manager.additional_urls "https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json"
            fi

            if ! arduino-cli core list | grep -q "esp32:esp32"; then
              echo "Downloading and installing ESP32 core (this may take a minute)..."
              arduino-cli core update-index
              arduino-cli core install esp32:esp32
              echo "ESP32 core installed successfully!"
            else
              echo "ESP32 core is already installed."
            fi




          '';
        };
      }
    );
}

{
  nixpkgs,
  system,
  tetherModule,
}:
let
  lib = nixpkgs.lib;
  pkgs = import nixpkgs { inherit system; };
  evaluated = lib.nixosSystem {
    inherit system;
    modules = [
      tetherModule
      {
        programs.tether = {
          enable = true;
          wifi.enable = true;
          wifi.openFirewall = true;
          bluetooth.enable = true;
          bluetooth.adapters = [
            "hci0"
            "hci1"
          ];
        };
      }
    ];
  };
  config = evaluated.config;
  tether = config.programs.tether.package;
  thunderbird = config.programs.thunderbird.package;
  baseOnlyConfig =
    (lib.nixosSystem {
      inherit system;
      modules = [
        tetherModule
        { programs.tether.enable = true; }
      ];
    }).config;
  masterDisabledConfig =
    (lib.nixosSystem {
      inherit system;
      modules = [
        tetherModule
        {
          programs.tether = {
            enable = false;
            wifi.enable = true;
            wifi.openFirewall = true;
            bluetooth.enable = true;
            bluetooth.adapters = [
              "hci0"
              "hci1"
            ];
          };
        }
      ];
    }).config;
  wifiOnlyConfig =
    (lib.nixosSystem {
      inherit system;
      modules = [
        tetherModule
        {
          programs.tether = {
            enable = true;
            wifi.enable = true;
          };
        }
      ];
    }).config;
  hasTetherBluetoothService =
    moduleConfig:
    lib.any (lib.hasPrefix "tether-btclass@") (builtins.attrNames moduleConfig.systemd.services);
in
assert lib.elem tether config.environment.systemPackages;
assert lib.elem tether config.programs.firefox.nativeMessagingHosts.packages;
assert !config.programs.thunderbird.enable;
assert config.environment.etc ? "chromium/native-messaging-hosts/com.tether.extension.json";
assert config.environment.etc ? "opt/chrome/native-messaging-hosts/com.tether.extension.json";
assert config.services.avahi.enable;
assert config.services.avahi.openFirewall;
assert lib.elem 5134 config.networking.firewall.allowedTCPPorts;
assert !baseOnlyConfig.services.avahi.enable;
assert !lib.elem 5134 baseOnlyConfig.networking.firewall.allowedTCPPorts;
assert !baseOnlyConfig.hardware.bluetooth.enable;
assert !hasTetherBluetoothService baseOnlyConfig;
assert !masterDisabledConfig.services.avahi.enable;
assert !lib.elem 5134 masterDisabledConfig.networking.firewall.allowedTCPPorts;
assert !masterDisabledConfig.hardware.bluetooth.enable;
assert !hasTetherBluetoothService masterDisabledConfig;
assert wifiOnlyConfig.services.avahi.enable;
assert !wifiOnlyConfig.services.avahi.openFirewall;
assert !lib.elem 5134 wifiOnlyConfig.networking.firewall.allowedTCPPorts;
assert config.hardware.bluetooth.enable;
assert config.hardware.bluetooth.settings.General.Experimental;
assert config.systemd.services ? "tether-btclass@hci0";
assert config.systemd.services ? "tether-btclass@hci1";
pkgs.runCommand "tether-module-test" { } ''
  export HOME="$TMPDIR/home"
  export MOZ_HOME="$TMPDIR/mozilla"

  ${thunderbird}/bin/thunderbird --version

  manifest="$MOZ_HOME/native-messaging-hosts/com.tether.extension.json"
  test -L "$manifest"
  test "$(readlink "$manifest")" = \
    "${tether}/lib/mozilla/native-messaging-hosts/com.tether.extension.json"

  touch "$out"
''

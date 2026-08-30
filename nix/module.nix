{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.programs.tether;
  bluetoothService = adapter: {
    name = "tether-btclass@${adapter}";
    value = {
      description = "Set Bluetooth Class of Device for Tether on ${adapter}";
      after = [ "bluetooth.service" ];
      partOf = [ "bluetooth.service" ];
      wantedBy = [ "bluetooth.service" ];
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
        ExecStart = pkgs.writeShellScript "tether-btclass-${adapter}" ''
          for attempt in 1 2 3 4 5 6 7 8 9 10; do
            ${pkgs.bluez}/bin/btmgmt --index ${adapter} class 4 8 >/dev/null 2>&1
            if ${pkgs.bluez}/bin/btmgmt --index ${adapter} info 2>/dev/null \
              | ${pkgs.gnugrep}/bin/grep -q "class 0x..0408"; then
              exit 0
            fi
            ${pkgs.coreutils}/bin/sleep 1
          done
          exit 1
        '';
      };
    };
  };
in
{
  options.programs.tether = {
    enable = lib.mkEnableOption "Tether, an iPhone integration bridge for Linux";
    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./package.nix { };
      defaultText = lib.literalExpression "pkgs.callPackage ./nix/package.nix { }";
      description = "The Tether package to install and integrate.";
    };
    wifi.enable = lib.mkEnableOption "Tether Wi-Fi discovery through Avahi";
    wifi.openFirewall = lib.mkEnableOption "the mDNS and TCP firewall ports required by Tether";
    bluetooth.enable = lib.mkEnableOption "Tether Bluetooth integration";
    bluetooth.adapters = lib.mkOption {
      type = lib.types.listOf (lib.types.strMatching "hci[0-9]+");
      default = [ "hci0" ];
      example = [
        "hci0"
        "hci1"
      ];
      description = "Bluetooth HCI adapters whose class Tether should configure.";
    };
  };

  config = lib.mkMerge [
    (lib.mkIf cfg.enable {
      environment.systemPackages = [ cfg.package ];
      programs.firefox.nativeMessagingHosts.packages = [ cfg.package ];
      programs.thunderbird.package = lib.mkDefault (
        pkgs.thunderbird.override {
          nativeMessagingHosts = [ cfg.package ];
        }
      );
      environment.etc = {
        "chromium/native-messaging-hosts/com.tether.extension.json".source =
          "${cfg.package}/etc/chromium/native-messaging-hosts/com.tether.extension.json";
        "opt/chrome/native-messaging-hosts/com.tether.extension.json".source =
          "${cfg.package}/etc/opt/chrome/native-messaging-hosts/com.tether.extension.json";
      };
    })

    (lib.mkIf (cfg.enable && cfg.wifi.enable) {
      services.avahi.enable = true;
      services.avahi.openFirewall = lib.mkDefault false;
    })

    (lib.mkIf (cfg.enable && cfg.wifi.openFirewall) {
      services.avahi.openFirewall = true;
      networking.firewall.allowedTCPPorts = [ 5134 ];
    })

    (lib.mkIf (cfg.enable && cfg.bluetooth.enable) {
      hardware.bluetooth.enable = true;
      hardware.bluetooth.settings.General.Experimental = true;
      systemd.services = builtins.listToAttrs (map bluetoothService cfg.bluetooth.adapters);
    })
  ];
}

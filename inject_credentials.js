const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");

function keyFromString(str) {
  return crypto.createHash("md5").update(str).digest("hex");
}

// ── Validate required env vars ────────────────────────────────────────────────
const required = ["PMK_INPUT", "WIFI_SSID", "WIFI_PASSWORD"];
for (const key of required) {
  if (!process.env[key]) {
    console.error(`Error: ${key} is not set in .env`);
    process.exit(1);
  }
}

const pmk          = keyFromString(process.env.PMK_INPUT);
const wifiSsid     = process.env.WIFI_SSID;
const wifiPassword = process.env.WIFI_PASSWORD;
const mqttBroker   = process.env.MQTT_BROKER_IP ?? "";
const mqttPort     = process.env.MQTT_PORT      ?? "1883";
const mqttUser     = process.env.MQTT_USER      ?? "";
const mqttPassword = process.env.MQTT_PASSWORD  ?? "";

console.log("PMK:", pmk);

// ── platformio.ini updater ────────────────────────────────────────────────────
function quoted(val) {
  const escaped = val.replaceAll(" ", String.raw`\ `);
  return String.raw`\"${escaped}\"`;
}

const CREDENTIAL_FLAGS = {
  "-DCONFIG_ESPNOW_PMK":     quoted(pmk),
  "-DCONFIG_WIFI_SSID":      quoted(wifiSsid),
  "-DCONFIG_WIFI_PASSWORD":  quoted(wifiPassword),
  "-DCONFIG_MQTT_BROKER_IP": quoted(mqttBroker),
  "-DCONFIG_MQTT_PORT":      mqttPort,
  "-DCONFIG_MQTT_USER":      quoted(mqttUser),
  "-DCONFIG_MQTT_PASSWORD":  quoted(mqttPassword),
};

function updatePlatformIni(iniPath) {
  let content = fs.readFileSync(iniPath, "utf8");

  for (const [flag, value] of Object.entries(CREDENTIAL_FLAGS)) {
    const escapedFlag = flag.replaceAll(/[.*+?^${}()|[\]\\]/g, String.raw`\$&`);

    // Only update flags that already exist in the file — never add new ones
    const existingRe = new RegExp(String.raw`([ \t]*${escapedFlag}=).*`, "g");
    if (existingRe.test(content)) {
      content = content.replaceAll(
        new RegExp(String.raw`([ \t]*${escapedFlag}=).*`, "g"),
        `$1${value}`
      );
    }
  }

  fs.writeFileSync(iniPath, content, "utf8");
  console.log("Updated:", iniPath);
}

const root = path.resolve(__dirname);
updatePlatformIni(path.join(root, "spot_firmware",        "platformio.ini"));
updatePlatformIni(path.join(root, "master_firmware",      "platformio.ini"));
updatePlatformIni(path.join(root, "wifi_bridge_firmware", "platformio.ini"));

//
// WiFi Channel Scanner — TTGO LoRa32 V1
//
// Scans all 13 WiFi channels and displays a bar chart of AP count + RSSI
// on the SSD1306 128×64 OLED.  Also streams a summary table to serial.
//
// OLED wiring (TTGO LoRa32 V1): SDA=4, SCL=15, RST=16
//

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "soc/rtc_cntl_reg.h"   // RTC_CNTL_BROWN_OUT_REG

// ─── OLED ────────────────────────────────────────────────────────────────────
#define OLED_SDA   4
#define OLED_SCL  15
#define OLED_RST  16
#define SCREEN_W  128
#define SCREEN_H   64
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, OLED_RST);

// ─── Scanner config ───────────────────────────────────────────────────────────
#define NUM_CHANNELS   13
#define SCAN_INTERVAL  5000   // ms between full scans

// Per-channel aggregates filled during each scan pass
static int    ch_count[NUM_CHANNELS + 1];   // index 1..13
static int    ch_rssi_sum[NUM_CHANNELS + 1];

// ─── Helpers ─────────────────────────────────────────────────────────────────
static int maxCount() {
    int m = 1;
    for (int c = 1; c <= NUM_CHANNELS; c++)
        if (ch_count[c] > m) m = ch_count[c];
    return m;
}

static int avgRssi(int ch) {
    if (ch_count[ch] == 0) return 0;
    return ch_rssi_sum[ch] / ch_count[ch];
}

// ─── Display ─────────────────────────────────────────────────────────────────
// Layout (128×64):
//   Row 0       : title "WiFi Channels" (small font)
//   Rows 8–55   : bar chart (13 bars)
//   Row 56–63   : bottom legend  "1  2  3  4 … 13"
//
// Each bar column is 128/13 ≈ 9px wide, 1px gap → bar width = 8px
// Bar height proportional to AP count, coloured solid.
// If a channel has APs, the avg RSSI is printed above the bar (tiny font).

static void drawChart(uint32_t scanDuration) {
    display.clearDisplay();

    // Title
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("WiFi Ch  ");
    display.print(scanDuration / 1000);
    display.print("s ago");

    const int BAR_AREA_TOP  = 9;    // y where bars start
    const int BAR_AREA_BOT  = 54;   // y bottom of bar area (inclusive)
    const int BAR_AREA_H    = BAR_AREA_BOT - BAR_AREA_TOP;  // 45px
    const int LEGEND_Y      = 56;
    const int COL_W         = 9;    // pixels per channel slot (9×13=117, leaves 11px on right)

    int maxC = maxCount();

    for (int ch = 1; ch <= NUM_CHANNELS; ch++) {
        int x = (ch - 1) * COL_W + 1;  // 1px left margin

        // Bar height
        int barH = 0;
        if (ch_count[ch] > 0)
            barH = max(2, (ch_count[ch] * BAR_AREA_H) / maxC);

        int barY = BAR_AREA_BOT - barH;
        display.fillRect(x, barY, 7, barH, SSD1306_WHITE);

        // AP count label above bar
        if (ch_count[ch] > 0) {
            int labelY = barY - 7;
            if (labelY < BAR_AREA_TOP) labelY = BAR_AREA_TOP;
            display.setTextSize(1);
            display.setCursor(x, labelY);
            display.print(ch_count[ch]);
        }

        // Channel number legend (squeeze: 1-digit straight, "10"–"13" tiny)
        display.setTextSize(1);
        int numX = x;
        if (ch >= 10) numX = x - 1;  // shift slightly for 2 digits
        display.setCursor(numX, LEGEND_Y);
        display.print(ch);
    }

    display.display();
}

// ─── Serial summary ──────────────────────────────────────────────────────────
static void printSerial(int totalAPs) {
    Serial.println("\n── WiFi Channel Scan ────────────────────────────────");
    Serial.printf("  Total APs found: %d\n", totalAPs);
    Serial.println("  Ch  APs  AvgRSSI  Bar");
    for (int ch = 1; ch <= NUM_CHANNELS; ch++) {
        Serial.printf("  %2d  %3d", ch, ch_count[ch]);
        if (ch_count[ch] > 0)
            Serial.printf("   %4d dBm  ", avgRssi(ch));
        else
            Serial.print("           ");

        // ASCII bar
        int bar = ch_count[ch] * 2;
        for (int i = 0; i < bar; i++) Serial.print('|');
        Serial.println();
    }
    Serial.println("─────────────────────────────────────────────────────");
}

// ─── Scan ────────────────────────────────────────────────────────────────────
static uint32_t lastScanTime = 0;
static uint32_t lastScanDuration = 0;
static bool scanning = false;

static void startScan() {
    memset(ch_count,    0, sizeof(ch_count));
    memset(ch_rssi_sum, 0, sizeof(ch_rssi_sum));

    // Show "Scanning…" on OLED while busy
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 24);
    display.print("Scanning...");
    display.display();

    // async=false, show_hidden=true, passive=false, max_ms_per_chan=150
    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true,
                              /*passive=*/false, /*max_ms_per_chan=*/150);
    if (n < 0) n = 0;

    for (int i = 0; i < n; i++) {
        int ch = WiFi.channel(i);
        if (ch >= 1 && ch <= NUM_CHANNELS) {
            ch_count[ch]++;
            ch_rssi_sum[ch] += WiFi.RSSI(i);
        }
    }

    WiFi.scanDelete();
    printSerial(n);
}

// ─── Setup / Loop ─────────────────────────────────────────────────────────────
void setup() {
    // Brownout can hit during WiFi scan TX bursts on USB power
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(200);
    Serial.println("WiFi Channel Scanner — TTGO LoRa32");

    // OLED
    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("SSD1306 init failed");
        while (true) delay(1000);
    }
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(8, 24);
    display.print("WiFi Channel Scanner");
    display.display();
    delay(1000);

    // WiFi in scan-only mode (no connection needed)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    // First scan immediately
    uint32_t t0 = millis();
    startScan();
    lastScanDuration = millis() - t0;
    lastScanTime = millis();
    drawChart(0);
}

void loop() {
    uint32_t now = millis();
    if (now - lastScanTime >= SCAN_INTERVAL) {
        uint32_t t0 = millis();
        startScan();
        lastScanDuration = millis() - t0;
        lastScanTime = millis();
    }
    drawChart(millis() - lastScanTime);
    delay(500);
}

#include "sdcard.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// The Waveshare schematic's SD-CARD block labels the socket pins CMD/CLK/D0/D3 (generic
// microSD naming) but wires only the four SPI-mode lines to the ESP32: CMD->MOSI,
// CLK->SCK, D0->MISO, D3->CS (D1/D2/card-detect are not connected). So this is a plain
// SPI card, not SD_MMC, even though the silkscreen uses SD_MMC-style pin names.
static constexpr int SD_PIN_MOSI = 1;
static constexpr int SD_PIN_SCK  = 2;
static constexpr int SD_PIN_MISO = 3;
static constexpr int SD_PIN_CS   = 41;

static SPIClass  s_sdSpi(HSPI);
static bool      s_mounted   = false;
static uint64_t  s_sizeBytes = 0;

// The Arduino SD library defaults to a conservative 4 MHz SPI clock. That's the main
// cause of choppy Spy Cam playback (each frame is a fresh open/read/close of a JPEG
// file) — 20 MHz is a solid 5x speedup and still well inside what any real SD card
// handles reliably in SPI mode. Push higher only after confirming no read errors.
static constexpr uint32_t SD_SPI_HZ = 20000000;

// Fall back rather than refuse.
//
// 20 MHz is what a modern card manages and what Spy Cam needs, so it is tried first and
// nothing that works today changes. But it is five times the Arduino default, and an older
// card cannot always hold it: an SDSC card, which is anything 2 GB or under, will power up,
// click, and then fail to mount with no hint as to why. Overcore lost a day to exactly that
// on 2026-09-29 and had every reason to think the board was broken.
//
// So the speed is negotiated instead of assumed. A card that manages 20 gets 20; one that
// does not gets a slower bus and works, which is far better than being told there is no card.
static constexpr uint32_t SD_SPI_TRY[] = { SD_SPI_HZ, 10000000, 4000000 };

bool sdcard::begin() {
    s_sdSpi.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
    bool up = false;
    uint32_t hz = 0;
    for (size_t i = 0; i < sizeof(SD_SPI_TRY) / sizeof(SD_SPI_TRY[0]) && !up; ++i) {
        if (i) { SD.end(); delay(20); }          // let the previous attempt let go of the bus
        if (SD.begin(SD_PIN_CS, s_sdSpi, SD_SPI_TRY[i]) && SD.cardType() != CARD_NONE) {
            up = true;
            hz = SD_SPI_TRY[i];
        }
    }
    if (!up) {
        Serial.println("[sd] no card detected (tried 20, 10 and 4 MHz)");
        s_mounted = false;
        return false;
    }
    if (hz != SD_SPI_HZ)
        Serial.printf("[sd] card would not run at %u MHz, using %u MHz instead\n",
                      (unsigned)(SD_SPI_HZ / 1000000), (unsigned)(hz / 1000000));
    s_sizeBytes = SD.cardSize();
    const uint8_t type = SD.cardType();
    const char *typeName = type == CARD_MMC ? "MMC" : type == CARD_SD ? "SDSC" :
                            type == CARD_SDHC ? "SDHC" : "UNKNOWN";
    Serial.printf("[sd] %s card mounted, %.2f GB\n", typeName,
                  s_sizeBytes / (1024.0 * 1024.0 * 1024.0));
    s_mounted = true;
    return true;
}

bool sdcard::mounted() { return s_mounted; }
uint64_t sdcard::sizeBytes() { return s_sizeBytes; }

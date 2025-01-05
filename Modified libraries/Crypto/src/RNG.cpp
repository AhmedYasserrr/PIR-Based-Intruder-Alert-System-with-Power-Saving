/*
 * Copyright (C) 2015 Southern Storm Software, Pty Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "RNG.h"
#include "NoiseSource.h"
#include "ChaCha.h"
#include "Crypto.h"
#include <Arduino.h>
#include "utility/ProgMemUtil.h"
#if defined (__arm__) && defined (__SAM3X8E__)
// The Arduino Due does not have any EEPROM natively on the main chip.
// However, it does have a TRNG and flash memory.
#define RNG_DUE_TRNG 1
#elif defined(__AVR__)
#define RNG_EEPROM 1        // Use EEPROM to save the seed.
#define RNG_WATCHDOG 1      // Harvest entropy from watchdog jitter.
#include <avr/eeprom.h>
#include <avr/wdt.h>
#include <avr/io.h>
#define RNG_EEPROM_ADDRESS (E2END + 1 - RNGClass::SEED_SIZE)
#elif defined(ESP8266)
// ESP8266 does not have EEPROM but it does have SPI flash memory.
// It also has a TRNG register for generating "true" random numbers.
// For now we use the TRNG but don't save the seed in flash memory.
#define RNG_WORD_TRNG 1
#define RNG_WORD_TRNG_GET() (ESP8266_DREG(0x20E44))
#elif defined(ESP32)
// ESP32 has a word-based TRNG and an API for Non-Volatile Storage (NVS).
#define RNG_WORD_TRNG 1
#define RNG_WORD_TRNG_GET() (esp_random())
#define RNG_ESP_NVS 1
#include <nvs.h>
#endif
#include <string.h>

// Throw a warning if there is no built-in hardware random number source.
// If this happens, then you need to do one of two things:
//    1. Edit RNG.cpp to add your platform's hardware TRNG.
//    2. Provide a proper noise source like TransistorNoiseSource
//       in your sketch and then comment out the #warning line below.
#if !defined(RNG_DUE_TRNG) && \
    !defined(RNG_WATCHDOG) && \
    !defined(RNG_WORD_TRNG)
#warning "no hardware random number source detected for this platform"
#endif

/**
 * \class RNGClass RNG.h <RNG.h>
 * \brief Pseudo random number generator suitable for cryptography.
 *
 * Random number generators must be seeded properly before they can
 * be used or an adversary may be able to predict the random output.
 * Seed data may be:
 *
 * \li Device-specific, for example serial numbers or MAC addresses.
 * \li Application-specific, unique to the application.  The tag that is
 * passed to begin() is an example of an application-specific value.
 * \li Noise-based, generated by a hardware random number generator
 * that provides unpredictable values from a noise source.
 *
 * The following example demonstrates how to initialise the random
 * number generator:
 *
 * \code
 * #include <SPI.h>
 * #include <Ethernet.h>
 * #include <Crypto.h>
 * #include <RNG.h>
 * #include <TransistorNoiseSource.h>
 *
 * // Noise source to seed the random number generator.
 * TransistorNoiseSource noise(A1);
 *
 * // MAC address for Ethernet communication.
 * byte mac_address[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
 *
 * void setup() {
 *     // Initialize the Ethernet shield.
 *     Ethernet.begin(mac_address);
 *
 *     // Initialize the random number generator with the application tag
 *     // "MyApp 1.0" and load the previous seed from EEPROM.
 *     RNG.begin("MyApp 1.0");
 *
 *     // Stir in the Ethernet MAC address.
 *     RNG.stir(mac_address, sizeof(mac_address));
 *
 *     // Add the noise source to the list of sources known to RNG.
 *     RNG.addNoiseSource(noise);
 *
 *     // ...
 * }
 * \endcode
 *
 * The application should regularly call loop() to stir in new data
 * from the registered noise sources and to periodically save the seed:
 *
 * \code
 * void loop() {
 *     // ...
 *
 *     // Perform regular housekeeping on the random number generator.
 *     RNG.loop();
 *
 *     // ...
 * }
 * \endcode
 *
 * The loop() function will automatically save the random number seed on a
 * regular basis to the last SEED_SIZE bytes of EEPROM memory.  By default
 * the seed is saved every hour but this can be changed using setAutoSaveTime().
 *
 * Keep in mind that saving too often may cause the EEPROM to wear out quicker.
 * It is wise to limit saving to once an hour or once a day depending
 * upon how long you intend to field the device before replacing it.
 * For example, an EEPROM rated for 100k erase/write cycles will last about
 * 69 days saving once a minute or 11 years saving once an hour.
 *
 * The application can still elect to call save() at any time if wants.
 * For example, if the application can detect power loss or shutdown
 * conditions programmatically, then it may make sense to force a save()
 * of the seed upon shutdown.
 *
 * The Arduino Due does not have EEPROM so RNG saves the seed into
 * the last page of system flash memory instead.  The RNG class will also
 * mix in data from the CPU's built-in True Random Number Generator (TRNG).
 * Assuming that the CPU's TRNG is trustworthy, this should be sufficient
 * to properly seed the random number generator.  It is recommended to
 * also mix in data from other noise sources just in case the CPU's TRNG
 * is not trustworthy.
 *
 * \sa NoiseSource
 */

/**
 * \brief Global random number generator instance.
 *
 * \sa RNGClass
 */
RNGClass RNG;

/**
 * \var RNGClass::SEED_SIZE
 * \brief Size of a saved random number seed in EEPROM space.
 *
 * The seed is saved into the last SEED_SIZE bytes of EEPROM memory.
 * The address is dependent upon the size of EEPROM fitted in the device.
 */

// Number of ChaCha hash rounds to use for random number generation.
#define RNG_ROUNDS          20

// Force a rekey after this many blocks of random data.
#define RNG_REKEY_BLOCKS    16

// Maximum entropy credit that can be contained in the pool.
#define RNG_MAX_CREDITS     384u

/** @cond */

// Imported from Crypto.cpp.
extern uint8_t crypto_crc8(uint8_t tag, const void *data, unsigned size);

// Tag for 256-bit ChaCha20 keys.  This will always appear in the
// first 16 bytes of the block.  The remaining 48 bytes are the seed.
static const char tagRNG[16] PROGMEM = {
    'e', 'x', 'p', 'a', 'n', 'd', ' ', '3',
    '2', '-', 'b', 'y', 't', 'e', ' ', 'k'
};

// Initialization seed.  This is the ChaCha20 output of hashing
// "expand 32-byte k" followed by 48 bytes set to the numbers 1 to 48.
// The ChaCha20 output block is then truncated to the first 48 bytes.
//
// This value is intended to start the RNG in a semi-chaotic state if
// we don't have a previously saved seed in EEPROM.
static const uint8_t initRNG[48] PROGMEM = {
    0xB0, 0x2A, 0xAE, 0x7D, 0xEE, 0xCB, 0xBB, 0xB1,
    0xFC, 0x03, 0x6F, 0xDD, 0xDC, 0x7D, 0x76, 0x67,
    0x0C, 0xE8, 0x1F, 0x0D, 0xA3, 0xA0, 0xAA, 0x1E,
    0xB0, 0xBD, 0x72, 0x6B, 0x2B, 0x4C, 0x8A, 0x7E,
    0x34, 0xFC, 0x37, 0x60, 0xF4, 0x1E, 0x22, 0xA0,
    0x0B, 0xFB, 0x18, 0x84, 0x60, 0xA5, 0x77, 0x72
};

#if defined(RNG_WATCHDOG)

// Use jitter between the watchdog timer and the main CPU clock to
// harvest some entropy on AVR-based systems.  This technique comes from:
//
// https://sites.google.com/site/astudyofentropy/project-definition/timer-jitter-entropy-sources/entropy-library
//
// The watchdog generates entropy very slowly - it can take around 32 seconds
// to generate 256 bits of entropy credit.  This is a "better than nothing"
// entropy source but a real noise source is definitely recommended.

// Helper macros for specific 32-bit shift counts.
#define leftShift3(value)   ((value) << 3)
#define leftShift10(value)  ((value) << 10)
#define leftShift15(value)  ((value) << 15)
#define rightShift6(value)  ((value) >> 6)
#define rightShift11(value) ((value) >> 11)

static uint32_t volatile hash = 0;
static uint8_t volatile outBits = 0;

// Watchdog interrupt handler.  This fires off every 16ms.  We collect
// 32 bits and then pass them off onto RNGClass::loop().
//ISR(WDT_vect)
//{
//    // Read the low byte of Timer 1.  We assume that the timer was
//    // initialized by the Arduino startup code for PWM use or that the
//    // application is free-running Timer 1 for its own purposes.
//    // Timer 0 is used on systems that don't have a Timer 1.
//#if defined(TCNT1L)
//    unsigned char value = TCNT1L;
//#elif defined(TCNT0L)
//    unsigned char value = TCNT0L;
//#else
//    unsigned char value = TCNT0;
//#endif
//    // Use Jenkin's one-at-a-time hash function to scatter the entropy a bit.
//    // https://en.wikipedia.org/wiki/Jenkins_hash_function
//    hash += value;
//    hash += leftShift10(hash);
//    hash ^= rightShift6(hash);
//    ++outBits;
//}
//ISR(WDT_vect)
//{
//    // Read the low byte of Timer 1.  We assume that the timer was
//    // initialized by the Arduino startup code for PWM use or that the
//    // application is free-running Timer 1 for its own purposes.
//    // Timer 0 is used on systems that don't have a Timer 1.
//#if defined(TCNT1L)
//    unsigned char value = TCNT1L;
//#elif defined(TCNT0L)
//    unsigned char value = TCNT0L;
//#else
//    unsigned char value = TCNT0;
//#endif
//    // Use Jenkin's one-at-a-time hash function to scatter the entropy a bit.
//    // https://en.wikipedia.org/wiki/Jenkins_hash_function
//    hash += value;
//    hash += leftShift10(hash);
//    hash ^= rightShift6(hash);
//    ++outBits;
//}

#endif // RNG_WATCHDOG

/** @endcond */

/**
 * \brief Constructs a new random number generator instance.
 *
 * This constructor must be followed by a call to begin() to
 * properly initialize the random number generator.
 *
 * \sa begin()
 */
RNGClass::RNGClass()
    : credits(0)
    , firstSave(1)
    , initialized(0)
    , trngPending(0)
    , timer(0)
    , timeout(3600000UL)    // 1 hour in milliseconds
    , count(0)
    , trngPosn(0)
{
}

/**
 * \brief Destroys this random number generator instance.
 */
RNGClass::~RNGClass()
{
#if defined(RNG_DUE_TRNG)
    // Disable the TRNG in the Arduino Due.
    REG_TRNG_CR = TRNG_CR_KEY(0x524E47);
#endif
#if defined(RNG_WATCHDOG)
    // Disable interrupts and reset the watchdog.
    cli();
    wdt_reset();

    // Clear the "reset due to watchdog" flag.
    MCUSR &= ~(1 << WDRF);

    // Disable the watchdog.
    _WD_CONTROL_REG |= (1 << _WD_CHANGE_BIT) | (1 << WDE);
    _WD_CONTROL_REG = 0;

    // Re-enable interrupts.  The watchdog should be stopped.
    sei();
#endif
    clean(block);
    clean(stream);
}

#if defined(RNG_DUE_TRNG)

// Find the flash memory of interest.  Allow for the possibility
// of other SAM-based Arduino variants in the future.
#if defined(IFLASH1_ADDR)
#define RNG_FLASH_ADDR      IFLASH1_ADDR
#define RNG_FLASH_SIZE      IFLASH1_SIZE
#define RNG_FLASH_PAGE_SIZE IFLASH1_PAGE_SIZE
#define RNG_EFC             EFC1
#elif defined(IFLASH0_ADDR)
#define RNG_FLASH_ADDR      IFLASH0_ADDR
#define RNG_FLASH_SIZE      IFLASH0_SIZE
#define RNG_FLASH_PAGE_SIZE IFLASH0_PAGE_SIZE
#define RNG_EFC             EFC0
#else
#define RNG_FLASH_ADDR      IFLASH_ADDR
#define RNG_FLASH_SIZE      IFLASH_SIZE
#define RNG_FLASH_PAGE_SIZE IFLASH_PAGE_SIZE
#define RNG_EFC             EFC
#endif

// Address of the flash page to use for saving the seed on the Due.
// All SAM variants have a page size of 256 bytes or greater so there is
// plenty of room for the 48 byte seed in the last page of flash memory.
#define RNG_SEED_ADDR (RNG_FLASH_ADDR + RNG_FLASH_SIZE - RNG_FLASH_PAGE_SIZE)
#define RNG_SEED_PAGE ((RNG_FLASH_SIZE / RNG_FLASH_PAGE_SIZE) - 1)

// Stir in the unique identifier for the Arduino Due's CPU.
// This function must be in RAM because programs running out of
// flash memory are not allowed to access the unique identifier.
// Info from: http://forum.arduino.cc/index.php?topic=289190.0
__attribute__((section(".ramfunc")))
static void stirUniqueIdentifier(void)
{
    uint32_t id[4];

    // Start Read Unique Identifier.
    RNG_EFC->EEFC_FCR = (0x5A << 24) | EFC_FCMD_STUI;
    while ((RNG_EFC->EEFC_FSR & EEFC_FSR_FRDY) != 0)
        ;   // do nothing until FRDY falls.

    // Read the identifier.
    id[0] = *((const uint32_t *)RNG_FLASH_ADDR);
    id[1] = *((const uint32_t *)(RNG_FLASH_ADDR + 4));
    id[2] = *((const uint32_t *)(RNG_FLASH_ADDR + 8));
    id[3] = *((const uint32_t *)(RNG_FLASH_ADDR + 12));

    // Stop Read Unique Identifier.
    RNG_EFC->EEFC_FCR = (0x5A << 24) | EFC_FCMD_SPUI;
    while ((RNG_EFC->EEFC_FSR & EEFC_FSR_FRDY) == 0)
        ;   // do nothing until FRDY rises.

    // Stir the unique identifier into the entropy pool.
    RNG.stir((uint8_t *)id, sizeof(id));
}

// Erases the flash page containing the seed and then writes the new seed.
// It is assumed the seed has already been loaded into the latch registers.
__attribute__((section(".ramfunc")))
static void eraseAndWriteSeed()
{
    // Execute the "Erase and Write Page" command.
    RNG_EFC->EEFC_FCR = (0x5A << 24) | (RNG_SEED_PAGE << 8) | EFC_FCMD_EWP;

    // Wait for the FRDY bit to be raised.
    while ((RNG_EFC->EEFC_FSR & EEFC_FSR_FRDY) == 0)
        ;   // do nothing until FRDY rises.
}

#endif

/**
 * \brief Initializes the random number generator.
 *
 * \param tag A string that is stirred into the random pool at startup;
 * usually this should be a value that is unique to the application and
 * version such as "MyApp 1.0" so that different applications do not
 * generate the same sequence of values upon first boot.
 *
 * This function should be followed by calls to addNoiseSource() to
 * register the application's noise sources.
 *
 * \sa addNoiseSource(), stir(), save()
 */
void RNGClass::begin(const char *tag)
{
    // Bail out if we have already done this.
    if (initialized)
        return;

    // Initialize the ChaCha20 input block from the saved seed.
    memcpy_P(block, tagRNG, sizeof(tagRNG));
    memcpy_P(block + 4, initRNG, sizeof(initRNG));
#if defined(RNG_EEPROM)
    int address = RNG_EEPROM_ADDRESS;
    eeprom_read_block(stream, (const void *)address, SEED_SIZE);
    if (crypto_crc8('S', stream, SEED_SIZE - 1) ==
            ((const uint8_t *)stream)[SEED_SIZE - 1]) {
        // We have a saved seed: XOR it with the initialization block.
        // Note: the CRC-8 value is included.  No point throwing it away.
        for (int posn = 0; posn < 12; ++posn)
            block[posn + 4] ^= stream[posn];
    }
#elif defined(RNG_DUE_TRNG)
    // Do we have a seed saved in the last page of flash memory on the Due?
    if (crypto_crc8('S', ((const uint32_t *)RNG_SEED_ADDR) + 1, SEED_SIZE)
            == ((const uint32_t *)RNG_SEED_ADDR)[0]) {
        // XOR the saved seed with the initialization block.
        for (int posn = 0; posn < 12; ++posn)
            block[posn + 4] ^= ((const uint32_t *)RNG_SEED_ADDR)[posn + 1];
    }

    // If the device has just been reprogrammed, there will be no saved seed.
    // XOR the initialization block with some output from the CPU's TRNG
    // to permute the state in a first boot situation after reprogramming.
    pmc_enable_periph_clk(ID_TRNG);
    REG_TRNG_CR = TRNG_CR_KEY(0x524E47) | TRNG_CR_ENABLE;
    REG_TRNG_IDR = TRNG_IDR_DATRDY; // Disable interrupts - we will poll.
    mixTRNG();
#endif
#if defined(RNG_ESP_NVS)
    // Do we have a seed saved in ESP non-volatile storage (NVS)?
    nvs_handle handle = 0;
    if (nvs_open("rng", NVS_READONLY, &handle) == 0) {
        size_t len = 0;
        if (nvs_get_blob(handle, "seed", NULL, &len) == 0 && len == SEED_SIZE) {
            uint32_t seed[12];
            if (nvs_get_blob(handle, "seed", seed, &len) == 0) {
                for (int posn = 0; posn < 12; ++posn)
                    block[posn + 4] ^= seed[posn];
            }
            clean(seed);
        }
        nvs_close(handle);
    }
#endif
#if defined(RNG_WORD_TRNG)
    // Mix in some output from a word-based TRNG to initialize the state.
    mixTRNG();
#endif

    // No entropy credits for the saved seed.
    credits = 0;

    // Trigger an automatic save once the entropy credits max out.
    firstSave = 1;

    // Rekey the random number generator immediately.
    rekey();

    // Stir in the supplied tag data but don't credit any entropy to it.
    if (tag)
        stir((const uint8_t *)tag, strlen(tag));

#if defined(RNG_DUE_TRNG)
    // Stir in the unique identifier for the CPU so that different
    // devices will give different outputs even without seeding.
    stirUniqueIdentifier();
#elif defined(ESP8266)
    // ESP8266's have a 32-bit CPU chip ID and 32-bit flash chip ID
    // that we can use as a device unique identifier.
    uint32_t ids[2];
    ids[0] = ESP.getChipId();
    ids[1] = ESP.getFlashChipId();
    stir((const uint8_t *)ids, sizeof(ids));
#elif defined(ESP32)
    // ESP32's have a MAC address that can be used as a device identifier.
    uint64_t mac = ESP.getEfuseMac();
    stir((const uint8_t *)&mac, sizeof(mac));
#else
    // AVR devices don't have anything like a serial number so it is
    // difficult to make every device unique.  Use the compilation
    // time and date to provide a little randomness across applications
    // if not across devices running the same pre-compiled application.
    tag = __TIME__ __DATE__;
    stir((const uint8_t *)tag, strlen(tag));
#endif

#if defined(RNG_WATCHDOG)
    // Disable interrupts and reset the watchdog.
    cli();
    wdt_reset();

    // Clear the "reset due to watchdog" flag.
    MCUSR &= ~(1 << WDRF);

    // Enable the watchdog with the smallest duration (16ms)
    // and interrupt-only mode.
    _WD_CONTROL_REG |= (1 << _WD_CHANGE_BIT) | (1 << WDE);
    _WD_CONTROL_REG = (1 << WDIE);

    // Re-enable interrupts.  The watchdog should be running.
    sei();
#endif

    // Re-save the seed to obliterate the previous value and to ensure
    // that if the system is reset without a call to save() that we won't
    // accidentally generate the same sequence of random data again.
    save();

    // The RNG has now been initialized.
    initialized = 1;
}

/**
 * \brief Adds a noise source to the random number generator.
 *
 * \param source The noise source to add, which will be polled regularly
 * by loop() to accumulate noise-based entropy from the source.
 *
 * RNG supports a maximum of four noise sources.  If the application needs
 * more than that then the application must poll the noise sources itself by
 * calling NoiseSource::stir() directly.
 *
 * \sa loop(), begin()
 */
void RNGClass::addNoiseSource(NoiseSource &source)
{
    #define MAX_NOISE_SOURCES (sizeof(noiseSources) / sizeof(noiseSources[0]))
    if (count < MAX_NOISE_SOURCES) {
        noiseSources[count++] = &source;
        source.added();
    }
}

/**
 * \brief Sets the amount of time between automatic seed saves.
 *
 * \param minutes The number of minutes between automatic seed saves.
 *
 * The default time between automatic seed saves is 1 hour.
 *
 * This function is intended to help with EEPROM wear by slowing down how
 * often seed data is saved as noise is stirred into the random pool.
 * The exact period to use depends upon how long you intend to field
 * the device before replacing it.  For example, an EEPROM rated for
 * 100k erase/write cycles will last about 69 days saving once a minute
 * or 11 years saving once an hour.
 *
 * \sa save(), stir()
 */
void RNGClass::setAutoSaveTime(uint16_t minutes)
{
    if (!minutes)
        minutes = 1; // Just in case.
    timeout = ((uint32_t)minutes) * 60000U;
}

/**
 * \brief Generates random bytes into a caller-supplied buffer.
 *
 * \param data Points to the buffer to fill with random bytes.
 * \param len Number of bytes to generate.
 *
 * Calling this function will decrease the amount of entropy in the
 * random number pool by \a len * 8 bits.  If there isn't enough
 * entropy, then this function will still return \a len bytes of
 * random data generated from what entropy it does have.
 *
 * If the application requires a specific amount of entropy before
 * generating important values, the available() function can be
 * polled to determine when sufficient entropy is available.
 *
 * \sa available(), stir()
 */
void RNGClass::rand(uint8_t *data, size_t len)
{
    // Make sure that the RNG is initialized in case the application
    // forgot to call RNG.begin() at startup time.
    if (!initialized)
        begin(0);

    // Decrease the amount of entropy in the pool.
    if ( (uint16_t)len > (credits / 8))
        credits = 0;
    else
        credits -= len * 8;

    // If we have pending TRNG data from the loop() function,
    // then force a stir on the state.  Otherwise mix in some
    // fresh data from the TRNG because it is possible that
    // the application forgot to call RNG.loop().
    if (trngPending) {
        stir(0, 0, 0);
        trngPending = 0;
        trngPosn = 0;
    } else {
        mixTRNG();
    }

    // Generate the random data.
    uint8_t count = 0;
    while (len > 0) {
        // Force a rekey if we have generated too many blocks in this request.
        if (count >= RNG_REKEY_BLOCKS) {
            rekey();
            count = 1;
        } else {
            ++count;
        }

        // Increment the low counter word and generate a new keystream block.
        ++(block[12]);
        ChaCha::hashCore(stream, block, RNG_ROUNDS);

        // Copy the data to the return buffer.
        if (len < 64) {
            memcpy(data, stream, len);
            break;
        } else {
            memcpy(data, stream, 64);
            data += 64;
            len -= 64;
        }
    }

    // Force a rekey after every request.
    rekey();
}

/**
 * \brief Determine if there is sufficient entropy available for a
 * specific request size.
 *
 * \param len The number of bytes of random data that will be requested
 * via a call to rand().
 * \return Returns true if there is at least \a len * 8 bits of entropy
 * in the random number pool, or false if not.
 *
 * This function can be used by the application to wait for sufficient
 * entropy to become available from the system's noise sources before
 * generating important values.  For example:
 *
 * \code
 * bool haveKey = false;
 * byte key[32];
 *
 * void loop() {
 *     ...
 *
 *     if (!haveKey && RNG.available(sizeof(key))) {
 *         RNG.rand(key, sizeof(key));
 *         haveKey = true;
 *     }
 *
 *     ...
 * }
 * \endcode
 *
 * If \a len is larger than the maximum number of entropy credits supported
 * by the random number pool (384 bits, 48 bytes), then the maximum will be
 * used instead.  For example, asking if 512 bits (64 bytes) are available
 * will return true if in reality only 384 bits are available.  If this is a
 * problem for the application's security requirements, then large requests
 * for random data should be broken up into smaller chunks with the
 * application waiting for the entropy pool to refill between chunks.
 *
 * \sa rand()
 */
bool RNGClass::available(size_t len) const
{
    if (len >= (RNG_MAX_CREDITS / 8))
        return credits >= RNG_MAX_CREDITS;
    else
        return (uint16_t)len <= (credits / 8);
}

/**
 * \brief Stirs additional entropy data into the random pool.
 *
 * \param data Points to the additional data to be stirred in.
 * \param len Number of bytes to be stirred in.
 * \param credit The number of bits of entropy to credit for the
 * data that is stirred in.  Note that this is bits, not bytes.
 *
 * The maximum credit allowed is \a len * 8 bits, indicating that
 * every bit in the input \a data is good and random.  Practical noise
 * sources are rarely that good, so \a credit will usually be smaller.
 * For example, to credit 2 bits of entropy per byte, the function
 * would be called as follows:
 *
 * \code
 * RNG.stir(noise_data, noise_bytes, noise_bytes * 2);
 * \endcode
 *
 * If \a credit is zero, then the \a data will be stirred in but no
 * entropy credit is given.  This is useful for static values like
 * serial numbers and MAC addresses that are different between
 * devices but highly predictable.
 *
 * \sa loop()
 */
void RNGClass::stir(const uint8_t *data, size_t len, unsigned int credit)
{
    // Increase the entropy credit.
    if ((credit / 8) >= len && len)
        credit = len * 8;
    if ((uint16_t)(RNG_MAX_CREDITS - credits) > credit)
        credits += credit;
    else
        credits = RNG_MAX_CREDITS;

    // Process the supplied input data.
    if (len > 0) {
        // XOR the data with the ChaCha input block in 48 byte
        // chunks and rekey the ChaCha cipher for each chunk to mix
        // the data in.  This should scatter any "true entropy" in
        // the input across the entire block.
        while (len > 0) {
            size_t templen = len;
            if (templen > 48)
                templen = 48;
            uint8_t *output = ((uint8_t *)block) + 16;
            len -= templen;
            while (templen > 0) {
                *output++ ^= *data++;
                --templen;
            }
            rekey();
        }
    } else {
        // There was no input data, so just force a rekey so we
        // get some mixing of the state even without new data.
        rekey();
    }

    // Save if this is the first time we have reached max entropy.
    // This provides some protection if the system is powered off before
    // the first auto-save timeout occurs.
    if (firstSave && credits >= RNG_MAX_CREDITS) {
        firstSave = 0;
        save();
    }
}

/**
 * \brief Saves the random seed to EEPROM.
 *
 * During system startup, noise sources typically won't have accumulated
 * much entropy.  But startup is usually the time when the system most
 * needs to generate random data for session keys, IV's, and the like.
 *
 * The purpose of this function is to pass some of the accumulated entropy
 * from one session to the next after a loss of power.  Thus, once the system
 * has been running for a while it will get progressively better at generating
 * random values and the accumulated entropy will not be completely lost.
 *
 * Normally it isn't necessary to call save() directly.  The loop() function
 * will automatically save the seed on a periodic basis (default of 1 hour).
 *
 * The seed that is saved is generated in such a way that it cannot be used
 * to predict random values that were generated previously or subsequently
 * in the current session.  So a compromise of the EEPROM contents of a
 * captured device should not result in compromise of random values
 * that have already been generated.  However, if power is lost and the
 * system restarted, then there will be a short period of time where the
 * random state will be predictable from the seed.  For this reason it is
 * very important to stir() in new noise data at startup.
 *
 * \sa loop(), stir()
 */
void RNGClass::save()
{
    // Generate random data from the current state and save
    // that as the seed.  Then force a rekey.
    ++(block[12]);
    ChaCha::hashCore(stream, block, RNG_ROUNDS);
#if defined(RNG_EEPROM)
    // We shorten the seed from 48 bytes to 47 to leave room for
    // the CRC-8 value.  We do this to align the data on an 8-byte
    // boundary in EERPOM.
    int address = RNG_EEPROM_ADDRESS;
    eeprom_write_block(stream, (void *)address, SEED_SIZE - 1);
    eeprom_write_byte((uint8_t *)(address + SEED_SIZE - 1),
                      crypto_crc8('S', stream, SEED_SIZE - 1));
#elif defined(RNG_DUE_TRNG)
    unsigned posn;
    ((uint32_t *)(RNG_SEED_ADDR))[0] = crypto_crc8('S', stream, SEED_SIZE);
    for (posn = 0; posn < 12; ++posn)
        ((uint32_t *)(RNG_SEED_ADDR))[posn + 1] = stream[posn];
    for (posn = 13; posn < (RNG_FLASH_PAGE_SIZE / 4); ++posn)
        ((uint32_t *)(RNG_SEED_ADDR))[posn + 13] = 0xFFFFFFFF;
    eraseAndWriteSeed();
#elif defined(RNG_ESP_NVS)
    // Save the seed into ESP non-volatile storage (NVS).
    nvs_handle handle = 0;
    if (nvs_open("rng", NVS_READWRITE, &handle) == 0) {
        nvs_erase_all(handle);
        nvs_set_blob(handle, "seed", stream, SEED_SIZE);
        nvs_commit(handle);
        nvs_close(handle);
    }
#endif
    rekey();
    timer = millis();
}

/**
 * \brief Run periodic housekeeping tasks on the random number generator.
 *
 * This function must be called on a regular basis from the application's
 * main "loop()" function.
 */
void RNGClass::loop()
{
    // Stir in the entropy from all registered noise sources.
    for (uint8_t posn = 0; posn < count; ++posn)
        noiseSources[posn]->stir();

#if defined(RNG_DUE_TRNG)
    // If there is data available from the Arudino Due's TRNG, then XOR
    // it with the state block and increase the entropy credit.  We don't
    // call stir() yet because that will seriously slow down the system
    // given how fast the TRNG is.  Instead we save up the XOR'ed TRNG
    // data until the next rand() call and then hash it to generate the
    // desired output.
    //
    // The CPU documentation claims that the TRNG output is very good so
    // this should only make the pool more and more random as time goes on.
    // However there is a risk that the CPU manufacturer was pressured by
    // government or intelligence agencies to insert a back door that
    // generates predictable output.  Or the manufacturer was overly
    // optimistic about their TRNG design and it is actually flawed in a
    // way they don't realise.
    //
    // If you are concerned about such threats, then make sure to mix in
    // data from other noise sources.  By hashing together the TRNG with
    // the other noise data, rand() should produce unpredictable data even
    // if one of the sources is actually predictable.
    if ((REG_TRNG_ISR & TRNG_ISR_DATRDY) != 0) {
        block[4 + trngPosn] ^= REG_TRNG_ODATA;
        if (++trngPosn >= 12)
            trngPosn = 0;
        if (credits < RNG_MAX_CREDITS) {
            // Credit 1 bit of entropy for the word.  The TRNG should be
            // better than this but it is so fast that we want to collect
            // up more data before passing it to the application.
            ++credits;
        }
        trngPending = 1;
    }
#elif defined(RNG_WORD_TRNG)
    // Read a word from the TRNG and XOR it into the state.
    block[4 + trngPosn] ^= RNG_WORD_TRNG_GET();
    if (++trngPosn >= 12)
        trngPosn = 0;
    if (credits < RNG_MAX_CREDITS) {
        // Credit 1 bit of entropy for the word.  The TRNG should be
        // better than this but it is so fast that we want to collect
        // up more data before passing it to the application.
        ++credits;
    }
    trngPending = 1;
#elif defined(RNG_WATCHDOG)
    // Read the 32 bit buffer from the WDT interrupt.
    cli();
    if (outBits >= 32) {
        uint32_t value = hash;
        hash = 0;
        outBits = 0;
        sei();

        // Final steps of the Jenkin's one-at-a-time hash function.
        // https://en.wikipedia.org/wiki/Jenkins_hash_function
        value += leftShift3(value);
        value ^= rightShift11(value);
        value += leftShift15(value);

        // Credit 1 bit of entropy for each byte of input.  It can take
        // between 30 and 40 seconds to accumulate 256 bits of credit.
        credits += 4;
        if (credits > RNG_MAX_CREDITS)
            credits = RNG_MAX_CREDITS;

        // XOR the word with the state.  Stir once we accumulate 48 bytes,
        // which happens about once every 6.4 seconds.
        block[4 + trngPosn] ^= value;
        if (++trngPosn >= 12) {
            trngPosn = 0;
            trngPending = 0;
            stir(0, 0, 0);
        } else {
            trngPending = 1;
        }
    } else {
        sei();
    }
#endif

    // Save the seed if the auto-save timer has expired.
    if ((millis() - timer) >= timeout)
        save();
}

/**
 * \brief Destroys the data in the random number pool and the saved seed
 * in EEPROM.
 *
 * This function attempts to throw away any data that could theoretically be
 * used to predict previous and future outputs of the random number generator
 * if the device is captured, sold, or otherwise compromised.
 *
 * After this function is called, begin() must be called again to
 * re-initialize the random number generator.
 *
 * \note The rand() and save() functions take some care to manage the
 * random number pool in a way that makes prediction of past outputs from a
 * captured state very difficult.  Future outputs may be predictable if
 * noise or other high-entropy data is not mixed in with stir() on a
 * regular basis.
 *
 * \sa begin()
 */
void RNGClass::destroy()
{
    clean(block);
    clean(stream);
#if defined(RNG_EEPROM)
    int address = RNG_EEPROM_ADDRESS;
    for (int posn = 0; posn < SEED_SIZE; ++posn)
        eeprom_write_byte((uint8_t *)(address + posn), 0xFF);
#elif defined(RNG_DUE_TRNG)
    for (unsigned posn = 0; posn < (RNG_FLASH_PAGE_SIZE / 4); ++posn)
        ((uint32_t *)(RNG_SEED_ADDR))[posn] = 0xFFFFFFFF;
    eraseAndWriteSeed();
#elif defined(RNG_ESP_NVS)
    nvs_handle handle = 0;
    if (nvs_open("rng", NVS_READWRITE, &handle) == 0) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
#endif
    initialized = 0;
}

/**
 * \brief Rekeys the random number generator.
 */
void RNGClass::rekey()
{
    // Rekey the cipher for the next request by generating a new block.
    // This is intended to make it difficult to wind the random number
    // backwards if the state is captured later.  The first 16 bytes of
    // "block" remain set to "tagRNG".
    ++(block[12]);
    ChaCha::hashCore(stream, block, RNG_ROUNDS);
    memcpy(block + 4, stream, 48);

    // Permute the high word of the counter using the system microsecond
    // counter to introduce a little bit of non-stir randomness for each
    // request.  Note: If random data is requested on a predictable schedule
    // then this may not help very much.  It is still necessary to stir in
    // high quality entropy data on a regular basis using stir().
    block[13] ^= micros();
}

/**
 * \brief Mix in fresh data from the TRNG when rand() is called.
 */
void RNGClass::mixTRNG()
{
#if defined(RNG_DUE_TRNG)
    // Mix in 12 words from the Due's TRNG.
    for (int posn = 0; posn < 12; ++posn) {
        // According to the documentation the TRNG should produce a new
        // 32-bit random value every 84 clock cycles.  If it still hasn't
        // produced a value after 200 iterations, then assume that the
        // TRNG is not producing output and stop.
        int counter;
        for (counter = 0; counter < 200; ++counter) {
            if ((REG_TRNG_ISR & TRNG_ISR_DATRDY) != 0)
                break;
        }
        if (counter >= 200)
            break;
        block[posn + 4] ^= REG_TRNG_ODATA;
    }
#elif defined(RNG_WORD_TRNG)
    // Read 12 words from the TRNG and XOR them into the state.
    for (uint8_t index = 4; index < 16; ++index)
        block[index] ^= RNG_WORD_TRNG_GET();
#elif defined(RNG_WATCHDOG)
    // Read the pending 32 bit buffer from the WDT interrupt and mix it in.
    cli();
    if (outBits >= 32) {
        uint32_t value = hash;
        hash = 0;
        outBits = 0;
        sei();

        // Final steps of the Jenkin's one-at-a-time hash function.
        // https://en.wikipedia.org/wiki/Jenkins_hash_function
        value += leftShift3(value);
        value ^= rightShift11(value);
        value += leftShift15(value);

        // XOR the word with the state.
        block[4] ^= value;
    } else {
        sei();
    }
#endif
}

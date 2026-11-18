/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ============================================================================
 * instrument_cluster_shim.c
 *
 * Purpose: make the head unit tell the iPhone that a SECOND display exists,
 * receive the compressed video the phone sends for it, and hand those frames
 * to a separate process that puts them on the instrument cluster.
 *
 * The file is split into numbered sections. The three intercepted entry points
 * are DECLARED first (section 1); their bodies live with the feature they
 * implement - the display advertisement in section 11, the /info dictionary in
 * section 12, the stream setup in section 19. Everything below a body supports
 * it.
 *
 * Nothing in here draws anything. The only outbound path is a socket.
 *
 * Build: src/shim/build.sh (no libc headers, links the unit's own libc).
 * ==========================================================================*/

/* ---------------------------------------------------------------------------
 * SECTION 1 - THE THREE ENTRY POINTS
 *
 * The injector repoints three call sites inside the running carplay process at
 * the three functions below. Each one calls the original first, then adds what
 * the head unit does not do.
 *
 *   AirPlayInfoArrayAddScreenDisplay  - add a display to the list
 *   AirPlayCopyServerInfo             - build the /info dictionary
 *   AirPlayReceiverSessionSetup       - handle a stream request
 * ------------------------------------------------------------------------- */

int AirPlayInfoArrayAddScreenDisplay(void *displayList,
                                     const void *displayIdentifier,
                                     int features,
                                     int primaryInputDevice,
                                     int maximumFramesPerSecond,
                                     int widthInPixels,
                                     int heightInPixels,
                                     int widthInMillimetres,
                                     int heightInMillimetres);

void *AirPlayCopyServerInfo(void *server, void *unusedSecondArgument, void *errorOut);

int AirPlayReceiverSessionSetup(void *session,
                                void *requestDictionary,
                                void **responseDictionaryOut,
                                int fourthArgument);

/* provided by mib3navtext.c */
extern void mib3_navtext_install(void (*logMessage)(const char *),
                                 int (*realConnect)(int, const void *, unsigned int),
                                 long (*realSend)(int, const void *, unsigned long, int),
                                 int (*realClose)(int));
extern int mib3_navtext_connect(int, const void *, unsigned int);
extern long mib3_navtext_send(int, const void *, unsigned long, int);
extern int mib3_navtext_close(int);
extern int mib3_navtext_mode(void);

/* ---------------------------------------------------------------------------
 * SECTION 2 - THE C LIBRARY WE BORROW, DECLARED BY HAND
 *
 * The head unit's C library is older than any toolchain available here, so the
 * build does not include its headers. The declarations below are exactly what
 * those headers would have said. See src/shim/build.sh for how the library is
 * linked without a matching sysroot.
 * ------------------------------------------------------------------------- */

extern void *dlsym(void *libraryHandle, const char *symbolName);
extern char *getenv(const char *variableName);
extern int open(const char *path, int flags, ...);
extern long write(int filedescriptor, const void *buffer, unsigned long byteCount);
extern long read(int filedescriptor, void *buffer, unsigned long byteCount);
extern int close(int filedescriptor);
extern void *memset(void *destination, int fillByte, unsigned long byteCount);
extern void *memcpy(void *destination, const void *source, unsigned long byteCount);
extern int strcmp(const char *first, const char *second);
extern long time(long *secondsOut);
extern int getpid(void);
extern void *malloc(unsigned long byteCount);
extern void free(void *memory);
extern int fsync(int filedescriptor);
extern int access(const char *path, int mode);
extern int socket(int domain, int type, int protocol);
extern int connect(int filedescriptor, const void *address, unsigned int addressLength);
extern int bind(int filedescriptor, const void *address, unsigned int addressLength);
extern int listen(int filedescriptor, int backlog);
extern int accept(int filedescriptor, void *addressOut, unsigned int *addressLengthOut);
extern int setsockopt(int filedescriptor, int level, int option,
                      const void *value, unsigned int valueLength);
extern int getsockopt(int filedescriptor, int level, int option,
                      void *valueOut, unsigned int *valueLengthInOut);
extern int getsockname(int filedescriptor, void *addressOut, unsigned int *addressLengthInOut);
extern long send(int filedescriptor, const void *buffer, unsigned long byteCount, int flags);
extern int poll(void *pollDescriptorArray, unsigned long count, int timeoutMilliseconds);
extern int unlink(const char *path);
extern int setenv(const char *name, const char *value, int overwrite);
extern int *__errno_location(void);
extern void *opendir(const char *path);
extern void *readdir(void *directoryStream);
extern int closedir(void *directoryStream);
typedef int (*createThreadFunction)(void **, const void *, void *(*)(void *), void *);
static createThreadFunction create_thread;

#define ERROR_NUMBER (*__errno_location())

#define FLAG_WRITE_ONLY     1
#define FLAG_CREATE         0100
#define FLAG_TRUNCATE       01000
#define FLAG_APPEND         02000
#define FLAG_NONBLOCKING    04000

#define LOOKUP_NEXT_OBJECT    ((void *)-1L)
#define LOOKUP_ANY_OBJECT     ((void *)0)

#define SOCKET_DOMAIN_UNIX     1
#define SOCKET_DOMAIN_INET6   10
#define SOCKET_STREAM          1
#define SOCKET_LEVEL_BASE      1
#define SOCKET_LEVEL_IPV6     41
#define SOCKET_OPTION_REUSE_ADDRESS  2
#define SOCKET_OPTION_SEND_BUFFER    7
#define SOCKET_OPTION_SEND_TIMEOUT  21
#define SOCKET_OPTION_IPV6_ONLY_V6   26

#define POLL_READABLE  1
#define POLL_WRITABLE  4

#define ERROR_INTERRUPTED   4
#define ERROR_WOULD_BLOCK  11

#define SEND_FLAG_NO_SIGNAL   0x4000
#define SEND_FLAG_DO_NOT_WAIT 0x40

#define ADDRESS_FAMILY_UNIX   1
#define ADDRESS_FAMILY_INET6 10

static int error_is_interrupted(void) { return ERROR_NUMBER == ERROR_INTERRUPTED; }
static int error_would_block(void)   { return ERROR_NUMBER == ERROR_WOULD_BLOCK; }

/* ---------------------------------------------------------------------------
 * SECTION 3 - THE FOUNDATION DIFFERENCE TYPES AND THE SYMBOLS WE BORROW
 * ------------------------------------------------------------------------- */

typedef unsigned char  byte;
typedef unsigned int   word32;
typedef unsigned long  word64;
typedef long           signedWord;
typedef unsigned long  frameSequence;

/* The head unit's CoreFoundation implementation. Every object is opaque to us:
 * we only ever hand them back to the functions that created them. */
typedef const void *coreFoundationString;
typedef const void *coreFoundationArray;
typedef void       *mutableCoreFoundationArray;
typedef const void *coreFoundationDictionary;
typedef void       *mutableCoreFoundationDictionary;
typedef const void *coreFoundationObject;
typedef long        coreFoundationIndex;

#define STRING_ENCODING_UTF8 0x08000100

/* ---- CoreFoundation function shapes we need ---------------------------- */
typedef coreFoundationString (*createStringFunction)(const void *, const char *, word32);
typedef unsigned char (*getStringFunction)(coreFoundationString, char *, long, word32);
typedef long          (*arrayCountFunction)(coreFoundationArray);
typedef coreFoundationObject (*arrayValueFunction)(coreFoundationArray, long);
typedef mutableCoreFoundationArray (*createArrayFunction)(const void *, long, const void *);
typedef void          (*arrayAppendFunction)(mutableCoreFoundationArray, const void *);
typedef void          (*arrayAppendIntegerFunction)(mutableCoreFoundationArray, signedWord);
typedef void          (*dictionarySetFunction)(mutableCoreFoundationDictionary,
                                               const void *, const void *);
typedef coreFoundationObject (*dictionaryGetFunction)(coreFoundationDictionary, const void *);
typedef void          (*dictionarySetIntegerFunction)(mutableCoreFoundationDictionary,
                                                      const void *, signedWord);
typedef mutableCoreFoundationDictionary (*createDictionaryFunction)(const void *, long,
                                                                    const void *, const void *);
typedef signedWord    (*dictionaryGetIntegerFunction)(coreFoundationDictionary,
                                                      const void *, signedWord);
typedef word64        (*objectTypeFunction)(coreFoundationObject);
typedef word64        (*constantTypeFunction)(void);
typedef void          (*releaseFunction)(coreFoundationObject);

static createStringFunction           create_string_from_characters;
static getStringFunction              read_string_characters;
static arrayCountFunction             array_count;
static arrayValueFunction             array_value_at;
static createArrayFunction            create_array;
static arrayAppendFunction            array_append;
static arrayAppendIntegerFunction     array_append_integer;
static dictionarySetFunction          dictionary_set;
static dictionaryGetFunction          dictionary_get;
static dictionarySetIntegerFunction   dictionary_set_integer;
static createDictionaryFunction       create_dictionary;
static dictionaryGetIntegerFunction   dictionary_get_integer;
static objectTypeFunction             object_type;
static constantTypeFunction           array_type_identifier;
static constantTypeFunction           dictionary_type_identifier;
static releaseFunction                release_object;

static void *array_callbacks;
static void *dictionary_key_callbacks;
static void *dictionary_value_callbacks;
static void *boolean_true_variable;
static void *boolean_false_variable;

/* ---- the unit's own helpers, borrowed by name -------------------------- */
typedef int  (*deriveStreamKeyFunction)(void *, int, signedWord, void *, void *);
typedef void (*cipherInitFunction)(void *, const void *, const void *);
typedef void (*cipherHeaderFunction)(void *, const void *, unsigned long);
typedef long (*cipherDecryptFunction)(void *, const void *, long, void *);
typedef long (*cipherVerifyFunction)(void *, const void *, const void *, int *);
typedef int  (*aesInitialiseFunction)(void *, const void *, const void *);
typedef int  (*aesUpdateFunction)(void *, const void *, long, void *);
typedef int  (*chachaKeyDeriveFunction)(void *, signedWord, int, int, void *);
typedef int  (*sendCommandFunction)(void *, void *, void *, void *);

static deriveStreamKeyFunction    derive_stream_key_for_screen;
static cipherInitFunction         chaCha_initialise;
static cipherHeaderFunction       chaCha_authenticate_header;
static cipherDecryptFunction      chaCha_decrypt;
static cipherVerifyFunction       chaCha_verify;
static aesInitialiseFunction      aes_counters_initialise;
static aesUpdateFunction          aes_counters_advance;
static sendCommandFunction        send_command_to_phone;

static void *the_real_add_screen_display;
static void *the_real_session_setup;
static void *the_real_copy_server_info;
static char *airplay_library_base;

#define CHACHA_KEY_DERIVE_OFFSET   0x163d8
#define AIRPLAY_ADD_DISPLAY_OFFSET 0x1f670

/* Symbols are looked up once. "The next object" is right when this library is
 * loaded before the original; "any object" is right when it is injected last,
 * which is the live case. */
static void *lookup_symbol(const char *symbolName)
{
    void *address = dlsym(LOOKUP_NEXT_OBJECT, symbolName);
    if (!address) address = dlsym(LOOKUP_ANY_OBJECT, symbolName);
    return address;
}

static int symbols_have_been_resolved = 0;

static void resolve_all_symbols(void)
{
    if (symbols_have_been_resolved) return;
    symbols_have_been_resolved = 1;

    create_string_from_characters = (createStringFunction)lookup_symbol("CFStringCreateWithCString");
    read_string_characters        = (getStringFunction)lookup_symbol("CFStringGetCString");
    array_count                   = (arrayCountFunction)lookup_symbol("CFArrayGetCount");
    array_value_at                = (arrayValueFunction)lookup_symbol("CFArrayGetValueAtIndex");
    create_array                  = (createArrayFunction)lookup_symbol("CFArrayCreateMutable");
    array_append                  = (arrayAppendFunction)lookup_symbol("CFArrayAppendValue");
    array_append_integer          = (arrayAppendIntegerFunction)lookup_symbol("CFArrayAppendInt64");
    dictionary_set                = (dictionarySetFunction)lookup_symbol("CFDictionarySetValue");
    dictionary_get                = (dictionaryGetFunction)lookup_symbol("CFDictionaryGetValue");
    dictionary_set_integer        = (dictionarySetIntegerFunction)lookup_symbol("CFDictionarySetInt64");
    create_dictionary             = (createDictionaryFunction)lookup_symbol("CFDictionaryCreateMutable");
    dictionary_get_integer        = (dictionaryGetIntegerFunction)lookup_symbol("CFDictionaryGetInt64");
    object_type                   = (objectTypeFunction)lookup_symbol("CFGetTypeID");
    array_type_identifier         = (constantTypeFunction)lookup_symbol("CFArrayGetTypeID");
    dictionary_type_identifier    = (constantTypeFunction)lookup_symbol("CFDictionaryGetTypeID");
    release_object                = (releaseFunction)lookup_symbol("CFRelease");

    array_callbacks            = lookup_symbol("kCFLArrayCallBacksCFLTypes");
    dictionary_key_callbacks   = lookup_symbol("kCFLDictionaryKeyCallBacksCFLTypes");
    dictionary_value_callbacks = lookup_symbol("kCFLDictionaryValueCallBacksCFLTypes");
    boolean_true_variable      = lookup_symbol("kCFLBooleanTrue");
    boolean_false_variable     = lookup_symbol("kCFLBooleanFalse");

    create_thread               = (createThreadFunction)lookup_symbol("pthread_create");

    derive_stream_key_for_screen = (deriveStreamKeyFunction)
        lookup_symbol("AirPlay_DeriveAESKeySHA512ForScreen");
    chaCha_initialise        = (cipherInitFunction)lookup_symbol("chacha20_poly1305_init_64x64");
    chaCha_authenticate_header = (cipherHeaderFunction)lookup_symbol("chacha20_poly1305_add_aad");
    chaCha_decrypt           = (cipherDecryptFunction)lookup_symbol("chacha20_poly1305_decrypt");
    chaCha_verify            = (cipherVerifyFunction)lookup_symbol("chacha20_poly1305_verify");
    aes_counters_initialise  = (aesInitialiseFunction)lookup_symbol("AES_CTR_Init");
    aes_counters_advance     = (aesUpdateFunction)lookup_symbol("AES_CTR_Update");
    send_command_to_phone    = (sendCommandFunction)lookup_symbol("AirPlayReceiverSessionSendCommand");

    the_real_add_screen_display = lookup_symbol("AirPlayInfoArrayAddScreenDisplay");
    the_real_session_setup      = lookup_symbol("AirPlayReceiverSessionSetup");
    the_real_copy_server_info   = lookup_symbol("AirPlayCopyServerInfo");

    if (the_real_add_screen_display)
        airplay_library_base = (char *)the_real_add_screen_display - AIRPLAY_ADD_DISPLAY_OFFSET;
}

/* ---------------------------------------------------------------------------
 * SECTION 4 - TEXT BUILDING AND LOGGING
 *
 * No stdio: a line is assembled in a fixed stack buffer and written once.
 * ------------------------------------------------------------------------- */

typedef struct {
    char *characters;
    unsigned long capacity;
    unsigned long length;
} textBuffer;

static void text_buffer_append_character(textBuffer *buffer, char character)
{
    if (buffer->length < buffer->capacity) {
        buffer->characters[buffer->length] = character;
        buffer->length++;
    }
}

static void text_buffer_append_string(textBuffer *buffer, const char *text)
{
    unsigned long index = 0;
    while (text && text[index]) text_buffer_append_character(buffer, text[index++]);
}

static void text_buffer_append_number(textBuffer *buffer, word64 value,
                                      unsigned long base, int minimumWidth)
{
    char reversedDigits[24];
    int digitCount = 0;
    const char *digitCharacters = "0123456789abcdef";

    if (value == 0) reversedDigits[digitCount++] = '0';
    while (value) {
        reversedDigits[digitCount++] = digitCharacters[value % base];
        value /= base;
    }
    while (digitCount < minimumWidth) reversedDigits[digitCount++] = '0';
    while (digitCount--) text_buffer_append_character(buffer, reversedDigits[digitCount]);
}

static void text_buffer_append_hexadecimal(textBuffer *buffer, word64 value)
{
    text_buffer_append_string(buffer, "0x");
    text_buffer_append_number(buffer, value, 16, 0);
}

static int log_file_descriptor = -1;

static void open_log_file(void)
{
    if (log_file_descriptor < 0)
        log_file_descriptor = open("/tmp/mib3shim.log",
                                   FLAG_WRITE_ONLY | FLAG_CREATE | FLAG_APPEND, 0644);
}

static void log_message(const char *tag, const char *message)
{
    char bufferStorage[512];
    textBuffer buffer = { bufferStorage, sizeof(bufferStorage) - 2, 0 };

    buffer.characters[buffer.length] = '\0';
    text_buffer_append_string(&buffer, "[mib3shim] ");
    text_buffer_append_string(&buffer, tag);
    text_buffer_append_string(&buffer, ": ");
    text_buffer_append_string(&buffer, message);
    text_buffer_append_character(&buffer, '\n');
    buffer.characters[buffer.length] = '\0';

    open_log_file();
    if (log_file_descriptor >= 0)
        write(log_file_descriptor, buffer.characters, buffer.length);
}

static void log_number(const char *tag, const char *label, word64 value, int asHex)
{
    char labelStorage[192];
    textBuffer buffer = { labelStorage, sizeof(labelStorage) - 2, 0 };

    text_buffer_append_string(&buffer, label);
    text_buffer_append_character(&buffer, '=');
    if (asHex) text_buffer_append_hexadecimal(&buffer, value);
    else       text_buffer_append_number(&buffer, value, 10, 0);
    buffer.characters[buffer.length] = '\0';

    log_message(tag, buffer.characters);
}

/* ---------------------------------------------------------------------------
 * SECTION 5 - CONFIGURATION
 * ------------------------------------------------------------------------- */

static int read_flag_setting(const char *variableName, int defaultValue)
{
    char *text = getenv(variableName);
    if (!text || !text[0]) return defaultValue;
    return (text[0] == '1' || text[0] == 'y' || text[0] == 'Y'
            || text[0] == 't' || text[0] == 'T');
}

static word64 read_number_setting(const char *variableName, word64 defaultValue)
{
    char *text = getenv(variableName);
    word64 value = 0;
    int sawAnyDigit = 0;

    if (!text) return defaultValue;
    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (word64)(*text - '0');
        text++;
        sawAnyDigit = 1;
    }
    return sawAnyDigit ? value : defaultValue;
}

/* ---------------------------------------------------------------------------
 * SECTION 6 - SHORT WRAPPERS OVER THE DICTIONARY API
 *
 * The key strings are created once and cached, and every wrapper releases what
 * it creates, because the head unit's dictionary retains its own references.
 * ------------------------------------------------------------------------- */

typedef struct {
    coreFoundationString identifier;
    coreFoundationString uniqueIdentifier;
    coreFoundationString name;
    coreFoundationString type;
    coreFoundationString properties;
    coreFoundationString initialVideoStreams;
    coreFoundationString adjacentViewAreas;
    coreFoundationString cornerMasks;
    coreFoundationString nightMode;
    coreFoundationString limitedUserInterface;
    coreFoundationString limitedUserInterfaceElements;
    coreFoundationString displayPanels;
    coreFoundationString extendedFeatures;
    coreFoundationString displays;
    coreFoundationString viewAreas;
    coreFoundationString initialViewArea;
    coreFoundationString initialAddress;
    coreFoundationString safeArea;
    coreFoundationString originXInPixels;
    coreFoundationString originYInPixels;
    coreFoundationString widthInPixels;
    coreFoundationString heightInPixels;
    coreFoundationString widthInMillimetres;
    coreFoundationString heightInMillimetres;
    coreFoundationString maximumFramesPerSecond;
    coreFoundationString streams;
    coreFoundationString dataPort;
    coreFoundationString streamConnectionIdentifier;
    coreFoundationString instrumentClusterName;
    coreFoundationString altScreenFeature;
    coreFoundationString viewAreasFeature;
    coreFoundationString showsInstruments;
} cachedKeyStrings;

static cachedKeyStrings keys;
static int key_strings_are_cached = 0;

static coreFoundationString make_string(const char *text)
{
    return create_string_from_characters ? create_string_from_characters(0, text,
                                                                        STRING_ENCODING_UTF8) : 0;
}

static void cache_key_strings(void)
{
    if (key_strings_are_cached) return;
    keys.identifier        = make_string("uid");
    keys.uniqueIdentifier  = make_string("uuid");
    keys.name              = make_string("name");
    keys.type              = make_string("type");
    keys.properties        = make_string("properties");
    keys.initialVideoStreams = make_string("initialVideoStreams");
    keys.adjacentViewAreas = make_string("adjacentViewAreas");
    keys.cornerMasks       = make_string("cornerMasks");
    keys.nightMode         = make_string("nightMode");
    keys.limitedUserInterface = make_string("limitedUI");
    keys.limitedUserInterfaceElements = make_string("limitedUIElements");
    keys.displayPanels     = make_string("displayPanels");
    keys.extendedFeatures  = make_string("extendedFeatures");
    keys.displays          = make_string("displays");
    keys.viewAreas         = make_string("viewAreas");
    keys.initialViewArea   = make_string("initialViewArea");
    keys.initialAddress    = make_string("initialURL");
    keys.safeArea          = make_string("safeArea");
    keys.originXInPixels   = make_string("originXPixels");
    keys.originYInPixels   = make_string("originYPixels");
    keys.widthInPixels     = make_string("widthPixels");
    keys.heightInPixels    = make_string("heightPixels");
    keys.widthInMillimetres  = make_string("widthPhysical");
    keys.heightInMillimetres = make_string("heightPhysical");
    keys.maximumFramesPerSecond = make_string("maxFPS");
    keys.streams           = make_string("streams");
    keys.dataPort          = make_string("dataPort");
    keys.streamConnectionIdentifier = make_string("streamConnectionID");
    keys.instrumentClusterName = make_string("Instrument Cluster");
    keys.altScreenFeature  = make_string("altScreen");
    keys.viewAreasFeature  = make_string("viewAreas");
    keys.showsInstruments  = make_string("showsInstruments");

    key_strings_are_cached = keys.identifier && keys.uniqueIdentifier && keys.streams
                          && keys.displayPanels && keys.viewAreas;
}

static int object_is_dictionary(coreFoundationObject value)
{
    return value && object_type && dictionary_type_identifier
        && object_type(value) == dictionary_type_identifier();
}

static int object_is_array(coreFoundationObject value)
{
    return value && object_type && array_type_identifier
        && object_type(value) == array_type_identifier();
}

static void dictionary_put_integer(mutableCoreFoundationDictionary dictionary,
                                   const char *keyText, signedWord value)
{
    coreFoundationString key = make_string(keyText);
    if (!dictionary || !key || !dictionary_set_integer) return;
    dictionary_set_integer(dictionary, key, value);
    if (release_object) release_object(key);
}

static void dictionary_put_text(mutableCoreFoundationDictionary dictionary,
                                const char *keyText, const char *valueText)
{
    coreFoundationString key = make_string(keyText);
    coreFoundationString value = make_string(valueText);
    if (dictionary && key && value && dictionary_set) dictionary_set(dictionary, key, value);
    if (release_object) { if (value) release_object(value); if (key) release_object(key); }
}

static void dictionary_put_object(mutableCoreFoundationDictionary dictionary,
                                  const char *keyText, const void *value)
{
    coreFoundationString key = make_string(keyText);
    if (dictionary && key && value && dictionary_set) dictionary_set(dictionary, key, value);
    if (release_object && key) release_object(key);
}

static void *dictionary_read_by_name(coreFoundationDictionary dictionary, const char *keyText)
{
    coreFoundationString key = make_string(keyText);
    void *value = 0;
    if (dictionary && key && dictionary_get) value = (void *)dictionary_get(dictionary, key);
    if (release_object && key) release_object(key);
    return value;
}

static signedWord dictionary_read_integer(coreFoundationDictionary dictionary,
                                          const char *keyText, signedWord defaultValue)
{
    coreFoundationString key = make_string(keyText);
    signedWord value = defaultValue;
    if (dictionary && key && dictionary_get && dictionary_get_integer) {
        if (dictionary_get(dictionary, key))
            value = dictionary_get_integer(dictionary, key, 0);
    }
    if (release_object && key) release_object(key);
    return value;
}

static mutableCoreFoundationArray make_empty_array(void)
{
    if (!create_array || !array_callbacks) return 0;
    return create_array(0, 0, array_callbacks);
}

/* ---------------------------------------------------------------------------
 * SECTION 7 - TIME
 *
 * Monotonic milliseconds. Resolved on first use so the module works even when
 * it is driven outside carplay (the benches do exactly that).
 * ------------------------------------------------------------------------- */

typedef struct { long seconds; long nanoseconds; } timestampStorage;

static long (*clock_gettime_symbol)(int, void *);
static long (*syscall_symbol)(long, ...);
static int clock_warning_has_been_logged = 0;

static word64 monotonic_milliseconds(void)
{
    timestampStorage now;
    now.seconds = 0;
    now.nanoseconds = 0;

    if (!clock_gettime_symbol && !syscall_symbol) {
        clock_gettime_symbol = (long (*)(int, void *))dlsym(LOOKUP_ANY_OBJECT, "clock_gettime");
        if (!clock_gettime_symbol)
            syscall_symbol = (long (*)(long, ...))dlsym(LOOKUP_ANY_OBJECT, "syscall");
    }
    if (clock_gettime_symbol)      clock_gettime_symbol(1 /* monotonic */, &now);
    else if (syscall_symbol)       syscall_symbol(113 /* clock_gettime */, 1, &now);
    else if (!clock_warning_has_been_logged) {
        clock_warning_has_been_logged = 1;
        log_message("time", "no clock available - timers are disabled");
    }
    return (word64)now.seconds * 1000u + (word64)now.nanoseconds / 1000000u;
}

/* ---------------------------------------------------------------------------
 * SECTION 8 - THE ACTIVATION GATE
 *
 * The mod is enabled by a file on the customer's USB stick, checked at runtime
 * rather than at boot, because the stick mounts after the carplay service
 * starts. Unplug the stick and the next session behaves exactly like stock.
 * ------------------------------------------------------------------------- */

static int marker_exists_in_directory(const char *directory)
{
    char path[256];
    unsigned long length = 0;
    const char *prefix = (const char *)0;

    while (*directory && length < sizeof(path) - 32) path[length++] = *directory++;
    for (prefix = "/PART1/mib3shim.enable"; *prefix && length < sizeof(path) - 1; )
        path[length++] = *prefix++;
    path[length] = '\0';

    return access(path, 0 /* existence only */) == 0;
}

static int activation_gate_is_open(void)
{
    char *forced = getenv("MIB3_SHIM_GATE");
    int portNumber = 0;

    if (forced && forced[0] == '0') return 0;
    if (forced && forced[0] == '1') return 1;

    for (portNumber = 1; portNumber <= 8; portNumber++) {
        char candidate[32];
        unsigned long length = 0;
        const char *base = "/tmp/USB/PORT";

        while (*base && length < sizeof(candidate) - 1) candidate[length++] = *base++;
        if (portNumber >= 10) candidate[length++] = (char)('0' + portNumber / 10);
        candidate[length++] = (char)('0' + portNumber % 10);
        candidate[length] = '\0';
        if (marker_exists_in_directory(candidate)) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * SECTION 9 - CLUSTER GEOMETRY
 *
 * One place decides the pixel size, the advertised physical size in
 * millimetres, and therefore the density the phone believes the panel has.
 * Every other section asks here.
 * ------------------------------------------------------------------------- */

typedef struct {
    int widthInPixels;
    int heightInPixels;
    int widthInMillimetres;
    int heightInMillimetres;
} clusterGeometry;

static int round_up_to_multiple_of_eight(int value)
{
    if (value < 8) return 8;
    return (value + 7) & ~7;
}

static void resolve_cluster_geometry(clusterGeometry *geometry)
{
    long scalePercent = (long)read_number_setting("MIB3_SHIM_SCALE", 100);
    long pixelWidth   = (long)read_number_setting("MIB3_SHIM_W", 1280);
    long pixelHeight  = (long)read_number_setting("MIB3_SHIM_H", 480);
    long millimetreWidth;
    long millimetreHeight;
    int heightWasGiven = 0;

    if (scalePercent <= 0) scalePercent = 100;
    if (scalePercent < 25) scalePercent = 25;
    if (scalePercent > 400) scalePercent = 400;

    if (scalePercent != 100) {
        pixelWidth  = pixelWidth  * scalePercent / 100;
        pixelHeight = pixelHeight * scalePercent / 100;
    }
    geometry->widthInPixels  = round_up_to_multiple_of_eight((int)pixelWidth);
    geometry->heightInPixels = round_up_to_multiple_of_eight((int)pixelHeight);
    if (geometry->widthInPixels  > 8192) geometry->widthInPixels  = 8192;
    if (geometry->heightInPixels > 8192) geometry->heightInPixels = 8192;

    millimetreWidth = (long)read_number_setting("MIB3_SHIM_CWPHY", 292);
    {
        char *explicitHeight = getenv("MIB3_SHIM_CHPHY");
        heightWasGiven = (explicitHeight && explicitHeight[0]) ? 1 : 0;
        millimetreHeight = heightWasGiven ? (long)read_number_setting("MIB3_SHIM_CHPHY", 110) : 0;
    }
    if (read_flag_setting("MIB3_SHIM_SCALE_PHY", 0)) {
        millimetreWidth = millimetreWidth * scalePercent / 100;
        if (heightWasGiven) millimetreHeight = millimetreHeight * scalePercent / 100;
    }
    if (millimetreWidth < 10)   millimetreWidth = 10;
    if (millimetreWidth > 4000) millimetreWidth = 4000;

    /* keep the advertised aspect ratio honest when no height was pinned */
    if (!heightWasGiven)
        millimetreHeight = (millimetreWidth * (long)geometry->heightInPixels
                            + (long)geometry->widthInPixels / 2)
                           / (long)geometry->widthInPixels;
    if (millimetreHeight < 10)   millimetreHeight = 10;
    if (millimetreHeight > 4000) millimetreHeight = 4000;

    geometry->widthInMillimetres  = (int)millimetreWidth;
    geometry->heightInMillimetres = (int)millimetreHeight;
}

/* ---------------------------------------------------------------------------
 * SECTION 10 - VIEW AREAS
 *
 * The phone draws the cluster scene into a box we declare here, and it picks a
 * card design from the size of the nested safe area. The thresholds below are
 * the phone's own classifier, reconstructed from its behaviour: get the box
 * wrong and the cluster draws nothing at all. There is no graceful fallback.
 *
 *   wide    : safe width  >= 660 * scale   -> a column down one edge
 *   centred : safe height >  383 * scale   -> a centred card
 *   else    : layout zero, which is a blank cluster
 * ------------------------------------------------------------------------- */

typedef struct {
    int originX, originY, width, height;
    int safeOriginX, safeOriginY, safeWidth, safeHeight;
} viewAreaRectangle;

#define MILLIMETRES_PER_SCALE_UNIT 153
#define WIDE_CLASS_WIDTH           660
#define CENTRED_CLASS_HEIGHT       383
#define ETA_CLASS_HEIGHT           202
#define REFERENCE_DENSITY          670          /* pixels per millimetre * 1000 at 1x */

static int classifier_scale_milli(int pixelWidth, int millimetreWidth)
{
    if (millimetreWidth < 50 || millimetreWidth > 2000) millimetreWidth = 292;
    if (pixelWidth < 100) pixelWidth = 1280;
    return (int)((MILLIMETRES_PER_SCALE_UNIT * (long)pixelWidth) / (long)millimetreWidth);
}

static int wide_class_width_threshold(int pixelWidth, int millimetreWidth)
{
    return (int)((WIDE_CLASS_WIDTH * (long)classifier_scale_milli(pixelWidth, millimetreWidth))
                 / 1000L);
}

static int centred_class_height_threshold(int pixelWidth, int millimetreWidth)
{
    return (int)((CENTRED_CLASS_HEIGHT * (long)classifier_scale_milli(pixelWidth, millimetreWidth))
                 / 1000L) + 1;
}

static int eta_class_height_threshold(int pixelWidth, int millimetreWidth)
{
    return (int)((ETA_CLASS_HEIGHT * (long)classifier_scale_milli(pixelWidth, millimetreWidth))
                 / REFERENCE_DENSITY);
}

/* 0 = the phone will draw nothing, 1 = centred card, 2 = wide column */
static int classify_safe_area(int safeWidth, int safeHeight,
                             int pixelWidth, int millimetreWidth)
{
    if (safeWidth <= 0 || safeHeight <= 0) return 0;
    if (safeWidth >= wide_class_width_threshold(pixelWidth, millimetreWidth)) return 2;
    if (safeHeight > centred_class_height_threshold(pixelWidth, millimetreWidth)) return 1;
    return 0;
}

/* An advertised area that leaves the frame is rejected along with the whole
 * display list, so every rectangle is pulled back inside and the fact is
 * logged rather than shipped silently. */
static int clamp_rectangle_into_frame(viewAreaRectangle *rectangle,
                                      int frameWidth, int frameHeight)
{
    int wasClamped = 0;

    if (rectangle->width <= 0 || rectangle->width > frameWidth) {
        rectangle->width = frameWidth; wasClamped = 1;
    }
    if (rectangle->height <= 0 || rectangle->height > frameHeight) {
        rectangle->height = frameHeight; wasClamped = 1;
    }
    if (rectangle->originX < 0 || rectangle->originX + rectangle->width > frameWidth) {
        rectangle->originX = 0; rectangle->width = frameWidth; wasClamped = 1;
    }
    if (rectangle->originY < 0 || rectangle->originY + rectangle->height > frameHeight) {
        rectangle->originY = 0; rectangle->height = frameHeight; wasClamped = 1;
    }
    if (rectangle->safeWidth <= 0)   { rectangle->safeWidth = rectangle->width;   wasClamped = 1; }
    if (rectangle->safeHeight <= 0)  { rectangle->safeHeight = rectangle->height; wasClamped = 1; }
    if (rectangle->safeOriginX < 0)  { rectangle->safeOriginX = rectangle->originX; wasClamped = 1; }
    if (rectangle->safeOriginY < 0)  { rectangle->safeOriginY = rectangle->originY; wasClamped = 1; }

    if (rectangle->safeOriginX < rectangle->originX) rectangle->safeOriginX = rectangle->originX;
    if (rectangle->safeOriginY < rectangle->originY) rectangle->safeOriginY = rectangle->originY;
    if (rectangle->safeOriginX + rectangle->safeWidth > rectangle->originX + rectangle->width)
        rectangle->safeWidth = rectangle->originX + rectangle->width - rectangle->safeOriginX;
    if (rectangle->safeOriginY + rectangle->safeHeight > rectangle->originY + rectangle->height)
        rectangle->safeHeight = rectangle->originY + rectangle->height - rectangle->safeOriginY;

    if (rectangle->safeWidth  < 8) rectangle->safeWidth  = 8;
    if (rectangle->safeHeight < 8) rectangle->safeHeight = 8;
    return wasClamped;
}

/* The three cluster layouts. Coordinates are fractions of the panel so the same
 * card lands in the same class at any pixel size; the first two are measured on
 * the car, the third is a placeholder pending measurement. */
static void preset_rectangle_for_layout(int layoutIndex, int pixelWidth, int pixelHeight,
                                       viewAreaRectangle *rectangle)
{
    clusterGeometry geometry;
    int wideThreshold;

    resolve_cluster_geometry(&geometry);
    wideThreshold = wide_class_width_threshold(pixelWidth, geometry.widthInMillimetres);

    /* the visible frame stays the whole panel; shrinking it gets rejected */
    rectangle->originX = 0;
    rectangle->originY = 0;
    rectangle->width   = pixelWidth;
    rectangle->height  = pixelHeight;

    if (layoutIndex == 0) {
        int safeWidth  = pixelWidth * 5 / 8;
        int safeHeight = pixelHeight * 7 / 12;
        if (safeWidth  < wideThreshold + wideThreshold / 3) safeWidth = wideThreshold + wideThreshold / 3;
        if (safeHeight < pixelHeight * 242 / 480)           safeHeight = pixelHeight * 242 / 480;
        if (safeWidth  > pixelWidth)  safeWidth  = pixelWidth;
        if (safeHeight > pixelHeight) safeHeight = pixelHeight;
        rectangle->safeWidth  = safeWidth;
        rectangle->safeHeight = safeHeight;
        rectangle->safeOriginX = (pixelWidth  - safeWidth)  / 2;
        rectangle->safeOriginY = (pixelHeight - safeHeight) / 2;
    } else if (layoutIndex == 1) {
        /* measured on the car: 439,50,397,375 on a 1280x480 panel */
        rectangle->safeWidth  = pixelWidth  * 397 / 1280;
        rectangle->safeHeight = pixelHeight * 375 / 480;
        rectangle->safeOriginX = pixelWidth  * 439 / 1280;
        rectangle->safeOriginY = pixelHeight * 50  / 480;
        if (rectangle->safeWidth  < pixelWidth  * 165 / 1280) rectangle->safeWidth  = pixelWidth  * 165 / 1280;
        if (rectangle->safeHeight < pixelHeight * 253 / 480)  rectangle->safeHeight = pixelHeight * 253 / 480;
    } else {
        int safeWidth  = pixelWidth * 47 / 100;
        int safeHeight = pixelHeight * 62 / 100;
        if (safeWidth  < wideThreshold + wideThreshold / 3) safeWidth = wideThreshold + wideThreshold / 3;
        if (safeHeight < pixelHeight * 242 / 480)           safeHeight = pixelHeight * 242 / 480;
        if (safeWidth  > pixelWidth)  safeWidth  = pixelWidth;
        if (safeHeight > pixelHeight) safeHeight = pixelHeight;
        rectangle->safeWidth  = safeWidth;
        rectangle->safeHeight = safeHeight;
        rectangle->safeOriginX = pixelWidth / 20;
        rectangle->safeOriginY = (pixelHeight - safeHeight) / 2;
    }
}

static int parse_rectangle_setting(const char *text, viewAreaRectangle *rectangle)
{
    int values[8];
    int valueCount = 0;
    int sawAnyDigit = 0;

    if (!text || !rectangle) return 0;
    for (;;) {
        while (*text == ' ' || *text == '\t' || *text == ',') text++;
        if (*text < '0' || *text > '9') break;
        {
            int parsed = 0;
            while (*text >= '0' && *text <= '9') { parsed = parsed * 10 + (*text - '0'); text++; }
            values[valueCount++] = parsed;
            sawAnyDigit = 1;
        }
        if (valueCount >= 8) break;
    }
    if (valueCount < 4 || !sawAnyDigit) return 0;

    rectangle->originX = values[0];
    rectangle->originY = values[1];
    rectangle->width   = values[2];
    rectangle->height  = values[3];
    if (valueCount >= 8) {
        rectangle->safeOriginX = values[4];
        rectangle->safeOriginY = values[5];
        rectangle->safeWidth   = values[6];
        rectangle->safeHeight  = values[7];
    } else {
        rectangle->safeOriginX = rectangle->originX;
        rectangle->safeOriginY = rectangle->originY;
        rectangle->safeWidth   = rectangle->width;
        rectangle->safeHeight  = rectangle->height;
    }
    return 1;
}

static int declared_view_area_count(void)
{
    long count = (long)read_number_setting("MIB3_SHIM_VA_COUNT", 3);
    if (count < 1) count = 1;
    if (count > 8) count = 8;
    return (int)count;
}

/* which area the phone is believed to be showing; moves only when the phone
 * confirms a switch, never when one is merely requested */
static volatile int current_view_area_index = 0;

static viewAreaRectangle rectangle_for_area_index(int areaIndex, int pixelWidth, int pixelHeight)
{
    char settingName[24];
    const char *settingPrefix = "MIB3_SHIM_VA";
    const char *sourceText = 0;
    viewAreaRectangle rectangle;
    int nameLength = 0;

    while (*settingPrefix && nameLength < (int)sizeof(settingName) - 3)
        settingName[nameLength++] = *settingPrefix++;
    settingName[nameLength++] = (char)('0' + (areaIndex % 10));
    settingName[nameLength] = '\0';

    sourceText = getenv(settingName);
    if (!(sourceText && sourceText[0] && parse_rectangle_setting(sourceText, &rectangle))) {
        preset_rectangle_for_layout(areaIndex, pixelWidth, pixelHeight, &rectangle);
    }
    if (clamp_rectangle_into_frame(&rectangle, pixelWidth, pixelHeight)) {
        log_number("view-areas", "rectangle clamped for area", (word64)areaIndex, 0);
    }
    return rectangle;
}

static mutableCoreFoundationDictionary build_area_dictionary(const viewAreaRectangle *rectangle)
{
    mutableCoreFoundationDictionary area;
    mutableCoreFoundationDictionary safeArea;

    if (!create_dictionary || !dictionary_key_callbacks || !dictionary_value_callbacks) return 0;

    area     = create_dictionary(0, 0, dictionary_key_callbacks, dictionary_value_callbacks);
    safeArea = create_dictionary(0, 0, dictionary_key_callbacks, dictionary_value_callbacks);
    if (!area || !safeArea) {
        if (release_object) { if (safeArea) release_object(safeArea); if (area) release_object(area); }
        return 0;
    }

    dictionary_put_integer(area, "widthPixels",  (signedWord)rectangle->width);
    dictionary_put_integer(area, "heightPixels", (signedWord)rectangle->height);
    dictionary_put_integer(area, "originXPixels", (signedWord)rectangle->originX);
    dictionary_put_integer(area, "originYPixels", (signedWord)rectangle->originY);

    dictionary_put_integer(safeArea, "widthPixels",  (signedWord)rectangle->safeWidth);
    dictionary_put_integer(safeArea, "heightPixels", (signedWord)rectangle->safeHeight);
    dictionary_put_integer(safeArea, "originXPixels", (signedWord)rectangle->safeOriginX);
    dictionary_put_integer(safeArea, "originYPixels", (signedWord)rectangle->safeOriginY);

    /* The deployment sets MIB3_SHIM_VA_NOOUTSIDE=1, so OMITTING the key is the
     * default; setting the knob to 0 restores it. The knob is honoured either
     * way, which is what keeps the shim and up.sh in balance. */
    if (!read_flag_setting("MIB3_SHIM_VA_NOOUTSIDE", 1) && boolean_true_variable)
        dictionary_put_object(safeArea, "drawUIOutsideSafeArea", *(void **)boolean_true_variable);

    dictionary_put_object(area, "safeArea", safeArea);
    if (release_object) release_object(safeArea);
    return area;
}

static mutableCoreFoundationArray build_view_areas(int pixelWidth, int pixelHeight)
{
    mutableCoreFoundationArray areas;
    int areaCount = declared_view_area_count();
    int areaIndex;

    if (!create_array || !array_callbacks || !array_append) return 0;
    areas = create_array(0, areaCount, array_callbacks);
    if (!areas) return 0;

    for (areaIndex = 0; areaIndex < areaCount; areaIndex++) {
        viewAreaRectangle rectangle = rectangle_for_area_index(areaIndex, pixelWidth, pixelHeight);
        mutableCoreFoundationDictionary areaDictionary = build_area_dictionary(&rectangle);
        clusterGeometry geometry;
        int classification;

        if (!areaDictionary) continue;
        array_append(areas, areaDictionary);
        if (release_object) release_object(areaDictionary);

        resolve_cluster_geometry(&geometry);
        classification = classify_safe_area(rectangle.safeWidth, rectangle.safeHeight,
                                            pixelWidth, geometry.widthInMillimetres);
        log_number("view-areas", "area index", (word64)areaIndex, 0);
        log_number("view-areas", "  safe width",  (word64)rectangle.safeWidth, 0);
        log_number("view-areas", "  safe height", (word64)rectangle.safeHeight, 0);
        /* 0 means the phone will draw a blank cluster - worth seeing in the log */
        log_number("view-areas", "  classification", (word64)classification, 0);
        if (rectangle.safeHeight <= eta_class_height_threshold(pixelWidth, geometry.widthInMillimetres))
            log_message("view-areas", "  arrival time will be suppressed (safe area too short)");
    }
    return areas;
}

/* ---------------------------------------------------------------------------
 * SECTION 11 - THE DISPLAY ADVERTISEMENT  (entry point 1)
 *
 * The real function only writes eight keys and has no "type" argument, so a
 * second call alone produces a second MAIN screen. The phone only believes this
 * is the instrument cluster once the type, the scene address and the view areas
 * are added, and once /info carries the modern schema in section 12.
 * ------------------------------------------------------------------------- */

static char cluster_display_identifier[64];
static char cluster_scene_address[160];
static int cluster_was_advertised = 0;

#define DEFAULT_CLUSTER_SCENE "maps:/car/instrumentcluster/map"

static int display_list_already_has_cluster(void *displayList)
{
    coreFoundationArray list = (coreFoundationArray)displayList;
    long entryCount;
    long entryIndex;
    int wantedType = (int)read_number_setting("MIB3_SHIM_TYPE", 111);

    if (!list || !array_count || !array_value_at) return 0;
    entryCount = array_count(list);
    for (entryIndex = 0; entryIndex < entryCount; entryIndex++) {
        coreFoundationObject entry = array_value_at(list, entryIndex);
        if (!object_is_dictionary(entry)) continue;
        if (!dictionary_get((coreFoundationDictionary)entry, keys.type)) continue;
        if ((int)dictionary_get_integer((coreFoundationDictionary)entry, keys.type, 0) == wantedType)
            return 1;
    }
    return 0;
}

static void add_cluster_display_fields(void *displayList, int pixelWidth, int pixelHeight)
{
    coreFoundationArray list = (coreFoundationArray)displayList;
    long entryCount;
    mutableCoreFoundationDictionary clusterEntry;

    if (!list || !array_count || !array_value_at) return;
    entryCount = array_count(list);
    if (entryCount < 1) return;
    clusterEntry = (mutableCoreFoundationDictionary)array_value_at(list, entryCount - 1);
    if (!object_is_dictionary(clusterEntry)) {
        log_message("advertisement", "no dictionary to annotate");
        return;
    }

    dictionary_put_integer(clusterEntry, "type",
                           (signedWord)read_number_setting("MIB3_SHIM_TYPE", 111));

    if (!read_flag_setting("MIB3_SHIM_NOURL", 0)) {
        dictionary_put_text(clusterEntry, "initialURL",
                            cluster_scene_address[0] ? cluster_scene_address : DEFAULT_CLUSTER_SCENE);
    }

    if (read_flag_setting("MIB3_SHIM_VIEWAREAS", 0)) {
        mutableCoreFoundationArray areas = build_view_areas(pixelWidth, pixelHeight);
        if (areas) {
            int areaCount = 0;
            int initialIndex;

            dictionary_put_object(clusterEntry, "viewAreas", areas);
            if (array_count) areaCount = (int)array_count(areas);

            initialIndex = (int)read_number_setting("MIB3_SHIM_VA_INITIAL", 1);
            if (initialIndex < 0 || (areaCount > 0 && initialIndex >= areaCount)) initialIndex = 0;
            dictionary_put_integer(clusterEntry, "initialViewArea", (signedWord)initialIndex);
            current_view_area_index = initialIndex;
            if (release_object) release_object(areas);
        }
    } else {
        mutableCoreFoundationArray areas = make_empty_array();
        mutableCoreFoundationDictionary singleArea;
        mutableCoreFoundationDictionary safeArea;

        if (!areas) return;
        singleArea = create_dictionary ? create_dictionary(0, 0, dictionary_key_callbacks,
                                                           dictionary_value_callbacks) : 0;
        safeArea   = create_dictionary ? create_dictionary(0, 0, dictionary_key_callbacks,
                                                           dictionary_value_callbacks) : 0;
        if (singleArea && safeArea) {
            dictionary_put_integer(singleArea, "widthPixels",  (signedWord)pixelWidth);
            dictionary_put_integer(singleArea, "heightPixels", (signedWord)pixelHeight);
            dictionary_put_integer(singleArea, "originXPixels", 0);
            dictionary_put_integer(singleArea, "originYPixels", 0);
            dictionary_put_integer(safeArea, "widthPixels",  (signedWord)pixelWidth);
            dictionary_put_integer(safeArea, "heightPixels", (signedWord)pixelHeight);
            dictionary_put_integer(safeArea, "originXPixels", 0);
            dictionary_put_integer(safeArea, "originYPixels", 0);
            if (!read_flag_setting("MIB3_SHIM_VA_NOOUTSIDE", 1) && boolean_true_variable)
                dictionary_put_object(safeArea, "drawUIOutsideSafeArea",
                                      *(void **)boolean_true_variable);
            dictionary_put_object(singleArea, "safeArea", safeArea);
            array_append(areas, singleArea);
            dictionary_put_object(clusterEntry, "viewAreas", areas);
            dictionary_put_integer(clusterEntry, "initialViewArea", 0);
        }
        if (release_object) {
            if (safeArea)   release_object(safeArea);
            if (singleArea) release_object(singleArea);
            if (areas)      release_object(areas);
        }
    }
}

int AirPlayInfoArrayAddScreenDisplay(void *displayList,
                                     const void *displayIdentifier,
                                     int features,
                                     int primaryInputDevice,
                                     int maximumFramesPerSecond,
                                     int widthInPixels,
                                     int heightInPixels,
                                     int widthInMillimetres,
                                     int heightInMillimetres)
{
    clusterGeometry geometry;
    int realResult;
    int clusterResult;
    coreFoundationString clusterIdentifier;
    const char *configuredIdentifier;

    log_message("advertisement", "entered");
    resolve_all_symbols();

    if (!the_real_add_screen_display) {
        log_message("advertisement", "the real function was not found");
        return -1;
    }

    /* 1. the original call, exactly as the head unit makes it */
    realResult = ((int (*)(void *, const void *, int, int, int, int, int, int, int))
                  the_real_add_screen_display)(displayList, displayIdentifier, features,
                                               primaryInputDevice, maximumFramesPerSecond,
                                               widthInPixels, heightInPixels,
                                               widthInMillimetres, heightInMillimetres);

    log_number("advertisement", "real result", (word64)(word32)realResult, 0);
    log_number("advertisement", "gate open", (word64)activation_gate_is_open(), 0);

    if (realResult != 0) return realResult;
    if (!read_flag_setting("MIB3_SHIM_ADD", 1)) return realResult;
    if (!activation_gate_is_open()) return realResult;

    cache_key_strings();
    if (!key_strings_are_cached) {
        log_message("advertisement", "the dictionary keys are unavailable");
        return realResult;
    }

    if (display_list_already_has_cluster(displayList)) {
        cluster_was_advertised = 1;
        log_message("advertisement", "cluster already present - not added twice");
        return realResult;
    }

    /* remember the exact identity: /info and every command must name it */
    configuredIdentifier = getenv("MIB3_SHIM_CUUID");
    if (!configuredIdentifier || !configuredIdentifier[0])
        configuredIdentifier = "4d494233-0000-4000-8000-000000000001";
    {
        unsigned long length = 0;
        while (configuredIdentifier[length] && length < sizeof(cluster_display_identifier) - 1) {
            cluster_display_identifier[length] = configuredIdentifier[length];
            length++;
        }
        cluster_display_identifier[length] = '\0';
    }
    {
        const char *configuredAddress = getenv("MIB3_SHIM_URL");
        const char *chosenAddress = (configuredAddress && configuredAddress[0])
                                  ? configuredAddress : DEFAULT_CLUSTER_SCENE;
        unsigned long length = 0;
        while (chosenAddress[length] && length < sizeof(cluster_scene_address) - 1) {
            cluster_scene_address[length] = chosenAddress[length];
            length++;
        }
        cluster_scene_address[length] = '\0';
    }

    clusterIdentifier = make_string(cluster_display_identifier);
    resolve_cluster_geometry(&geometry);

    log_message("advertisement", "adding the cluster display");
    log_number("advertisement", "cluster width",  (word64)geometry.widthInPixels, 0);
    log_number("advertisement", "cluster height", (word64)geometry.heightInPixels, 0);
    log_number("advertisement", "cluster width in mm",  (word64)geometry.widthInMillimetres, 0);
    log_number("advertisement", "cluster height in mm", (word64)geometry.heightInMillimetres, 0);
    log_number("advertisement", "advertised frames per second",
               read_number_setting("MIB3_SHIM_FPS", 60), 0);

    clusterResult = ((int (*)(void *, const void *, int, int, int, int, int, int, int))
                     the_real_add_screen_display)(
                        displayList, clusterIdentifier,
                        (int)read_number_setting("MIB3_SHIM_CFEAT", 0x0A),
                        (int)read_number_setting("MIB3_SHIM_CINDEV", 0),
                        (int)read_number_setting("MIB3_SHIM_FPS", 60),
                        geometry.widthInPixels, geometry.heightInPixels,
                        geometry.widthInMillimetres, geometry.heightInMillimetres);

    log_number("advertisement", "cluster result", (word64)(word32)clusterResult, 0);
    if (clusterResult == 0) {
        add_cluster_display_fields(displayList, geometry.widthInPixels, geometry.heightInPixels);
        cluster_was_advertised = 1;
    }
    if (release_object && clusterIdentifier) release_object(clusterIdentifier);
    return realResult;
}

/* ---------------------------------------------------------------------------
 * SECTION 12 - THE /info DICTIONARY  (entry point 3)
 *
 * This is the only seam that can add a sibling of "displays", which is why the
 * modern schema lives here. The phone rejects the whole display list when one
 * entry cannot be parsed, so every key below is required, not decorative.
 * ------------------------------------------------------------------------- */

static int complete_display_entry(mutableCoreFoundationDictionary entry,
                                 int isClusterEntry, signedWord nightModeValue)
{
    coreFoundationObject identifierValue;

    if (!entry || !object_is_dictionary(entry) || !create_dictionary
        || !dictionary_set || !dictionary_get) return 0;
    identifierValue = (coreFoundationObject)dictionary_get(entry, keys.uniqueIdentifier);

    /* the phone reads "uid" for the panel list; without it the entry is dropped */
    if (identifierValue && !dictionary_get(entry, keys.identifier))
        dictionary_set(entry, keys.identifier, identifierValue);

    if (isClusterEntry) {
        if (!dictionary_get(entry, keys.name))
            dictionary_set(entry, keys.name, keys.instrumentClusterName);
        if (!dictionary_get(entry, keys.properties)) {
            mutableCoreFoundationArray properties = make_empty_array();
            if (properties) {
                array_append(properties, keys.instrumentClusterName);
                dictionary_set(entry, keys.properties, properties);
                if (release_object) release_object(properties);
            }
        }
    }

    if (dictionary_set_integer) {
        if (!dictionary_get(entry, keys.cornerMasks)) {
            if (boolean_false_variable)
                dictionary_set(entry, keys.cornerMasks, *(void **)boolean_false_variable);
            else
                dictionary_set_integer(entry, keys.cornerMasks, 0);
        }
        if (!dictionary_get(entry, keys.limitedUserInterface))
            dictionary_set_integer(entry, keys.limitedUserInterface, 0);
        if (!dictionary_get(entry, keys.nightMode))
            dictionary_set_integer(entry, keys.nightMode, nightModeValue);
    }

    if (!dictionary_get(entry, keys.limitedUserInterfaceElements)) {
        mutableCoreFoundationArray elements = make_empty_array();
        if (elements) {
            dictionary_set(entry, keys.limitedUserInterfaceElements, elements);
            if (release_object) release_object(elements);
        }
    }

    /* a present but EMPTY array here ends the session at zero seconds */
    if (!dictionary_get(entry, keys.initialVideoStreams) && identifierValue) {
        mutableCoreFoundationArray streamsList = make_empty_array();
        if (streamsList) {
            mutableCoreFoundationDictionary streamEntry =
                create_dictionary(0, 0, dictionary_key_callbacks, dictionary_value_callbacks);
            if (streamEntry) {
                dictionary_put_integer(streamEntry, "originXPixels", 0);
                dictionary_put_integer(streamEntry, "originYPixels", 0);
                dictionary_put_integer(streamEntry, "zIndex", 0);
                dictionary_put_object(streamEntry, "uuid", identifierValue);
                array_append(streamsList, streamEntry);
                if (release_object) release_object(streamEntry);
            }
            dictionary_set(entry, keys.initialVideoStreams, streamsList);
            if (release_object) release_object(streamsList);
        }
    }

    if (isClusterEntry && !dictionary_get(entry, keys.adjacentViewAreas)) {
        mutableCoreFoundationArray adjacent = make_empty_array();
        if (adjacent) {
            dictionary_set(entry, keys.adjacentViewAreas, adjacent);
            if (release_object) release_object(adjacent);
        }
    }
    return 1;
}

static void build_display_panels(mutableCoreFoundationDictionary infoDictionary)
{
    coreFoundationObject displayList;
    mutableCoreFoundationArray panels;
    long displayCount;
    long displayIndex;

    displayList = (coreFoundationObject)dictionary_get(infoDictionary, keys.displays);
    if (!object_is_array(displayList) || !create_array || !array_callbacks
        || !create_dictionary) return;

    panels = create_array(0, 0, array_callbacks);
    if (!panels) return;

    displayCount = array_count((coreFoundationArray)displayList);
    for (displayIndex = 0; displayIndex < displayCount; displayIndex++) {
        mutableCoreFoundationDictionary display =
            (mutableCoreFoundationDictionary)array_value_at((coreFoundationArray)displayList,
                                                            displayIndex);
        coreFoundationString displayIdentifier;
        mutableCoreFoundationDictionary panel;
        char panelIdentifier[96];
        unsigned long length = 0;
        int isCluster;

        if (!object_is_dictionary(display)) continue;
        displayIdentifier = (coreFoundationString)dictionary_get(display, keys.uniqueIdentifier);
        if (!displayIdentifier || !read_string_characters) continue;
        panelIdentifier[0] = '\0';
        if (!read_string_characters(displayIdentifier, panelIdentifier,
                                    sizeof(panelIdentifier) - 16, STRING_ENCODING_UTF8)) continue;
        while (panelIdentifier[length]) length++;
        {
            const char *suffix = "-panel";
            int suffixIndex = 0;
            while (suffix[suffixIndex] && length < sizeof(panelIdentifier) - 1)
                panelIdentifier[length++] = suffix[suffixIndex++];
            panelIdentifier[length] = '\0';
        }

        panel = create_dictionary(0, 0, dictionary_key_callbacks, dictionary_value_callbacks);
        if (!panel) continue;

        dictionary_put_text(panel, "uid", panelIdentifier);
        dictionary_put_integer(panel, "widthPixels",  dictionary_read_integer(display, "widthPixels", 0));
        dictionary_put_integer(panel, "heightPixels", dictionary_read_integer(display, "heightPixels", 0));
        dictionary_put_integer(panel, "widthPhysical",
                               dictionary_read_integer(display, "widthPhysical", 0));
        dictionary_put_integer(panel, "heightPhysical",
                               dictionary_read_integer(display, "heightPhysical", 0));

        isCluster = ((int)dictionary_read_integer(display, "type", 0) == 111);
        {
            mutableCoreFoundationArray properties = make_empty_array();
            if (properties) {
                if (isCluster) array_append(properties, keys.showsInstruments);
                dictionary_put_object(panel, "properties", properties);
                if (release_object) release_object(properties);
            }
        }
        {
            mutableCoreFoundationArray streamsList = make_empty_array();
            mutableCoreFoundationDictionary streamEntry =
                create_dictionary(0, 0, dictionary_key_callbacks, dictionary_value_callbacks);
            if (streamsList && streamEntry) {
                dictionary_put_integer(streamEntry, "originXPixels", 0);
                dictionary_put_integer(streamEntry, "originYPixels", 0);
                dictionary_put_integer(streamEntry, "zIndex", 0);
                dictionary_put_object(streamEntry, "uuid", displayIdentifier);
                array_append(streamsList, streamEntry);
                dictionary_put_object(panel, "initialVideoStreams", streamsList);
            }
            if (release_object) {
                if (streamEntry) release_object(streamEntry);
                if (streamsList) release_object(streamsList);
            }
        }
        array_append(panels, panel);
        if (release_object) release_object(panel);
    }
    dictionary_set(infoDictionary, keys.displayPanels, panels);
    if (release_object) release_object(panels);
}

static int complete_info_dictionary(mutableCoreFoundationDictionary infoDictionary)
{
    coreFoundationObject displayList;
    mutableCoreFoundationDictionary mainDisplay = 0;
    mutableCoreFoundationDictionary clusterDisplay = 0;
    long displayCount;
    long displayIndex;
    signedWord nightModeValue = 0;

    if (!infoDictionary || !dictionary_get || !array_count || !array_value_at) return 0;
    cache_key_strings();
    if (!key_strings_are_cached) return 0;

    displayList = (coreFoundationObject)dictionary_get(infoDictionary, keys.displays);
    if (!object_is_array(displayList)) {
        log_message("info", "no display list in the answer");
        return 0;
    }
    displayCount = array_count((coreFoundationArray)displayList);
    for (displayIndex = 0; displayIndex < displayCount; displayIndex++) {
        coreFoundationObject entry = array_value_at((coreFoundationArray)displayList, displayIndex);
        if (!object_is_dictionary(entry)) continue;
        if ((int)dictionary_get_integer((coreFoundationDictionary)entry, keys.type, 0) == 111
            && !clusterDisplay)
            clusterDisplay = (mutableCoreFoundationDictionary)entry;
        else if (!mainDisplay)
            mainDisplay = (mutableCoreFoundationDictionary)entry;
    }
    /* nothing is published unless a cluster entry really exists: claiming the
     * feature without the display is what makes the phone refuse the session */
    if (!clusterDisplay) {
        log_message("info", "no cluster display present - publishing nothing");
        return 0;
    }

    if (mainDisplay) nightModeValue = dictionary_get_integer(mainDisplay, keys.nightMode, 0);
    if (mainDisplay && read_flag_setting("MIB3_SHIM_MAINMOD", 1))
        complete_display_entry(mainDisplay, 0, nightModeValue);
    complete_display_entry(clusterDisplay, 1, nightModeValue);

    /* night mode is FOLLOW, never enforce: copy the car's own value object */
    {
        coreFoundationObject carNightValue =
            mainDisplay ? (coreFoundationObject)dictionary_get(mainDisplay, keys.nightMode) : 0;
        if (carNightValue && dictionary_set) {
            dictionary_set(clusterDisplay, keys.nightMode, carNightValue);
            log_message("info", "cluster night mode mirrored from the main display");
        }
    }

    if (read_flag_setting("MIB3_SHIM_PANELS", 1)) {
        build_display_panels(infoDictionary);
    } else if (dictionary_set) {
        dictionary_set(infoDictionary, keys.displayPanels, displayList);
    }

    /* merge, never replace: the head unit builds this key from its own flags */
    {
        mutableCoreFoundationArray features = make_empty_array();
        if (features) {
            coreFoundationObject existing =
                (coreFoundationObject)dictionary_get(infoDictionary, keys.extendedFeatures);
            long existingCount = object_is_array(existing) ? array_count((coreFoundationArray)existing) : 0;
            long existingIndex;

            for (existingIndex = 0; existingIndex < existingCount; existingIndex++) {
                coreFoundationObject feature =
                    array_value_at((coreFoundationArray)existing, existingIndex);
                char featureText[64];
                int alreadyOurs = 0;

                featureText[0] = '\0';
                if (!feature || !read_string_characters) continue;
                if (!read_string_characters((coreFoundationString)feature, featureText,
                                            sizeof(featureText), STRING_ENCODING_UTF8)) continue;
                if (!strcmp(featureText, "altScreen")) alreadyOurs = 1;
                if (!strcmp(featureText, "viewAreas")) alreadyOurs = 1;
                if (!alreadyOurs) array_append(features, feature);
            }
            if (read_flag_setting("MIB3_SHIM_EXTFEAT", 1)) {
                array_append(features, keys.altScreenFeature);
                array_append(features, keys.viewAreasFeature);
            }
            dictionary_set(infoDictionary, keys.extendedFeatures, features);
            if (release_object) release_object(features);
        }
    }

    log_number("info", "displays", (word64)displayCount, 0);
    log_message("info", "dictionary completed");
    return 1;
}

void *AirPlayCopyServerInfo(void *server, void *unusedSecondArgument, void *errorOut)
{
    void *infoDictionary = 0;

    resolve_all_symbols();
    log_message("info", "entered");

    if (the_real_copy_server_info)
        infoDictionary = ((void *(*)(void *, void *, void *))the_real_copy_server_info)(
                            server, unusedSecondArgument, errorOut);

    if (infoDictionary && read_flag_setting("MIB3_SHIM_INFO", 1) && activation_gate_is_open())
        complete_info_dictionary((mutableCoreFoundationDictionary)infoDictionary);

    return infoDictionary;
}

/* ---------------------------------------------------------------------------
 * SECTION 13 - COMMANDS THE CAR SENDS TO THE PHONE
 *
 * The phone sends no video until the car drives the scene, and its encoder
 * emits a recovery point far too rarely for the head unit's decoder, so the
 * keyframe request is on a timer rather than a one-off.
 * ------------------------------------------------------------------------- */

static void *current_command_session = 0;
static unsigned current_command_generation = 0;

/* ---- command acknowledgements --------------------------------------------
 * The completion runs on the phone library's own HTTP thread, AFTER the send
 * returns, and it may outlive the session that issued it. So it touches no
 * CoreFoundation object, takes no lock, and does no I/O: it compares a
 * generation and, for a view-area switch, records the index the phone applied.
 *
 * The context pointer is one of a small ring of process-lifetime slots, so
 * there is nothing to free and no ownership crosses a thread.
 * ---------------------------------------------------------------------- */

#define COMMAND_ANSWER_SLOT_COUNT 8

typedef struct {
    volatile unsigned generation;
    volatile int viewAreaIndex;      /* -1 when the command was not a switch */
    char typeName[24];
} commandAnswerSlot;

static commandAnswerSlot answerSlots[COMMAND_ANSWER_SLOT_COUNT];
static volatile int nextAnswerSlot = 0;
static volatile long acceptedCommandCount = 0;
static volatile long refusedCommandCount = 0;
static volatile long staleAnswerCount = 0;

static commandAnswerSlot *claim_answer_slot(const char *commandType, int viewAreaIndex)
{
    int slotIndex = nextAnswerSlot;
    commandAnswerSlot *slot;
    int characterIndex = 0;

    nextAnswerSlot = (slotIndex + 1) % COMMAND_ANSWER_SLOT_COUNT;
    slot = &answerSlots[slotIndex];
    while (commandType && commandType[characterIndex]
           && characterIndex < (int)sizeof(slot->typeName) - 1) {
        slot->typeName[characterIndex] = commandType[characterIndex];
        characterIndex++;
    }
    slot->typeName[characterIndex] = '\0';
    slot->viewAreaIndex = viewAreaIndex;
    slot->generation = current_command_generation;
    return slot;
}

static void command_acknowledged(int status, void *response, void *context)
{
    commandAnswerSlot *slot = (commandAnswerSlot *)context;

    (void)response;             /* deliberately untouched: another thread owns it */
    if (!slot) return;

    /* a reply from a session that has been replaced must be dropped, never
     * applied: acting on it would command a session that no longer exists */
    if (slot->generation != current_command_generation) {
        staleAnswerCount++;
        log_number("command-ack/stale", "status", (word64)(word32)status, 0);
        log_message("command-ack/stale/type", slot->typeName);
        return;
    }
    if (status == 0) acceptedCommandCount++;
    else             refusedCommandCount++;

    log_number("command-ack", "status (0 = accepted)", (word64)(word32)status, 0);
    log_message("command-ack/type", slot->typeName);

    if (slot->viewAreaIndex >= 0) {
        if (status == 0) {
            /* only now do we believe the display actually moved */
            current_view_area_index = slot->viewAreaIndex;
            log_number("view-areas", "applied index",
                       (word64)(word32)slot->viewAreaIndex, 0);
        } else {
            /* refused: the index is left alone, so a refusal cannot strand the
             * display in a state we merely believe we are in */
            log_number("view-areas", "refused index",
                       (word64)(word32)slot->viewAreaIndex, 0);
        }
    }
}

static mutableCoreFoundationDictionary make_parameters_with_display_identifier(void)
{
    mutableCoreFoundationDictionary parameters;

    if (!create_dictionary || !dictionary_key_callbacks || !dictionary_value_callbacks) return 0;
    parameters = create_dictionary(0, 0, dictionary_key_callbacks, dictionary_value_callbacks);
    if (!parameters) return 0;
    if (cluster_display_identifier[0])
        dictionary_put_text(parameters, "uuid", cluster_display_identifier);
    return parameters;
}

static int send_command_with_acknowledgement(void *session, const char *commandType,
                                            mutableCoreFoundationDictionary parameters,
                                            int viewAreaIndex)
{
    mutableCoreFoundationDictionary command;
    int result = -1;

    if (!session || !send_command_to_phone || !create_dictionary) return -1;

    /* only the current setup may command: an older receiver thread must not
     * reach a session object that has already been torn down */
    if (session != current_command_session) {
        log_message("commands", "skipped: superseded session");
        log_number("commands", "current generation",
                   (word64)current_command_generation, 0);
        return -2;
    }

    command = create_dictionary(0, 0, dictionary_key_callbacks, dictionary_value_callbacks);
    if (!command) return -1;
    {
        coreFoundationString typeKey   = make_string("type");
        coreFoundationString paramsKey = make_string("params");
        coreFoundationString typeValue = make_string(commandType);

        if (typeKey && paramsKey && typeValue && parameters) {
            dictionary_set(command, typeKey, typeValue);
            dictionary_set(command, paramsKey, parameters);
            if (read_flag_setting("MIB3_SHIM_CMDACK", 1)) {
                result = send_command_to_phone(session, command,
                                               (void *)command_acknowledged,
                                               claim_answer_slot(commandType, viewAreaIndex));
            } else {
                result = send_command_to_phone(session, command, 0, 0);
            }
        }
        if (release_object) {
            if (typeValue) release_object(typeValue);
            if (paramsKey) release_object(paramsKey);
            if (typeKey) release_object(typeKey);
        }
    }
    if (release_object) release_object(command);   /* the caller releases after sending */
    return result;
}

/* send without tracking the phone's verdict */
static int send_command(void *session, const char *commandType,
                        mutableCoreFoundationDictionary parameters)
{
    return send_command_with_acknowledgement(session, commandType, parameters, -1);
}

static void command_show_scene(void *session, const char *reason)
{
    mutableCoreFoundationDictionary parameters;

    if (!read_flag_setting("MIB3_SHIM_CMDS", 1) || !session) return;
    if (!cluster_display_identifier[0]) return;

    parameters = make_parameters_with_display_identifier();
    if (!parameters) return;
    dictionary_put_text(parameters, "url",
                        cluster_scene_address[0] ? cluster_scene_address : DEFAULT_CLUSTER_SCENE);
    log_number("commands", "show scene result", (word64)(word32)send_command(
                   session, "showUI", parameters), 0);
    log_message("commands/show-scene-reason", reason ? reason : "?");
    if (release_object) release_object(parameters);
}

static void command_request_keyframe(void *session, const char *reason)
{
    mutableCoreFoundationDictionary parameters;

    if (!read_flag_setting("MIB3_SHIM_CMDS", 1) || !session) return;
    if (!cluster_display_identifier[0]) return;

    parameters = make_parameters_with_display_identifier();
    if (!parameters) return;
    log_number("commands", "keyframe request shipped",
               (word64)(word32)send_command(session, "forceKeyFrame", parameters), 0);
    log_message("commands/keyframe-reason", reason ? reason : "?");
    if (release_object) release_object(parameters);
}

static void command_set_night_mode(void *session)
{
    mutableCoreFoundationDictionary parameters;
    signedWord nightModeValue;

    /* nothing is sent unless the debug override exists: the phone decides */
    if (!read_flag_setting("MIB3_SHIM_CMDS", 1) || !session) return;
    if (!getenv("MIB3_SHIM_NIGHT")) return;
    if (!cluster_display_identifier[0]) return;

    parameters = make_parameters_with_display_identifier();
    if (!parameters) return;
    nightModeValue = dictionary_read_integer(
        (coreFoundationDictionary)current_command_session, "nightMode", 0);
    dictionary_put_integer(parameters, "nightMode", nightModeValue ? 1 : 0);
    log_number("commands", "set night mode result",
               (word64)(word32)send_command(session, "setNightMode", parameters), 0);
    if (release_object) release_object(parameters);
}

static void command_switch_view_area(void *session, int areaIndex, const char *reason)
{
    mutableCoreFoundationDictionary parameters;
    mutableCoreFoundationArray adjacent;

    if (!read_flag_setting("MIB3_SHIM_CMDS", 1) || !session) return;
    if (!read_flag_setting("MIB3_SHIM_VIEWAREAS", 0)) return;
    if (!read_flag_setting("MIB3_SHIM_VA_CMD", 1)) return;
    if (areaIndex < 0 || areaIndex >= declared_view_area_count()) return;

    parameters = make_parameters_with_display_identifier();
    if (!parameters) return;
    dictionary_put_integer(parameters, "viewAreaIndex", (signedWord)areaIndex);
    dictionary_put_integer(parameters, "animationDurationMillis", 250);

    adjacent = make_empty_array();
    if (adjacent) {
        /* the phone keeps this separately from the current area: an empty array
         * and an absent key mean different things */
        if (areaIndex != 0 && array_append_integer) array_append_integer(adjacent, 0);
        dictionary_put_object(parameters, "adjacentViewAreas", adjacent);
        if (release_object) release_object(adjacent);
    }
    log_number("view-areas", "switch requested", (word64)(word32)areaIndex, 0);
    log_number("view-areas", "switch result",
               (word64)(word32)send_command_with_acknowledgement(
                   session, "updateViewArea", parameters, areaIndex), 0);
    log_message("view-areas/switch-reason", reason ? reason : "?");
    if (release_object) release_object(parameters);
}

static void command_zoom(void *session, int zoomingIn)
{
    mutableCoreFoundationDictionary parameters;

    if (!read_flag_setting("MIB3_SHIM_CMDS", 1) || !session) return;
    if (!cluster_display_identifier[0]) return;

    parameters = make_parameters_with_display_identifier();
    if (!parameters) return;
    /* zero means in, anything else means out, as the phone encodes it */
    dictionary_put_integer(parameters, "zoomDirection", zoomingIn ? 0 : 1);
    log_number("zoom", "zoom result",
               (word64)(word32)send_command(session, "changeMapZoomLevel", parameters), 0);
    if (release_object) release_object(parameters);
}

static void command_set_appearance(void *session, int isMap,
                                   int appearanceMode, int appearanceSetting)
{
    mutableCoreFoundationDictionary parameters;

    if (!read_flag_setting("MIB3_SHIM_CMDS", 1) || !session) return;
    if (!read_flag_setting("MIB3_SHIM_APPEARANCE", 0)) return;

    parameters = make_parameters_with_display_identifier();
    if (!parameters) return;
    dictionary_put_integer(parameters, "appearanceMode", (signedWord)appearanceMode);
    dictionary_put_integer(parameters, "appearanceSetting", (signedWord)appearanceSetting);
    log_number("appearance", "appearance result",
               (word64)(word32)send_command(
                   session, isMap ? "mapAppearanceUpdate" : "uiAppearanceUpdate", parameters), 0);
    if (release_object) release_object(parameters);
}

/* ---------------------------------------------------------------------------
 * SECTION 14 - ONE QUEUE, THREE USES
 *
 * The listener thread only ever QUEUES. The receiver thread, which owns a valid
 * session for the length of the stream, drains and sends. That rule is what
 * keeps the listener from ever touching a session that has been torn down.
 * ------------------------------------------------------------------------- */

#define QUEUE_CAPACITY 16

typedef struct {
    volatile int entries[QUEUE_CAPACITY];
    volatile int writeIndex;
    volatile int readIndex;
    volatile int droppedCount;
} workQueue;

static workQueue zoomStepQueue;
static workQueue viewAreaQueue;
static workQueue appearanceQueue;

static int queue_push(workQueue *queue, int value)
{
    int nextWrite = (queue->writeIndex + 1) % QUEUE_CAPACITY;
    if (nextWrite == queue->readIndex) {
        queue->droppedCount++;
        return 0;
    }
    queue->entries[queue->writeIndex] = value;
    queue->writeIndex = nextWrite;
    return 1;
}

static int queue_pop(workQueue *queue, int *valueOut)
{
    if (queue->readIndex == queue->writeIndex) return 0;
    *valueOut = queue->entries[queue->readIndex];
    queue->readIndex = (queue->readIndex + 1) % QUEUE_CAPACITY;
    return 1;
}

#define VIEW_AREA_REQUEST_NEXT (-2)
#define VIEW_AREA_REQUEST_PREVIOUS (-3)

/* one line grammar: "+1" / "-1" zoom, "v2" / "next" / "prev" view area,
 * "a map 1 0" appearance. Anything unrecognised is logged and ignored. */
static int token_equals(const char *text, int length, const char *expected)
{
    int index;
    for (index = 0; index < length; index++) {
        char actual = text[index];
        char wanted = expected[index];
        if (!wanted) return 0;
        if (actual >= 'A' && actual <= 'Z') actual = (char)(actual - 'A' + 'a');
        if (wanted >= 'A' && wanted <= 'Z') wanted = (char)(wanted - 'A' + 'a');
        if (actual != wanted) return 0;
    }
    return expected[length] == '\0';
}

static void handle_control_line(const char *text, int length)
{
    int index = 0;

    if (length <= 0) return;

    if (text[0] == 'a' || text[0] == 'A') {
        int isMap = 0;
        int values[2] = { -1, -1 };
        int valueCount = 0;

        index = 1;
        while (index < length && (text[index] == ' ' || text[index] == '\t')) index++;
        if (index < length && (text[index] == 'm' || text[index] == 'M')) {
            isMap = 1;
            while (index < length && text[index] != ' ' && text[index] != '\t') index++;
        } else if (index < length && (text[index] == 'u' || text[index] == 'U')) {
            while (index < length && text[index] != ' ' && text[index] != '\t') index++;
        }
        while (index < length && valueCount < 2) {
            while (index < length && (text[index] == ' ' || text[index] == '\t'
                                      || text[index] == ',')) index++;
            if (index >= length) break;
            if (text[index] < '0' || text[index] > '9') { index++; continue; }
            {
                int parsed = 0;
                while (index < length && text[index] >= '0' && text[index] <= '9') {
                    parsed = parsed * 10 + (text[index] - '0');
                    index++;
                }
                values[valueCount++] = parsed;
            }
        }
        if (valueCount == 0) { log_message("appearance", "no mode or setting - ignored"); return; }
        if (values[1] < 0) values[1] = 0;
        queue_push(&appearanceQueue, ((isMap & 0xff) << 24) | ((values[0] & 0xfff) << 12)
                                     | (values[1] & 0xfff));
        return;
    }

    if (text[0] == 'v' || text[0] == 'V') {
        int areaIndex = 0;
        int sawAnyDigit = 0;
        for (index = 1; index < length; index++) {
            if (text[index] >= '0' && text[index] <= '9') {
                areaIndex = areaIndex * 10 + (text[index] - '0');
                sawAnyDigit = 1;
            }
        }
        if (sawAnyDigit) queue_push(&viewAreaQueue, areaIndex);
        else log_message("view-areas", "no index given - ignored");
        return;
    }
    if (token_equals(text, length, "next")) {
        queue_push(&viewAreaQueue, VIEW_AREA_REQUEST_NEXT);
        return;
    }
    if (token_equals(text, length, "prev")) {
        queue_push(&viewAreaQueue, VIEW_AREA_REQUEST_PREVIOUS);
        return;
    }

    {
        int direction = 0;
        int sign = 0;
        for (; index < length; index++) {
            char character = text[index];
            if (character == ' ' || character == '\t') continue;
            if (character == 'i' || character == 'I') { direction = 1;  continue; }
            if (character == 'o' || character == 'O') { direction = -1; continue; }
            if (character == '+') { if (!sign) sign = 1;  continue; }
            if (character == '-') { sign = -1; continue; }
            if (character >= '0' && character <= '9') { if (!sign && !direction) sign = 1; continue; }
        }
        if (read_flag_setting("MIB3_SHIM_ZOOM_INVERT", 0)) { direction = -direction; sign = -sign; }
        if (direction) queue_push(&zoomStepQueue, direction);
        else if (sign) queue_push(&zoomStepQueue, sign);
    }
}

static void *control_socket_thread(void *argument)
{
    const char *socketPath = (const char *)argument;
    struct { unsigned short family; char path[108]; } address;
    struct { int descriptor; char line[64]; int length; } clients[8];
    int listener;
    int clientIndex;

    log_message("control-socket", "starting");
    unlink(socketPath);
    listener = socket(SOCKET_DOMAIN_UNIX, SOCKET_STREAM, 0);
    if (listener < 0) { log_message("control-socket", "socket() failed"); return 0; }

    memset(&address, 0, sizeof(address));
    address.family = ADDRESS_FAMILY_UNIX;
    {
        unsigned long length = 0;
        while (socketPath[length] && length < sizeof(address.path) - 1) {
            address.path[length] = socketPath[length];
            length++;
        }
    }
    if (bind(listener, &address, (unsigned int)sizeof(address)) != 0) {
        log_message("control-socket", "bind failed");
        close(listener);
        return 0;
    }
    if (listen(listener, 4) != 0) {
        log_message("control-socket", "listen failed");
        close(listener);
        return 0;
    }
    log_message("control-socket/ready", socketPath);

    for (clientIndex = 0; clientIndex < 8; clientIndex++) {
        clients[clientIndex].descriptor = -1;
        clients[clientIndex].length = 0;
    }

    for (;;) {
        struct { int descriptor; short events; short returnedEvents; } polled[9];
        int polledCount = 1;
        int clientOf[9];

        polled[0].descriptor = listener;
        polled[0].events = POLL_READABLE;
        polled[0].returnedEvents = 0;
        clientOf[0] = -1;
        for (clientIndex = 0; clientIndex < 8; clientIndex++) {
            if (clients[clientIndex].descriptor >= 0) {
                polled[polledCount].descriptor = clients[clientIndex].descriptor;
                polled[polledCount].events = POLL_READABLE;
                polled[polledCount].returnedEvents = 0;
                clientOf[polledCount] = clientIndex;
                polledCount++;
            }
        }
        if (poll(polled, (unsigned long)polledCount, -1) < 0) continue;

        if (polled[0].returnedEvents & POLL_READABLE) {
            int accepted = accept(listener, 0, 0);
            if (accepted >= 0) {
                int freeSlot = -1;
                for (clientIndex = 0; clientIndex < 8; clientIndex++)
                    if (clients[clientIndex].descriptor < 0) { freeSlot = clientIndex; break; }
                if (freeSlot >= 0) {
                    clients[freeSlot].descriptor = accepted;
                    clients[freeSlot].length = 0;
                } else {
                    close(accepted);
                }
            }
        }

        for (clientIndex = 1; clientIndex < polledCount; clientIndex++) {
            int slot = clientOf[clientIndex];
            char incoming[64];
            long received;
            long byteIndex;

            if (!(polled[clientIndex].returnedEvents & POLL_READABLE)) continue;
            if (clients[slot].descriptor < 0) continue;

            received = read(clients[slot].descriptor, incoming, sizeof(incoming));
            if (received <= 0) {
                /* a line that never got its terminator still counts, exactly once */
                if (clients[slot].length > 0)
                    handle_control_line(clients[slot].line, clients[slot].length);
                close(clients[slot].descriptor);
                clients[slot].descriptor = -1;
                clients[slot].length = 0;
                continue;
            }
            for (byteIndex = 0; byteIndex < received; byteIndex++) {
                char character = incoming[byteIndex];
                if (character == '\n' || character == '\r') {
                    if (clients[slot].length > 0) {
                        handle_control_line(clients[slot].line, clients[slot].length);
                        clients[slot].length = 0;
                    }
                    continue;
                }
                if (clients[slot].length < (int)sizeof(clients[slot].line) - 1) {
                    clients[slot].line[clients[slot].length++] = character;
                } else {
                    handle_control_line(clients[slot].line, clients[slot].length);
                    clients[slot].length = 0;
                }
            }
        }
    }
    return 0;
}

static int control_socket_has_started = 0;

static void start_control_socket(void)
{
    static char socketPath[128];
    const char *configuredPath;
    void *thread = 0;

    if (control_socket_has_started) return;
    if (!read_flag_setting("MIB3_SHIM_ZOOM", 1)) return;

    configuredPath = getenv("MIB3_SHIM_ZOOM_SOCK");
    if (!configuredPath || !configuredPath[0]) configuredPath = "/dev/shm/mib3zoom.sock";
    {
        unsigned long length = 0;
        while (configuredPath[length] && length < sizeof(socketPath) - 1) {
            socketPath[length] = configuredPath[length];
            length++;
        }
        socketPath[length] = '\0';
    }
    if (!create_thread || create_thread(&thread, 0, control_socket_thread, socketPath) != 0) {
        log_message("control-socket", "thread creation failed");
        return;
    }
    control_socket_has_started = 1;
}

static void drain_queues(void *session)
{
    int value;

    while (queue_pop(&zoomStepQueue, &value))    command_zoom(session, value > 0);
    while (queue_pop(&viewAreaQueue, &value)) {
        int areaIndex = value;
        if (value == VIEW_AREA_REQUEST_NEXT)
            areaIndex = (current_view_area_index + 1) % declared_view_area_count();
        else if (value == VIEW_AREA_REQUEST_PREVIOUS)
            areaIndex = (current_view_area_index + declared_view_area_count() - 1)
                        % declared_view_area_count();
        command_switch_view_area(session, areaIndex, "control socket");
    }
    while (queue_pop(&appearanceQueue, &value)) {
        command_set_appearance(session, (value >> 24) & 0xff,
                               (value >> 12) & 0xfff, value & 0xfff);
    }
}

/* ---------------------------------------------------------------------------
 * SECTION 15 - FRAME DELIVERY TO THE SEPARATE PROCESS
 *
 * carplay performs no display call at all. Each record is a 16-byte header and
 * the frame. A full send buffer means WAIT, not drop: the phone's stream has
 * about one recovery point per minute, so a discarded frame breaks the picture
 * until the next one.
 * ------------------------------------------------------------------------- */

#define DELIVERY_MAGIC        0x4d334331u
#define DELIVERY_HEADER_BYTES 16u
#define DELIVERY_DEFAULT_PATH "/dev/shm/mib3cluster.sock"
#define DELIVERY_MAX_FRAME    (8u << 20)
#define DELIVERY_SEND_BUFFER  (4u << 20)

/* MIB3_SHIM_SINK names where frames go. up.sh and the supervisor both set
 * "helper"; "off" (or 0) disables delivery without touching anything else, and
 * anything else is refused loudly rather than silently behaving as off. The
 * default is the consumer, because it is the only delivery path that exists and
 * a silent no-op is the harder failure to diagnose. */
#define DELIVERY_MODE_DISABLED  0
#define DELIVERY_MODE_CONSUMER  1

static int delivery_mode = -1;

static void resolve_delivery_mode(void)
{
    char *configured = getenv("MIB3_SHIM_SINK");

    if (delivery_mode >= 0) return;
    delivery_mode = DELIVERY_MODE_CONSUMER;

    if (configured && configured[0]) {
        if (configured[0] == 'o' || configured[0] == '0') {
            delivery_mode = DELIVERY_MODE_DISABLED;
        } else if (configured[0] == 'h') {
            delivery_mode = DELIVERY_MODE_CONSUMER;
        } else {
            log_message("delivery", "MIB3_SHIM_SINK names a mode that no longer exists "
                                    "(only off or helper) - staying off");
            delivery_mode = DELIVERY_MODE_DISABLED;
        }
    }
    log_number("delivery", "mode (0 = off, 1 = consumer)", (word64)(word32)delivery_mode, 0);
}

static int delivery_is_enabled(void)
{
    resolve_delivery_mode();
    return delivery_mode == DELIVERY_MODE_CONSUMER;
}

static int delivery_descriptor = -1;
static frameSequence delivery_sequence = 0;
static unsigned long delivery_sent_count = 0;
static unsigned long delivery_dropped_count = 0;
static word64 delivery_last_success_milliseconds = 0;
static int delivery_consecutive_would_block = 0;

static long delivery_wait_milliseconds(void)
{
    long configured = (long)read_number_setting("MIB3_SHIM_HLP_WAIT", 60);
    return configured > 0 ? configured : 60;
}

static long delivery_stall_milliseconds(void)
{
    long configured = (long)read_number_setting("MIB3_SHIM_HLP_STALL_MS", 600);
    return configured > 0 ? configured : 600;
}

static void close_delivery_socket(void)
{
    if (delivery_descriptor >= 0) close(delivery_descriptor);
    delivery_descriptor = -1;
    delivery_consecutive_would_block = 0;
}

static int connect_delivery_socket(void)
{
    struct { unsigned short family; char path[108]; } address;
    const char *path = getenv("MIB3_SHIM_HELPER");
    int descriptor;
    int index;

    if (!delivery_is_enabled()) return -1;
    if (delivery_descriptor >= 0) return 0;
    if (!path || !path[0]) path = DELIVERY_DEFAULT_PATH;

    memset(&address, 0, sizeof(address));
    for (index = 0; index < 107 && path[index]; index++) address.path[index] = path[index];
    address.family = ADDRESS_FAMILY_UNIX;

    descriptor = socket(SOCKET_DOMAIN_UNIX, SOCKET_STREAM, 0);
    if (descriptor < 0) {
        log_number("delivery", "socket error", (word64)(word32)ERROR_NUMBER, 0);
        return -1;
    }
    if (connect(descriptor, &address, sizeof(address)) != 0) {
        log_number("delivery", "connect error", (word64)(word32)ERROR_NUMBER, 0);
        close(descriptor);
        return -1;
    }

    /* the default buffer holds barely two frames; a decoder that pauses for a
     * hundred milliseconds would then lose one, so ask for more and verify */
    {
        int requested = (int)DELIVERY_SEND_BUFFER;
        int granted = 0;
        unsigned int grantedLength = (unsigned int)sizeof(granted);
        setsockopt(descriptor, SOCKET_LEVEL_BASE, SOCKET_OPTION_SEND_BUFFER,
                   &requested, sizeof(requested));
        if (getsockopt(descriptor, SOCKET_LEVEL_BASE, SOCKET_OPTION_SEND_BUFFER,
                       &granted, &grantedLength) == 0) {
            log_number("delivery", "send buffer granted", (word64)(word32)granted, 0);
            if (granted < 425984)
                log_message("delivery", "send buffer clamped - raise wmem_max or frames will drop");
        }
    }

    delivery_descriptor = descriptor;
    memset(&address, 0, sizeof(address));
    log_message("delivery", "connected to the frame consumer");
    return 0;
}

/* Retried from the tick that already exists, so a consumer that starts while
 * the map is static is still picked up. */
static void reconnect_delivery_socket_periodically(void)
{
    static long lastAttemptSecond = -1;
    long now = time(0);

    if (!delivery_is_enabled()) return;
    if (delivery_descriptor >= 0) return;
    if (now == lastAttemptSecond) return;
    lastAttemptSecond = now;
    (void)connect_delivery_socket();
}

static long write_all_bytes(int descriptor, const void *data, unsigned long length,
                           int waitMilliseconds)
{
    const unsigned char *bytes = (const unsigned char *)data;
    unsigned long written = 0;
    word64 deadline = monotonic_milliseconds() + (word64)waitMilliseconds;
    int spinCount = 0;

    while (written < length) {
        long result = send(descriptor, bytes + written, length - written,
                           SEND_FLAG_NO_SIGNAL | SEND_FLAG_DO_NOT_WAIT);
        if (result > 0) { written += (unsigned long)result; continue; }
        if (result < 0 && error_is_interrupted()) continue;
        if (result < 0 && error_would_block()) {
            struct { int descriptor; short events; short returnedEvents; } waiting;
            if (monotonic_milliseconds() >= deadline) return -1;
            if (++spinCount > 100) return -1;      /* survives a broken clock */
            waiting.descriptor = descriptor;
            waiting.events = POLL_WRITABLE;
            waiting.returnedEvents = 0;
            poll(&waiting, 1, 20);
            continue;
        }
        return -1;
    }
    return (long)length;
}

static int send_record(const void *data, unsigned long length)
{
    unsigned char header[DELIVERY_HEADER_BYTES];
    word32 headerPart;
    word64 sequencePart;
    int index;

    if (!delivery_is_enabled()) return 0;
    if (!data || !length || length > DELIVERY_MAX_FRAME) return -1;

    if (delivery_descriptor < 0) {
        static long lastConnectSecond = 0;
        long now = time(0);
        if (now != lastConnectSecond) {
            lastConnectSecond = now;
            if (connect_delivery_socket() != 0) { delivery_dropped_count++; return -1; }
        } else {
            delivery_dropped_count++;
            return -1;
        }
    }

    headerPart = DELIVERY_MAGIC;
    for (index = 0; index < 4; index++)
        header[index] = (unsigned char)((headerPart >> (index * 8)) & 0xff);
    headerPart = (word32)length;
    for (index = 0; index < 4; index++)
        header[4 + index] = (unsigned char)((headerPart >> (index * 8)) & 0xff);
    sequencePart = ++delivery_sequence;
    for (index = 0; index < 8; index++)
        header[8 + index] = (unsigned char)((sequencePart >> (index * 8)) & 0xff);

    if (write_all_bytes(delivery_descriptor, header, DELIVERY_HEADER_BYTES,
                        (int)delivery_wait_milliseconds()) != (long)DELIVERY_HEADER_BYTES) {
        if (error_would_block()) {
            delivery_consecutive_would_block++;
            if (delivery_consecutive_would_block >= 32
                || (delivery_last_success_milliseconds
                    && monotonic_milliseconds() - delivery_last_success_milliseconds
                       > (word64)delivery_stall_milliseconds())) {
                close_delivery_socket();
            }
        } else {
            close_delivery_socket();
        }
        delivery_dropped_count++;
        return -1;
    }

    /* a partial body leaves the reader in an unknown position, so the only
     * recovery is to break the connection and start again */
    if (write_all_bytes(delivery_descriptor, data, length,
                        (int)delivery_wait_milliseconds()) != (long)length) {
        close_delivery_socket();
        delivery_dropped_count++;
        return -1;
    }

    delivery_consecutive_would_block = 0;
    delivery_last_success_milliseconds = monotonic_milliseconds();
    delivery_sent_count++;
    return 0;
}

/* ---------------------------------------------------------------------------
 * SECTION 16 - VIDEO FRAMING
 *
 * The phone's access units arrive length-prefixed; the decoder wants start
 * codes. The parameter sets arrive out of band, so they are stamped onto every
 * frame to keep each one self-contained.
 * ------------------------------------------------------------------------- */

static unsigned char parameter_sets_annexb[4096];
static unsigned long parameter_sets_length = 0;
static unsigned char nal_length_size = 4;

static unsigned extract_parameter_sets(const unsigned char *configuration,
                                      unsigned configurationLength,
                                      unsigned char *output, unsigned outputCapacity,
                                      unsigned *outputLength)
{
    unsigned cursor = 6;
    unsigned written = 0;
    unsigned groupIndex;
    unsigned setCount;
    unsigned char lengthSize;

    *outputLength = 0;
    if (!configuration || !output || configurationLength < 7) return 0;
    if (configuration[0] != 1) return 0;

    lengthSize = (unsigned char)((configuration[4] & 3) + 1);
    setCount = configuration[5] & 31;

    for (groupIndex = 0; groupIndex < 2; groupIndex++) {
        unsigned setIndex;
        if (!setCount) return 0;
        for (setIndex = 0; setIndex < setCount; setIndex++) {
            unsigned setSize;
            if (configurationLength - cursor < 2) return 0;
            setSize = ((unsigned)configuration[cursor] << 8) | configuration[cursor + 1];
            cursor += 2;
            if (!setSize || setSize > configurationLength - cursor) return 0;
            if (outputCapacity - written < 4 || setSize > outputCapacity - written - 4) return 0;
            output[written] = 0; output[written + 1] = 0;
            output[written + 2] = 0; output[written + 3] = 1;
            memcpy(output + written + 4, configuration + cursor, setSize);
            written += 4 + setSize;
            cursor += setSize;
        }
        if (groupIndex == 0) {
            if (cursor == configurationLength) return 0;
            setCount = configuration[cursor++];
        }
    }
    *outputLength = written;
    return lengthSize;
}

/* Length prefixes become start codes in place. Reports whether the unit starts
 * with a recovery point, which decides whether the decoder can begin here. */
static int convert_access_unit(unsigned char *data, unsigned long length,
                              unsigned char lengthSize)
{
    unsigned long cursor = 0;

    if (!data || !length || length > DELIVERY_MAX_FRAME) return -1;
    if (lengthSize < 1 || lengthSize > 4) return -1;

    while (length - cursor >= lengthSize) {
        unsigned long nalLength = 0;
        unsigned long prefix = cursor;
        int index;

        for (index = 0; index < (int)lengthSize; index++)
            nalLength = (nalLength << 8) | data[cursor + index];
        cursor += lengthSize;
        if (!nalLength || nalLength > length - cursor) return -1;
        if (lengthSize == 4) {
            data[prefix] = 0; data[prefix + 1] = 0;
            data[prefix + 2] = 0; data[prefix + 3] = 1;
        }
        cursor += nalLength;
    }
    if (cursor != length) return -1;
    return 0;
}

static int access_unit_starts_with_recovery_point(const unsigned char *data,
                                                 unsigned long length,
                                                 unsigned char lengthSize)
{
    unsigned long cursor = 0;

    while (length - cursor >= lengthSize) {
        unsigned long nalLength = 0;
        int index;
        for (index = 0; index < (int)lengthSize; index++)
            nalLength = (nalLength << 8) | data[cursor + index];
        cursor += lengthSize;
        if (!nalLength || nalLength > length - cursor) return 0;
        if ((data[cursor] & 31) == 5) return 1;
        cursor += nalLength;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * SECTION 17 - THE DATA LISTENER
 *
 * The car's own firewall only admits a few ports on the phone-facing
 * interface, so the chain below is a discovered constraint, not a preference.
 * ------------------------------------------------------------------------- */

static int data_listener_descriptor = -1;
static volatile unsigned data_listener_generation = 0;

static int open_data_listener(int *portOut, int *generationOut)
{
    struct { unsigned short family; unsigned short port;
             unsigned int flowInformation; unsigned char address[16];
             unsigned int scope; } address, boundAddress;
    unsigned long portChain[4];
    int chainLength = 0;
    int descriptor;
    int reuseAddress = 1;
    unsigned int boundLength = sizeof(boundAddress);
    int chainIndex;
    int didBind = 0;

    {
        char *configuredPort = getenv("MIB3_SHIM_PORT");
        unsigned long portValue = 0;
        if (configuredPort && configuredPort[0] >= '0' && configuredPort[0] <= '9') {
            while (*configuredPort >= '0' && *configuredPort <= '9') {
                portValue = portValue * 10 + (unsigned long)(*configuredPort - '0');
                configuredPort++;
            }
        }
        if (portValue) portChain[chainLength++] = portValue;
    }
    portChain[chainLength++] = 5000;   /* the only firewall-legal free port */
    portChain[chainLength++] = 6031;   /* native alternative, blocked without help */
    portChain[chainLength++] = 0;      /* ephemeral, also blocked */

    /* a listener from an earlier setup holds the port and must go first */
    if (data_listener_descriptor >= 0) {
        close(data_listener_descriptor);
        data_listener_descriptor = -1;
        data_listener_generation++;
    }

    descriptor = socket(SOCKET_DOMAIN_INET6, SOCKET_STREAM, 0);
    if (descriptor < 0) return -1;

    setsockopt(descriptor, SOCKET_LEVEL_BASE, SOCKET_OPTION_REUSE_ADDRESS,
               &reuseAddress, sizeof(reuseAddress));
    {
        int acceptOnlyIPv4 = 0;
        setsockopt(descriptor, SOCKET_LEVEL_IPV6, SOCKET_OPTION_IPV6_ONLY_V6,
                   &acceptOnlyIPv4, sizeof(acceptOnlyIPv4));
    }

    memset(&address, 0, sizeof(address));
    address.family = ADDRESS_FAMILY_INET6;
    for (chainIndex = 0; chainIndex < chainLength && !didBind; chainIndex++) {
        unsigned long port = portChain[chainIndex];
        address.port = (unsigned short)((port << 8) | (port >> 8));   /* network order */
        if (bind(descriptor, &address, sizeof(address)) == 0) didBind = 1;
        log_number("listener", "tried port", (word64)port, 0);
    }
    if (!didBind) { close(descriptor); return -1; }
    if (listen(descriptor, 8) != 0) { close(descriptor); return -1; }

    data_listener_descriptor = descriptor;
    data_listener_generation++;
    if (generationOut) *generationOut = (int)data_listener_generation;
    *portOut = 0;
    if (getsockname(descriptor, &boundAddress, &boundLength) == 0) {
        *portOut = (int)(((boundAddress.port & 0xff) << 8) | (boundAddress.port >> 8));
        log_number("listener", "bound port", (word64)(word32)(*portOut), 0);
        if (*portOut == 5000 || *portOut == 5001)
            log_message("listener", "port is allowed by the car firewall");
        else
            log_message("listener", "port is blocked by the car firewall - needs a rule");
    }
    return descriptor;
}

/* ---------------------------------------------------------------------------
 * SECTION 18 - THE RECEIVER
 *
 * One setup, one thread, one context. Nothing here reads shared mutable state a
 * later setup could overwrite, which is what made the older design corrupt
 * itself across reconnects.
 * ------------------------------------------------------------------------- */

typedef struct {
    int listenerDescriptor;
    int listenerGeneration;
    void *session;
    unsigned char key[32];
    int cipherKind;                  /* 0 none, 1 aes counter, 2 chacha */
    signedWord frameCounter;
    int aesHasBeenInitialised;
} receiverContext;

static volatile unsigned newest_receiver_generation = 0;

#define CIPHER_NONE        0
#define CIPHER_AES_COUNTER 1
#define CIPHER_CHACHA      2

static word64 last_frame_milliseconds = 0;
static word64 last_recovery_point_milliseconds = 0;
static word64 last_request_milliseconds = 0;
static word64 delayed_activation_milliseconds = 0;
static int frame_flow_has_paused = 0;

static int wait_for_frame_or_run_pump(int descriptor, void *session, int waitMilliseconds)
{
    struct { int descriptor; short events; short returnedEvents; } waiting;
    int ready;

    waiting.descriptor = descriptor;
    waiting.events = POLL_READABLE;
    waiting.returnedEvents = 0;
    ready = poll(&waiting, 1, waitMilliseconds);

    reconnect_delivery_socket_periodically();

    if (delayed_activation_milliseconds && monotonic_milliseconds() >= delayed_activation_milliseconds) {
        delayed_activation_milliseconds = 0;
        command_show_scene(session, "delayed activation");
    }
    if (last_recovery_point_milliseconds) {
        int interval = (int)read_number_setting("MIB3_SHIM_IDR_MS", 500);
        word64 now = monotonic_milliseconds();
        int isQuiet = last_frame_milliseconds && (now - last_frame_milliseconds) > 2000;
        if (interval > 0 && !isQuiet
            && (now - last_recovery_point_milliseconds) >= (word64)interval
            && (!last_request_milliseconds || (now - last_request_milliseconds) >= (word64)interval)) {
            last_request_milliseconds = now;
            command_request_keyframe(session, "periodic recovery point");
        }
    }
    return ready > 0;
}

static void *receiver_thread(void *argument)
{
    receiverContext *context = (receiverContext *)argument;
    unsigned myGeneration = ++newest_receiver_generation;
    int connectionDescriptor = -1;
    unsigned char *bodyBuffer = 0;
    unsigned long bodyCapacity = 0;
    unsigned char cipherScratch[512];
    long frameCount = 0;
    long recoveryPointCount = 0;
    long otherOpcodeCount = 0;
    int firstRecoveryPointSeen = 0;
    int shouldStop = 0;

    if (!context) return 0;
    log_message("receiver", "waiting for the phone");

    /* a blocking accept with no timeout once held the port for the life of the
     * process, so this ticks and gives up the moment a newer setup arrives */
    while (context->listenerDescriptor >= 0 && connectionDescriptor < 0) {
        struct { int descriptor; short events; short returnedEvents; } waiting;
        int ready;

        waiting.descriptor = context->listenerDescriptor;
        waiting.events = POLL_READABLE;
        waiting.returnedEvents = 0;
        ready = poll(&waiting, 1, 250);
        if ((int)data_listener_generation != context->listenerGeneration) {
            log_message("receiver", "listener superseded - abandoning it");
            break;
        }
        if (ready < 0) continue;
        if (ready > 0 && (waiting.returnedEvents & POLL_READABLE))
            connectionDescriptor = accept(context->listenerDescriptor, 0, 0);
        if (ready > 0 && (waiting.returnedEvents & ~POLL_READABLE) && connectionDescriptor < 0)
            break;
    }

    if (connectionDescriptor < 0) {
        log_message("receiver", "no connection");
        return 0;
    }

    close(context->listenerDescriptor);
    if ((int)data_listener_generation == context->listenerGeneration) {
        data_listener_descriptor = -1;
        data_listener_generation++;
    }
    context->listenerDescriptor = -1;
    log_message("receiver", "stream connected");

    (void)connect_delivery_socket();

    command_request_keyframe(context->session, "stream start");
    command_show_scene(context->session, "stream connected");
    command_set_night_mode(context->session);

    last_recovery_point_milliseconds = 0;
    last_request_milliseconds = 0;
    last_frame_milliseconds = 0;
    {
        long delay = (long)read_number_setting("MIB3_SHIM_SHOWUI_MS", 1800);
        delayed_activation_milliseconds = (delay > 0)
            ? monotonic_milliseconds() + (word64)delay : 0;
    }

    while (!shouldStop) {
        unsigned char frameHeader[128];
        long received = 0;
        unsigned bodyLength;
        unsigned opcode;

        drain_queues(context->session);

        if (frame_flow_has_paused) {
            static word64 lastSceneEdge = 0;
            word64 now = monotonic_milliseconds();
            frame_flow_has_paused = 0;
            if (now - lastSceneEdge > 1000) {
                lastSceneEdge = now;
                command_request_keyframe(context->session, "scene edge");
                command_show_scene(context->session, "scene edge");
            }
        }

        while (received < 128) {
            long got;
            if (received == 0 && !wait_for_frame_or_run_pump(connectionDescriptor,
                                                             context->session, 100)) continue;
            got = read(connectionDescriptor, frameHeader + received, (long)(128 - received));
            if (got <= 0) { shouldStop = 1; break; }
            received += got;
        }
        if (shouldStop) break;

        bodyLength = (unsigned)frameHeader[0] | ((unsigned)frameHeader[1] << 8)
                   | ((unsigned)frameHeader[2] << 16) | ((unsigned)frameHeader[3] << 24);
        opcode = frameHeader[4];
        if (bodyLength > DELIVERY_MAX_FRAME) {
            log_number("receiver", "body too large", (word64)bodyLength, 0);
            break;
        }

        /* the buffer grows to the frame: a fixed 64 KB buffer overran on any
         * recovery point larger than that */
        if (bodyLength > bodyCapacity) {
            unsigned long newCapacity = bodyCapacity ? bodyCapacity : 65536;
            unsigned char *replacement;
            while (newCapacity < bodyLength) newCapacity <<= 1;
            replacement = (unsigned char *)malloc(newCapacity);
            if (!replacement) { log_message("receiver", "out of memory"); break; }
            if (bodyCapacity) memcpy(replacement, bodyBuffer, bodyCapacity);
            if (bodyBuffer) free(bodyBuffer);
            bodyBuffer = replacement;
            bodyCapacity = newCapacity;
        }

        received = 0;
        while (received < (long)bodyLength) {
            long got = read(connectionDescriptor, bodyBuffer + received,
                            (long)bodyLength - (unsigned long)received);
            if (got <= 0) { shouldStop = 1; break; }
            received += got;
        }
        if (shouldStop) break;

        if (opcode == 1) {
            /* unencrypted codec configuration: the parameter sets the decoder
             * needs, plus the encoder's own reported size */
            unsigned char annexb[4096];
            unsigned annexbLength = 0;
            unsigned detectedLengthSize;

            detectedLengthSize = extract_parameter_sets(bodyBuffer, bodyLength, annexb,
                                                        sizeof(annexb), &annexbLength);
            if (detectedLengthSize) {
                nal_length_size = (unsigned char)detectedLengthSize;
                if (annexbLength > sizeof(parameter_sets_annexb))
                    annexbLength = sizeof(parameter_sets_annexb);
                memcpy(parameter_sets_annexb, annexb, annexbLength);
                parameter_sets_length = annexbLength;
                log_number("receiver", "parameter sets in bytes",
                           (word64)parameter_sets_length, 0);
            }
        } else if (opcode == 0) {
            long plainLength = 0;
            int decrypted = 0;

            if (context->cipherKind == CIPHER_CHACHA && chaCha_initialise) {
                if (bodyLength >= 16) {
                    unsigned char nonce[8];
                    unsigned long long counter = (unsigned long long)context->frameCounter;
                    int index;
                    for (index = 0; index < 8; index++)
                        nonce[index] = (unsigned char)(counter >> (index * 8));

                    chaCha_initialise(cipherScratch, context->key, nonce);
                    chaCha_authenticate_header(cipherScratch, frameHeader, 128);
                    {
                        long decryptedLength = chaCha_decrypt(cipherScratch, bodyBuffer,
                                                              (long)bodyLength - 16, bodyBuffer);
                        int verificationResult = -1;
                        long tagLength = 0;
                        if (chaCha_verify)
                            tagLength = chaCha_verify(cipherScratch,
                                                      bodyBuffer + decryptedLength,
                                                      bodyBuffer + (long)bodyLength - 16,
                                                      &verificationResult);
                        /* the tag is the last sixteen bytes and is not data */
                        if (verificationResult == 0
                            && decryptedLength + tagLength == (long)bodyLength - 16) {
                            plainLength = (long)bodyLength - 16;
                            decrypted = 1;
                        } else {
                            log_number("receiver", "authentication failed", 0, 0);
                        }
                    }
                }
            } else if (context->cipherKind == CIPHER_AES_COUNTER && aes_counters_initialise) {
                if (!context->aesHasBeenInitialised) {
                    /* the keystream persists across frames: initialise once */
                    aes_counters_initialise(cipherScratch, context->key, context->key + 16);
                    context->aesHasBeenInitialised = 1;
                }
                aes_counters_advance(cipherScratch, bodyBuffer, (long)bodyLength, bodyBuffer);
                plainLength = (long)bodyLength;
                decrypted = 1;
            } else if (context->cipherKind == CIPHER_NONE) {
                plainLength = (long)bodyLength;
                decrypted = 1;                       /* diagnosis only */
            }

            context->frameCounter++;
            frameCount++;

            if (decrypted && plainLength > 0
                && convert_access_unit(bodyBuffer, (unsigned long)plainLength,
                                       nal_length_size) == 0) {
                word64 now = monotonic_milliseconds();
                int isRecoveryPoint = access_unit_starts_with_recovery_point(
                                          bodyBuffer, (unsigned long)plainLength, nal_length_size);

                if (last_frame_milliseconds && now - last_frame_milliseconds > 800)
                    frame_flow_has_paused = 1;
                last_frame_milliseconds = now;

                if (isRecoveryPoint) {
                    recoveryPointCount++;
                    last_recovery_point_milliseconds = now;
                    if (!firstRecoveryPointSeen) {
                        firstRecoveryPointSeen = 1;
                        log_number("receiver", "first recovery point at frame", (word64)frameCount, 0);
                        command_show_scene(context->session, "first recovery point");
                        command_request_keyframe(context->session, "first recovery point");
                    }
                }

                /* the parameter sets are not inside the media frames, so each
                 * frame is made self-contained before it leaves */
                if (parameter_sets_length > 0)
                    send_record(parameter_sets_annexb, parameter_sets_length);

                {
                    int wasConnected = (delivery_descriptor >= 0);
                    send_record(bodyBuffer, (unsigned long)plainLength);
                    if (!wasConnected && delivery_descriptor >= 0)
                        command_request_keyframe(context->session, "consumer connected");
                }
            }
        } else {
            /* opcodes 2, 4 and 5 are inert in the head unit's own receiver */
            otherOpcodeCount++;
        }
    }

    log_number("receiver", "frames", (word64)frameCount, 0);
    log_number("receiver", "recovery points", (word64)recoveryPointCount, 0);
    log_number("receiver", "other opcodes", (word64)otherOpcodeCount, 0);
    log_number("receiver", "records sent to consumer", (word64)delivery_sent_count, 0);
    log_number("receiver", "records dropped on the way", (word64)delivery_dropped_count, 0);
    if (bodyBuffer) free(bodyBuffer);
    close(connectionDescriptor);

    /* closing the socket is the end-of-stream the consumer already handles;
     * without it the cluster keeps re-showing the last frame forever */
    if (newest_receiver_generation == myGeneration) {
        close_delivery_socket();
        log_message("receiver", "frame consumer disconnected");
    }
    free(context);
    return 0;
}

/* ---------------------------------------------------------------------------
 * SECTION 19 - STREAM SETUP  (entry point 2, the type 111 path)
 *
 * The head unit's own dispatcher handles 100/101/102/110 and silently drops
 * 111, and it holds exactly one screen, so a second 110 is impossible. This
 * therefore owns its own listener and thread.
 * ------------------------------------------------------------------------- */

static int setup_cluster_stream(void *session, void *streamDictionary, void *response)
{
    long connectionIdentifier;
    unsigned char streamKey[32];
    int cipherKind = CIPHER_NONE;
    int port = 0;
    int listenerGeneration = 0;
    int listenerDescriptor;
    receiverContext *context;

    if (!session || !streamDictionary || !response) return -1;

    connectionIdentifier = dictionary_read_integer((coreFoundationDictionary)streamDictionary,
                                                   "streamConnectionID", 0);
    log_number("setup", "stream connection identifier", (word64)connectionIdentifier, 0);

    memset(streamKey, 0, sizeof(streamKey));
    if (*(void **)((char *)session + 0x478) == 0) {
        /* the session holds no pairing key object: derive an AES key and counter */
        if (derive_stream_key_for_screen) {
            derive_stream_key_for_screen((char *)session + 0x268, 0x10,
                                         connectionIdentifier, streamKey, streamKey + 16);
            cipherKind = CIPHER_AES_COUNTER;
        }
    } else {
        chachaKeyDeriveFunction deriveChaChaKey = airplay_library_base
            ? (chachaKeyDeriveFunction)(airplay_library_base + CHACHA_KEY_DERIVE_OFFSET) : 0;
        if (deriveChaChaKey && deriveChaChaKey((char *)session + 0x478,
                                              connectionIdentifier, 0, 0, streamKey) == 0)
            cipherKind = CIPHER_CHACHA;
    }
    log_number("setup", "cipher", (word64)(word32)cipherKind, 0);

    listenerDescriptor = open_data_listener(&port, &listenerGeneration);
    if (listenerDescriptor < 0) {
        log_message("setup", "could not open the data listener");
        return -1;
    }

    /* append {type:111, dataPort} to the response's stream list */
    if (create_dictionary && array_append && dictionary_set) {
        mutableCoreFoundationDictionary descriptor =
            create_dictionary(0, 0, dictionary_key_callbacks, dictionary_value_callbacks);
        if (descriptor) {
            mutableCoreFoundationArray streamList = 0;
            int createdList = 0;

            dictionary_put_integer(descriptor, "type", 111);
            dictionary_put_integer(descriptor, "dataPort", (signedWord)port);
            if (read_flag_setting("MIB3_SHIM_CONNID", 0) && connectionIdentifier)
                dictionary_put_integer(descriptor, "streamConnectionID", connectionIdentifier);

            streamList = (mutableCoreFoundationArray)dictionary_get(
                             (coreFoundationDictionary)response, keys.streams);
            if (!streamList) {
                streamList = make_empty_array();
                if (streamList) {
                    dictionary_put_object(response, "streams", streamList);
                    createdList = 1;
                }
            }
            if (streamList) array_append(streamList, descriptor);
            if (release_object) release_object(descriptor);
            if (createdList && release_object) release_object(streamList);
            log_message("setup", "cluster stream added to the answer");
        }
    }

    context = (receiverContext *)malloc(sizeof(receiverContext));
    if (!context) return -1;
    context->listenerDescriptor = listenerDescriptor;
    context->listenerGeneration = listenerGeneration;
    context->session = session;
    context->cipherKind = cipherKind;
    context->frameCounter = 0;
    context->aesHasBeenInitialised = 0;
    memcpy(context->key, streamKey, sizeof(context->key));

    /* a newer setup supersedes the older session for every command */
    current_command_session = session;
    current_command_generation++;

    start_control_socket();

    {
        void *thread = 0;
        if (create_thread(&thread, 0, receiver_thread, context) != 0) {
            log_message("setup", "receiver thread creation failed");
            free(context);
            return -1;
        }
    }
    return 0;
}

int AirPlayReceiverSessionSetup(void *session,
                                void *requestDictionary,
                                void **responseDictionaryOut,
                                int fourthArgument)
{
    int realResult = 0;

    resolve_all_symbols();
    log_message("session-setup", "entered");

    if (the_real_session_setup && !read_flag_setting("MIB3_SHIM_SKIP_REAL", 0))
        realResult = ((int (*)(void *, void *, void **, int))the_real_session_setup)(
                         session, requestDictionary, responseDictionaryOut, fourthArgument);

    log_number("session-setup", "real result", (word64)(word32)realResult, 0);

    if (read_flag_setting("MIB3_SHIM_ADD", 1) && activation_gate_is_open()
        && session && requestDictionary && responseDictionaryOut && *responseDictionaryOut) {
        coreFoundationObject streamsValue =
            (coreFoundationObject)dictionary_read_by_name(
                (coreFoundationDictionary)requestDictionary, "streams");

        if (streamsValue && object_is_array(streamsValue)) {
            coreFoundationArray requestedStreams = (coreFoundationArray)streamsValue;
            long streamCount = array_count(requestedStreams);
            long streamIndex;
            int handledClusterStream = 0;

            for (streamIndex = 0; streamIndex < streamCount; streamIndex++) {
                void *streamEntry = (void *)array_value_at(requestedStreams, streamIndex);
                if (!object_is_dictionary((coreFoundationObject)streamEntry)) continue;
                if (dictionary_read_integer((coreFoundationDictionary)streamEntry, "type", 0) == 111) {
                    if (setup_cluster_stream(session, streamEntry, *responseDictionaryOut) == 0)
                        handledClusterStream = 1;
                }
            }
            if (handledClusterStream && realResult != 0) realResult = 0;
        }
    }

    /* enabledFeatures belongs to the CONTROL setup only, and only when the
     * cluster was really advertised: the phone enforces the pair */
    if (read_flag_setting("MIB3_SHIM_ENABLEDFEAT", 1) && activation_gate_is_open()
        && cluster_was_advertised && responseDictionaryOut && *responseDictionaryOut) {
        coreFoundationObject proposedFeatures =
            (coreFoundationObject)dictionary_read_by_name(
                (coreFoundationDictionary)requestDictionary, "features");

        if (proposedFeatures) {
            coreFoundationString enabledFeaturesKey = make_string("enabledFeatures");
            if (enabledFeaturesKey
                && !dictionary_get((coreFoundationDictionary)*responseDictionaryOut,
                                   enabledFeaturesKey)) {
                mutableCoreFoundationArray accepted = make_empty_array();
                if (accepted) {
                    array_append(accepted, keys.viewAreasFeature);
                    array_append(accepted, keys.altScreenFeature);
                    dictionary_set((mutableCoreFoundationDictionary)*responseDictionaryOut,
                                   enabledFeaturesKey, accepted);
                    if (release_object) release_object(accepted);
                    log_message("session-setup", "alt screen and view areas claimed");
                }
            }
            if (release_object && enabledFeaturesKey) release_object(enabledFeaturesKey);
        }
    }
    return realResult;
}

/* ---------------------------------------------------------------------------
 * SECTION 20 - THE NAVIGATION TEXT GATE
 *
 * Unrelated to the cluster picture: the head unit draws its own address and
 * arrival lines around our map, fed by text carplay posts to its navigation
 * service. The injector points carplay's socket calls here so those records can
 * be dropped. The gate itself lives in mib3navtext.c.
 * ------------------------------------------------------------------------- */

static void navigation_text_log(const char *message)
{
    log_message("navigation-text", message);
}

static void *resolve_real_libc_symbol(const char *symbolName, void *linkedSymbol)
{
    typedef void *(*versionedLookupFunction)(void *, const char *, const char *);
    static versionedLookupFunction versionedLookup;
    static int alreadyTried = 0;
    void *resolved = 0;

    if (!alreadyTried) {
        alreadyTried = 1;
        versionedLookup = (versionedLookupFunction)dlsym(LOOKUP_ANY_OBJECT, "dlvsym");
    }
    /* the version tag matters: carplay's own relocations are GLIBC_2.17, so an
     * unversioned lookup could land on another library's wrapper */
    if (versionedLookup) resolved = versionedLookup(LOOKUP_ANY_OBJECT, symbolName, "GLIBC_2.17");
    if (!resolved) resolved = dlsym(LOOKUP_ANY_OBJECT, symbolName);
    if (!resolved) resolved = linkedSymbol;
    return resolved;
}

static void arm_navigation_text_gate(void)
{
    static int isArmed = 0;
    void *realConnect;
    void *realSend;
    void *realClose;

    if (isArmed) return;

    realConnect = resolve_real_libc_symbol("connect", (void *)connect);
    realSend    = resolve_real_libc_symbol("send", (void *)send);
    realClose   = resolve_real_libc_symbol("close", (void *)close);

    /* never pass a null through: that would turn every socket call into -1 */
    if (!realConnect) realConnect = (void *)connect;
    if (!realSend)    realSend = (void *)send;
    if (!realClose)   realClose = (void *)close;

    mib3_navtext_install(navigation_text_log,
                         (int (*)(int, const void *, unsigned int))realConnect,
                         (long (*)(int, const void *, unsigned long, int))realSend,
                         (int (*)(int))realClose);
    isArmed = 1;
}

int mib3_nt_connect(int descriptor, const void *address, unsigned int addressLength)
{
    arm_navigation_text_gate();
    return mib3_navtext_connect(descriptor, address, addressLength);
}

long mib3_nt_send(int descriptor, const void *buffer, unsigned long length, int flags)
{
    arm_navigation_text_gate();
    return mib3_navtext_send(descriptor, buffer, length, flags);
}

int mib3_nt_close(int descriptor)
{
    arm_navigation_text_gate();
    return mib3_navtext_close(descriptor);
}

/* ---------------------------------------------------------------------------
 * SECTION 21 - THE TEARDOWN CRASH GUARD
 *
 * The head unit's own fault, unrelated to the cluster picture. On teardown the
 * RSI client destructor frees the wrapped client and sends its unsubscribes;
 * the acknowledgement comes back through the library's dispatch and is
 * delivered to the FREED callback, which dereferences null and SIGSEGVs, and
 * the unit's crash handler reboots.
 *
 * Every symbol below is exported by the head unit's RSI library and every
 * internal call goes through the PLT, so interposing reaches all of it. A
 * process-global flag is set by ANY unsubscribe call; while it is set, message
 * dispatches are dropped, so no message can reach the freed callback. A process
 * restart clears the flag.
 *
 * The flag is all-or-nothing for the rest of the process. That is acceptable on
 * this build because unsubscribes only happen at teardown, which is exactly the
 * dangerous window.
 * ------------------------------------------------------------------------- */

static int guard_has_tripped = 0;
static unsigned long guard_trip_count = 0;
static unsigned long guard_dropped_count = 0;

static int crash_guard_is_armed(void)
{
    if (!read_flag_setting("MIB3_SHIM_GUARD", 1)) return 0;
    return activation_gate_is_open();
}

/* The next object ONLY - deliberately not the two-tier lookup used elsewhere.
 * "Any object" would find this library's own definition, so the seam would call
 * itself; measured as a hang when the real symbol is absent (the host bench has
 * no RSI library at all). "Next object" returns null instead, which is exactly
 * "there is nothing to forward to". */
static void *crash_guard_real_symbol(const char *mangledName)
{
    return dlsym(LOOKUP_NEXT_OBJECT, mangledName);
}

typedef void (*messageDispatchFunction)(void *, void *);
typedef void (*unsubscribeWithBoolFunction)(void *, void *, unsigned char);
typedef void (*unsubscribeAllFunction)(void *);
typedef void (*unsubscribePlainFunction)(void *, void *);

static void guard_note_trip(const char *what)
{
    if (!guard_has_tripped)
        log_message("crash-guard", "teardown detected - dropping dispatches");
    guard_has_tripped = 1;
    guard_trip_count++;
    log_number("crash-guard", what, (word64)guard_trip_count, 0);
}

static void guard_count_drop(void)
{
    if (guard_has_tripped) {
        guard_dropped_count++;
        log_number("crash-guard", "dropped dispatch", (word64)guard_dropped_count, 0);
    }
}

static int guard_passes_through(void) { return !crash_guard_is_armed(); }

/* The names below are the exact mangled identity of the head unit's symbols, so
 * they must NOT be extern "C". The C ABI carries the argument registers; the
 * return type is void at every call site in the shipped binary. */

/* WsSubscribeClient::broadcastMessage(const WsClientMessage&) - the fatal seam,
 * called through the PLT from WsSubscribeClient::onMessage. */
void _ZN3SAI17WsSubscribeClient16broadcastMessageERKNS_15WsClientMessageE(void *self, void *message)
  __asm__("_ZN3SAI17WsSubscribeClient16broadcastMessageERKNS_15WsClientMessageE");
void _ZN3SAI17WsSubscribeClient16broadcastMessageERKNS_15WsClientMessageE(void *self, void *message)
{
    static void *realFunction;
    static int alreadyLookedUp;
    if (!alreadyLookedUp) {
        realFunction = crash_guard_real_symbol(
            "_ZN3SAI17WsSubscribeClient16broadcastMessageERKNS_15WsClientMessageE");
        alreadyLookedUp = 1;
    }
    if (guard_passes_through()) {
        if (realFunction) ((messageDispatchFunction)realFunction)(self, message);
        return;
    }
    guard_count_drop();
    if (guard_has_tripped) return;          /* the freed callback never sees this */
    if (realFunction) ((messageDispatchFunction)realFunction)(self, message);
}

/* WsClientImpl::broadcastOnMessage(const WsClientMessage&) - the parent seam;
 * dropping here also suffices. */
void _ZN3SAI12WsClientImpl18broadcastOnMessageERKNS_15WsClientMessageE(void *self, void *message)
  __asm__("_ZN3SAI12WsClientImpl18broadcastOnMessageERKNS_15WsClientMessageE");
void _ZN3SAI12WsClientImpl18broadcastOnMessageERKNS_15WsClientMessageE(void *self, void *message)
{
    static void *realFunction;
    static int alreadyLookedUp;
    if (!alreadyLookedUp) {
        realFunction = crash_guard_real_symbol(
            "_ZN3SAI12WsClientImpl18broadcastOnMessageERKNS_15WsClientMessageE");
        alreadyLookedUp = 1;
    }
    if (guard_passes_through()) {
        if (realFunction) ((messageDispatchFunction)realFunction)(self, message);
        return;
    }
    guard_count_drop();
    if (guard_has_tripped) return;
    if (realFunction) ((messageDispatchFunction)realFunction)(self, message);
}

/* the unsubscribe family: flag setters, then the real call */

void _ZN3SAI17WsSubscribeClient11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEb(void *self, void *condition, unsigned char value)
  __asm__("_ZN3SAI17WsSubscribeClient11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEb");
void _ZN3SAI17WsSubscribeClient11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEb(void *self, void *condition, unsigned char value)
{
    static void *realFunction;
    static int alreadyLookedUp;
    if (!alreadyLookedUp) {
        realFunction = crash_guard_real_symbol(
            "_ZN3SAI17WsSubscribeClient11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEb");
        alreadyLookedUp = 1;
    }
    if (!guard_passes_through()) guard_note_trip("unsubscribe(string, bool)");
    if (realFunction) ((unsubscribeWithBoolFunction)realFunction)(self, condition, value);
}

void _ZN3SAI17WsSubscribeClient11unsubscribeERKNS_20WsSubscribeConditionEb(void *self, void *condition, unsigned char value)
  __asm__("_ZN3SAI17WsSubscribeClient11unsubscribeERKNS_20WsSubscribeConditionEb");
void _ZN3SAI17WsSubscribeClient11unsubscribeERKNS_20WsSubscribeConditionEb(void *self, void *condition, unsigned char value)
{
    static void *realFunction;
    static int alreadyLookedUp;
    if (!alreadyLookedUp) {
        realFunction = crash_guard_real_symbol(
            "_ZN3SAI17WsSubscribeClient11unsubscribeERKNS_20WsSubscribeConditionEb");
        alreadyLookedUp = 1;
    }
    if (!guard_passes_through()) guard_note_trip("unsubscribe(condition, bool)");
    if (realFunction) ((unsubscribeWithBoolFunction)realFunction)(self, condition, value);
}

void _ZN3SAI17WsSubscribeClient14unsubscribeAllEv(void *self)
  __asm__("_ZN3SAI17WsSubscribeClient14unsubscribeAllEv");
void _ZN3SAI17WsSubscribeClient14unsubscribeAllEv(void *self)
{
    static void *realFunction;
    static int alreadyLookedUp;
    if (!alreadyLookedUp) {
        realFunction = crash_guard_real_symbol(
            "_ZN3SAI17WsSubscribeClient14unsubscribeAllEv");
        alreadyLookedUp = 1;
    }
    if (!guard_passes_through()) guard_note_trip("unsubscribeAll");
    if (realFunction) ((unsubscribeAllFunction)realFunction)(self);
}

void _ZN3SAI12WsClientImpl11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(void *self, void *condition)
  __asm__("_ZN3SAI12WsClientImpl11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
void _ZN3SAI12WsClientImpl11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(void *self, void *condition)
{
    static void *realFunction;
    static int alreadyLookedUp;
    if (!alreadyLookedUp) {
        realFunction = crash_guard_real_symbol(
            "_ZN3SAI12WsClientImpl11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
        alreadyLookedUp = 1;
    }
    if (!guard_passes_through()) guard_note_trip("unsubscribe(string)");
    if (realFunction) ((unsubscribePlainFunction)realFunction)(self, condition);
}

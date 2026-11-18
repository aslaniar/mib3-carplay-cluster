/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ============================================================================
 * instrument_cluster_shim.h
 *
 * The vocabulary of instrument_cluster_shim.c: the six symbols the injector
 * repoints, the C library declarations the shim borrows, the CoreFoundation
 * object and function shapes, and every constant.
 *
 * Types, macros and prototypes only - nothing here allocates or executes. The
 * implementation, and the reasoning behind it, is in the .c file.
 * ==========================================================================*/
#ifndef INSTRUMENT_CLUSTER_SHIM_H
#define INSTRUMENT_CLUSTER_SHIM_H

/* ---- the six symbols the injector patches -------------------------------
 * Three Apple / LG seams, exported with C linkage and matched by name, plus the
 * three libc call slots. The slots are repointed at mib3_nt_* rather than at
 * connect / send / close, so RTLD_DEFAULT can never find them and recurse; the
 * gate itself lives in mib3navtext.c.
 * ---------------------------------------------------------------------- */

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

extern void mib3_navtext_install(void (*logMessage)(const char *),
                                 int (*realConnect)(int, const void *, unsigned int),
                                 long (*realSend)(int, const void *, unsigned long, int),
                                 int (*realClose)(int));
extern int mib3_navtext_connect(int, const void *, unsigned int);
extern long mib3_navtext_send(int, const void *, unsigned long, int);
extern int mib3_navtext_close(int);
extern int mib3_navtext_mode(void);
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

/* ---- constants -------------------------------------------------------- */

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
#define STRING_ENCODING_UTF8 0x08000100
#define CHACHA_KEY_DERIVE_OFFSET   0x163d8
#define AIRPLAY_ADD_DISPLAY_OFFSET 0x1f670
#define KNOWN_KEY_COUNT ((int)(sizeof(knownKeys) / sizeof(knownKeys[0])))
#define MILLIMETRES_PER_SCALE_UNIT 153
#define WIDE_CLASS_WIDTH           660
#define CENTRED_CLASS_HEIGHT       383
#define ETA_CLASS_HEIGHT           202
#define REFERENCE_DENSITY          670          /* pixels per millimetre * 1000 at 1x */
#define DEFAULT_CLUSTER_SCENE "maps:/car/instrumentcluster/map"
#define COMMAND_ANSWER_SLOT_COUNT 8
#define QUEUE_CAPACITY 16
#define VIEW_AREA_REQUEST_NEXT (-2)
#define VIEW_AREA_REQUEST_PREVIOUS (-3)
#define DELIVERY_MAGIC        0x4d334331u
#define DELIVERY_HEADER_BYTES 16u
#define DELIVERY_DEFAULT_PATH "/dev/shm/mib3cluster.sock"
#define DELIVERY_MAX_FRAME    (8u << 20)
#define DELIVERY_SEND_BUFFER  (4u << 20)
#define DELIVERY_MODE_DISABLED  0
#define DELIVERY_MODE_CONSUMER  1
#define CIPHER_NONE        0
#define CIPHER_AES_COUNTER 1
#define CIPHER_CHACHA      2


/* ---- object types and function shapes --------------------------------- */

typedef int (*createThreadFunction)(void **, const void *, void *(*)(void *), void *);

typedef unsigned char  byte;

typedef unsigned int   word32;

typedef unsigned long  word64;

typedef long           signedWord;

typedef unsigned long  frameSequence;

typedef const void *coreFoundationString;

typedef const void *coreFoundationArray;

typedef void       *mutableCoreFoundationArray;

typedef const void *coreFoundationDictionary;

typedef void       *mutableCoreFoundationDictionary;

typedef const void *coreFoundationObject;

typedef long        coreFoundationIndex;

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

typedef int  (*deriveStreamKeyFunction)(void *, int, signedWord, void *, void *);

typedef void (*cipherInitFunction)(void *, const void *, const void *);

typedef void (*cipherHeaderFunction)(void *, const void *, unsigned long);

typedef long (*cipherDecryptFunction)(void *, const void *, long, void *);

typedef long (*cipherVerifyFunction)(void *, const void *, const void *, int *);

typedef int  (*aesInitialiseFunction)(void *, const void *, const void *);

typedef int  (*aesUpdateFunction)(void *, const void *, long, void *);

typedef int  (*chachaKeyDeriveFunction)(void *, signedWord, int, int, void *);

typedef int  (*sendCommandFunction)(void *, void *, void *, void *);

typedef struct {
    char *characters;
    unsigned long capacity;
    unsigned long length;
} textBuffer;

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

typedef struct { long seconds; long nanoseconds; } timestampStorage;

typedef struct {
    int widthInPixels;
    int heightInPixels;
    int widthInMillimetres;
    int heightInMillimetres;
} clusterGeometry;

typedef struct {
    int originX, originY, width, height;
    int safeOriginX, safeOriginY, safeWidth, safeHeight;
} viewAreaRectangle;

typedef struct {
    volatile unsigned generation;
    volatile int viewAreaIndex;      /* -1 when the command was not a switch */
    char typeName[24];
} commandAnswerSlot;

typedef struct {
    volatile int entries[QUEUE_CAPACITY];
    volatile int writeIndex;
    volatile int readIndex;
    volatile int droppedCount;
} workQueue;

typedef struct {
    int listenerDescriptor;
    int listenerGeneration;
    void *session;
    unsigned char key[32];
    int cipherKind;                  /* 0 none, 1 aes counter, 2 chacha */
    signedWord frameCounter;
    int aesHasBeenInitialised;
} receiverContext;

    typedef void *(*versionedLookupFunction)(void *, const char *, const char *);

typedef void (*messageDispatchFunction)(void *, void *);

typedef void (*unsubscribeWithBoolFunction)(void *, void *, unsigned char);

typedef void (*unsubscribeAllFunction)(void *);

typedef void (*unsubscribePlainFunction)(void *, void *);

#endif /* INSTRUMENT_CLUSTER_SHIM_H */

#include "lwm2mclient.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <curl/curl.h>

// LwM2M Object 9: Software Management with correct resource mapping
#define SWM_OBJ_ID               9
#define SWM_RES_PACKAGE_NAME     0   // /9/0/0 (R/W)
#define SWM_RES_PACKAGE_VERSION  1   // /9/0/1 (R/W)
#define SWM_RES_PACKAGE          2   // /9/0/2 (Write binary) – not implemented here
#define SWM_RES_PACKAGE_URI      3   // /9/0/3 (R/W URI)
#define SWM_RES_INSTALL          4   // /9/0/4 (EXEC)
#define SWM_RES_UPDATE_STATE     7   // /9/0/7 (R)
#define SWM_RES_UPDATE_RESULT    9   // /9/0/9 (R)

// Software State Values
#define SW_STATE_IDLE        0
#define SW_STATE_DOWNLOADING 1
#define SW_STATE_DOWNLOADED  2
#define SW_STATE_UPDATING    3
#define SW_STATE_UPDATED     4

// Software Update Result Values (OMA LwM2M Object 9 spec /9/0/9)
#define SW_RESULT_INITIAL         0   // Initial value
#define SW_RESULT_DOWNLOADING     1   // Downloading in progress
#define SW_RESULT_INSTALLED       2   // Successfully installed
#define SW_RESULT_NO_STORAGE      3   // Not enough storage
#define SW_RESULT_OOM             4   // Out of memory
#define SW_RESULT_CONN_LOST       5   // Connection lost during download
#define SW_RESULT_INTEGRITY_FAIL  6   // Package integrity check failure
#define SW_RESULT_UNSUPPORTED     7   // Unsupported package type
#define SW_RESULT_INVALID_URI     8   // Invalid URI
#define SW_RESULT_DEVICE_ERROR    9   // Device defined update error

#define DOWNLOAD_MAX_RETRIES  3
#define DOWNLOAD_RETRY_DELAY  5   // seconds between retries

typedef struct {
    char packageName[128];
    char packageVersion[64];
    char packageUri[256];
    char downloadedFilePath[256];
    uint8_t updateState;
    uint8_t updateResult;
    pthread_mutex_t lock;
    pthread_t downloadThread;
    bool threadRunning;
    volatile bool stateChanged; // set by thread, cleared+acted on by main loop
    volatile bool cancelDownload; // set by free_object to abort curl
} software_mgmt_data_t;

typedef struct {
    char uri[256];
    char destpath[256];
    software_mgmt_data_t *data;
} download_args_t;

static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *stream) {
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}

// Called by curl periodically — return non-zero to abort the transfer
static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    software_mgmt_data_t *data = (software_mgmt_data_t *)clientp;
    return data->cancelDownload ? 1 : 0;
}

static void *download_thread(void *arg) {
    download_args_t *dl = (download_args_t *)arg;
    software_mgmt_data_t *data = dl->data;
    bool ok = false;
    uint8_t last_result = SW_RESULT_CONN_LOST;

    for (int attempt = 0; attempt < DOWNLOAD_MAX_RETRIES && !ok; attempt++) {
        if (attempt > 0) {
            fprintf(stdout, "[SWM] Retry %d/%d: %s\n", attempt, DOWNLOAD_MAX_RETRIES - 1, dl->uri);
            sleep(DOWNLOAD_RETRY_DELAY);
        }

        // Check for partial file to resume from
        curl_off_t resume_from = 0;
        struct stat st;
        if (stat(dl->destpath, &st) == 0) {
            resume_from = (curl_off_t)st.st_size;
            fprintf(stdout, "[SWM] Resuming from byte %lld\n", (long long)resume_from);
        }

        FILE *fp = fopen(dl->destpath, resume_from > 0 ? "ab" : "wb");
        if (!fp) {
            fprintf(stderr, "[SWM] Cannot open %s for writing\n", dl->destpath);
            break;
        }

        CURL *curl = curl_easy_init();
        if (!curl) {
            fclose(fp);
            break;
        }

        curl_easy_setopt(curl, CURLOPT_URL, dl->uri);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, resume_from);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);    // follow HTTP redirects (S3 presigned URLs)
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);   // 30s connection timeout
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);   // stall detection: < 1 byte/s
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);   // for 30s = network stall → retry
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);       // treat HTTP 4xx/5xx as error
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, data);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);        // enable progress callback

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);
        fclose(fp);

        if (res == CURLE_OK) {
            ok = true;
            fprintf(stdout, "[SWM] Download complete: %s (HTTP %ld)\n", dl->destpath, http_code);
        } else if (res == CURLE_RANGE_ERROR || (resume_from > 0 && http_code == 200)) {
            // Server doesn't support range requests — delete partial file and retry from scratch
            fprintf(stderr, "[SWM] Server doesn't support resume, restarting download\n");
            remove(dl->destpath);
            resume_from = 0;
        } else {
            fprintf(stderr, "[SWM] Download attempt %d failed: %s (HTTP %ld)\n",
                    attempt + 1, curl_easy_strerror(res), http_code);
            // Map curl error to OMA result code for final failure reporting
            if (res == CURLE_COULDNT_RESOLVE_HOST || res == CURLE_COULDNT_CONNECT ||
                res == CURLE_OPERATION_TIMEDOUT || res == CURLE_SEND_ERROR ||
                res == CURLE_RECV_ERROR || res == CURLE_GOT_NOTHING) {
                last_result = SW_RESULT_CONN_LOST;
            } else if (res == CURLE_BAD_CONTENT_ENCODING || res == CURLE_PARTIAL_FILE) {
                last_result = SW_RESULT_INTEGRITY_FAIL;
            } else if (http_code >= 400 && http_code < 500) {
                // Any 4xx (400, 403, 404 etc.) = bad/inaccessible URI
                last_result = SW_RESULT_INVALID_URI;
            } else {
                last_result = SW_RESULT_DEVICE_ERROR;
            }
            // Keep partial file for next resume attempt — do not remove
        }
    }

    if (!ok) {
        remove(dl->destpath);
    }

    pthread_mutex_lock(&data->lock);
    if (ok) {
        data->updateState = SW_STATE_DOWNLOADED;
        data->updateResult = SW_RESULT_INITIAL;
    } else {
        data->updateState = SW_STATE_IDLE;
        data->updateResult = last_result;
        fprintf(stderr, "[SWM] Download failed after %d retries: %s\n", DOWNLOAD_MAX_RETRIES, dl->uri);
    }
    data->threadRunning = false;
    data->stateChanged = true;
    pthread_mutex_unlock(&data->lock);

    free(dl);
    return NULL;
}

// Discover function — tells the server which resource IDs exist under /9/0
static uint8_t prv_sw_discover(lwm2m_context_t *contextP, uint16_t instanceId,
                               int *numDataP, lwm2m_data_t **dataArrayP,
                               lwm2m_object_t *objectP) {
    (void)contextP;
    (void)objectP;

    if (instanceId != 0) return COAP_404_NOT_FOUND;

    static const uint16_t resList[] = {
        SWM_RES_PACKAGE_NAME,
        SWM_RES_PACKAGE_VERSION,
        SWM_RES_PACKAGE_URI,
        SWM_RES_INSTALL,
        SWM_RES_UPDATE_STATE,
        SWM_RES_UPDATE_RESULT,
    };
    static const int resCount = sizeof(resList) / sizeof(resList[0]);

    if (*numDataP == 0) {
        *dataArrayP = lwm2m_data_new(resCount);
        if (!*dataArrayP) return COAP_500_INTERNAL_SERVER_ERROR;
        *numDataP = resCount;
        for (int i = 0; i < resCount; i++) {
            (*dataArrayP)[i].id = resList[i];
        }
    }
    return COAP_205_CONTENT;
}

// Read function
static uint8_t prv_sw_read(lwm2m_context_t *contextP, uint16_t instanceId,
                           int *numDataP, lwm2m_data_t **dataArrayP, lwm2m_object_t *objectP) {
    software_mgmt_data_t *data = (software_mgmt_data_t *)objectP->userData;
    (void)contextP;
    if (instanceId != 0) return COAP_404_NOT_FOUND;

    if (*numDataP == 0) {
        *dataArrayP = lwm2m_data_new(5);
        if (!*dataArrayP) return COAP_500_INTERNAL_SERVER_ERROR;
        (*dataArrayP)[0].id = SWM_RES_PACKAGE_NAME;
        (*dataArrayP)[1].id = SWM_RES_PACKAGE_VERSION;
        (*dataArrayP)[2].id = SWM_RES_PACKAGE_URI;
        (*dataArrayP)[3].id = SWM_RES_UPDATE_STATE;
        (*dataArrayP)[4].id = SWM_RES_UPDATE_RESULT;
        *numDataP = 5;
    }

    pthread_mutex_lock(&data->lock);
    uint8_t ret = COAP_205_CONTENT;
    for (int i = 0; i < *numDataP; i++) {
        switch ((*dataArrayP)[i].id) {
        case SWM_RES_PACKAGE_NAME:
            lwm2m_data_encode_string(data->packageName, *dataArrayP + i);
            break;
        case SWM_RES_PACKAGE_VERSION:
            lwm2m_data_encode_string(data->packageVersion, *dataArrayP + i);
            break;
        case SWM_RES_PACKAGE_URI:
            lwm2m_data_encode_string(data->packageUri, *dataArrayP + i);
            break;
        case SWM_RES_UPDATE_STATE:
            lwm2m_data_encode_int(data->updateState, *dataArrayP + i);
            break;
        case SWM_RES_UPDATE_RESULT:
            lwm2m_data_encode_int(data->updateResult, *dataArrayP + i);
            break;
        default:
            ret = COAP_404_NOT_FOUND;
            goto done;
        }
    }
done:
    pthread_mutex_unlock(&data->lock);
    return ret;
}

// Write function
static uint8_t prv_sw_write(lwm2m_context_t *contextP, uint16_t instanceId,
                            int numData, lwm2m_data_t *dataArray,
                            lwm2m_object_t *objectP, lwm2m_write_type_t writeType) {
    software_mgmt_data_t *data = (software_mgmt_data_t *)objectP->userData;
    (void)contextP;
    (void)writeType;
    if (instanceId != 0) return COAP_404_NOT_FOUND;

    for (int i = 0; i < numData; i++) {
        switch (dataArray[i].id) {
        case SWM_RES_PACKAGE_NAME: {
            size_t nlen = (dataArray[i].value.asBuffer.length >= sizeof(data->packageName)) ?
                          sizeof(data->packageName) - 1 : dataArray[i].value.asBuffer.length;
            pthread_mutex_lock(&data->lock);
            memset(data->packageName, 0, sizeof(data->packageName));
            strncpy(data->packageName, (char*)dataArray[i].value.asBuffer.buffer, nlen);
            pthread_mutex_unlock(&data->lock);
            break;
        }
        case SWM_RES_PACKAGE_VERSION: {
            size_t vlen = (dataArray[i].value.asBuffer.length >= sizeof(data->packageVersion)) ?
                          sizeof(data->packageVersion) - 1 : dataArray[i].value.asBuffer.length;
            pthread_mutex_lock(&data->lock);
            memset(data->packageVersion, 0, sizeof(data->packageVersion));
            strncpy(data->packageVersion, (char*)dataArray[i].value.asBuffer.buffer, vlen);
            pthread_mutex_unlock(&data->lock);
            break;
        }
        case SWM_RES_PACKAGE_URI: {
            pthread_mutex_lock(&data->lock);
            if (data->threadRunning) {
                // Check if this is a CoAP retransmission of the same URI.
                // If so, return 204 — download is already in progress for that URI.
                // If it's a different URI trying to interrupt, reject with 400.
                size_t inlen = (dataArray[i].value.asBuffer.length >= sizeof(data->packageUri)) ?
                               sizeof(data->packageUri) - 1 : dataArray[i].value.asBuffer.length;
                bool sameUri = (strncmp(data->packageUri,
                                        (char*)dataArray[i].value.asBuffer.buffer,
                                        inlen) == 0 && data->packageUri[inlen] == '\0');
                pthread_mutex_unlock(&data->lock);
                if (sameUri) {
                    fprintf(stdout, "[SWM] Retransmit for same URI, download already in progress\n");
                    return COAP_204_CHANGED;
                }
                fprintf(stderr, "[SWM] Download already in progress, rejecting different URI\n");
                return COAP_400_BAD_REQUEST;
            }

            memset(data->packageUri, 0, sizeof(data->packageUri));
            size_t len = (dataArray[i].value.asBuffer.length >= sizeof(data->packageUri)) ?
                         sizeof(data->packageUri) - 1 : dataArray[i].value.asBuffer.length;
            strncpy(data->packageUri, (char*)dataArray[i].value.asBuffer.buffer, len);

            const char *lastslash = strrchr(data->packageUri, '/');
            const char *fname = (lastslash && *(lastslash + 1) != '\0') ? lastslash + 1 : "sw_download.bin";
            snprintf(data->downloadedFilePath, sizeof(data->downloadedFilePath), "/tmp/%s", fname);

            data->updateState = SW_STATE_DOWNLOADING;
            data->updateResult = SW_RESULT_INITIAL;
            data->threadRunning = true;
            data->stateChanged = true;  // notify server: download started
            pthread_mutex_unlock(&data->lock);

            download_args_t *dl = (download_args_t *)malloc(sizeof(download_args_t));
            if (!dl) {
                pthread_mutex_lock(&data->lock);
                data->updateState = SW_STATE_IDLE;
                data->threadRunning = false;
                pthread_mutex_unlock(&data->lock);
                return COAP_500_INTERNAL_SERVER_ERROR;
            }
            strncpy(dl->uri, data->packageUri, sizeof(dl->uri) - 1);
            dl->uri[sizeof(dl->uri) - 1] = '\0';
            strncpy(dl->destpath, data->downloadedFilePath, sizeof(dl->destpath) - 1);
            dl->destpath[sizeof(dl->destpath) - 1] = '\0';
            dl->data = data;

            if (pthread_create(&data->downloadThread, NULL, download_thread, dl) != 0) {
                free(dl);
                pthread_mutex_lock(&data->lock);
                data->updateState = SW_STATE_IDLE;
                data->threadRunning = false;
                pthread_mutex_unlock(&data->lock);
                return COAP_500_INTERNAL_SERVER_ERROR;
            }
            // Keep thread joinable so shutdown can cleanly wait for it.
            // Do NOT detach — pthread_cancel/pthread_join on detached threads
            // is undefined behavior.
            break;
        }
        default:
            return COAP_404_NOT_FOUND;
        }
    }
    return COAP_204_CHANGED;
}

// Execute function
static uint8_t prv_sw_execute(lwm2m_context_t *contextP, uint16_t instanceId,
                              uint16_t resourceId, uint8_t *buffer, int length,
                              lwm2m_object_t *objectP) {
    software_mgmt_data_t *data = (software_mgmt_data_t *)objectP->userData;
    (void)contextP;
    (void)buffer;
    (void)length;

    if (instanceId != 0) return COAP_404_NOT_FOUND;

    switch (resourceId) {
    case SWM_RES_INSTALL: {
        // Check and transition atomically under one lock — prevents race
        // where download thread changes state between check and act.
        pthread_mutex_lock(&data->lock);
        if (data->updateState != SW_STATE_DOWNLOADED) {
            data->updateResult = SW_RESULT_CONN_LOST;
            data->stateChanged = true;
            pthread_mutex_unlock(&data->lock);
            fprintf(stderr, "[SWM] Cannot install: package not in DOWNLOADED state\n");
            return COAP_400_BAD_REQUEST;
        }
        data->updateState = SW_STATE_UPDATING;
        pthread_mutex_unlock(&data->lock);

        // Simulate installation
        pthread_mutex_lock(&data->lock);
        data->updateState = SW_STATE_UPDATED;
        data->updateResult = SW_RESULT_INSTALLED;
        data->stateChanged = true;
        pthread_mutex_unlock(&data->lock);

        fprintf(stdout, "[SWM] Software installed successfully from: %s\n", data->downloadedFilePath);
        return COAP_204_CHANGED;
    }
    default:
        return COAP_405_METHOD_NOT_ALLOWED;
    }
}

// Called from the main loop (main thread) to safely notify the server of state changes.
// lwm2m_resource_value_changed() is NOT thread-safe so it must never be called from the download thread.
void swm_notify_if_changed(lwm2m_context_t *lwm2mH, lwm2m_object_t *objectP) {
    if (!objectP || !objectP->userData) return;
    software_mgmt_data_t *data = (software_mgmt_data_t *)objectP->userData;

    pthread_mutex_lock(&data->lock);
    bool changed = data->stateChanged;
    data->stateChanged = false;
    pthread_mutex_unlock(&data->lock);

    if (!changed) return;

    lwm2m_uri_t uri;
    // Notify Update State (/9/0/7)
    if (lwm2m_stringToUri("/9/0/7", 6, &uri)) {
        lwm2m_resource_value_changed(lwm2mH, &uri);
    }
    // Notify Update Result (/9/0/9)
    if (lwm2m_stringToUri("/9/0/9", 6, &uri)) {
        lwm2m_resource_value_changed(lwm2mH, &uri);
    }
}

// Allocate/init object
lwm2m_object_t *init_software_mgmt_object(void) {
    lwm2m_object_t *obj = (lwm2m_object_t*)lwm2m_malloc(sizeof(lwm2m_object_t));
    if (!obj) return NULL;
    memset(obj, 0, sizeof(lwm2m_object_t));

    obj->objID = SWM_OBJ_ID;
    obj->instanceList = (lwm2m_list_t*)lwm2m_malloc(sizeof(lwm2m_list_t));
    if (!obj->instanceList) { lwm2m_free(obj); return NULL; }
    memset(obj->instanceList, 0, sizeof(lwm2m_list_t));
    obj->instanceList->id = 0;

    software_mgmt_data_t *data = (software_mgmt_data_t*)lwm2m_malloc(sizeof(software_mgmt_data_t));
    if (!data) { lwm2m_free(obj->instanceList); lwm2m_free(obj); return NULL; }
    memset(data, 0, sizeof(software_mgmt_data_t));
    data->updateState = SW_STATE_IDLE;
    data->updateResult = SW_RESULT_CONN_LOST; // Signal to server that any prior task did not complete
    data->stateChanged = true;              // Notify server of current state right after registration
    pthread_mutex_init(&data->lock, NULL);

    obj->userData = data;
    obj->readFunc = prv_sw_read;
    obj->discoverFunc = prv_sw_discover;
    obj->writeFunc = prv_sw_write;
    obj->executeFunc = prv_sw_execute;
    return obj;
}

// Free object
void free_object_software_mgmt(lwm2m_object_t *objectP) {
    if (objectP) {
        if (objectP->userData) {
            software_mgmt_data_t *data = (software_mgmt_data_t *)objectP->userData;
            // If a download is running, cancel and join before freeing.
            // Thread is joinable (not detached), so pthread_join is safe.
            pthread_mutex_lock(&data->lock);
            bool running = data->threadRunning;
            pthread_mutex_unlock(&data->lock);
            if (running) {
                data->cancelDownload = true;           // signal curl to abort cleanly
                pthread_join(data->downloadThread, NULL);  // wait for thread to exit
            }
            pthread_mutex_destroy(&data->lock);
            lwm2m_free(data);
        }
        if (objectP->instanceList) lwm2m_free(objectP->instanceList);
        lwm2m_free(objectP);
    }
}

// Debug display
void display_software_mgmt_object(lwm2m_object_t *objectP) {
    software_mgmt_data_t *data = (software_mgmt_data_t *)objectP->userData;
    fprintf(stdout, "  /%u: Software Management object:\n", objectP->objID);
    if (data) {
        pthread_mutex_lock(&data->lock);
        fprintf(stdout, "    package name: %s, version: %s\n", data->packageName, data->packageVersion);
        fprintf(stdout, "    uri: %s\n", data->packageUri);
        fprintf(stdout, "    state: %u, result: %u\n", data->updateState, data->updateResult);
        fprintf(stdout, "    downloaded file: %s\n", data->downloadedFilePath);
        pthread_mutex_unlock(&data->lock);
    }
}

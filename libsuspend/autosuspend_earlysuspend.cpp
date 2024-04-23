#define LOG_TAG "libsuspend"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <log/log.h>

#include "autosuspend_ops.h"

#define EARLYSUSPEND_SYS_POWER_STATE "/sys/power/state"
#define EARLYSUSPEND_WAIT_FOR_FB_SLEEP "/sys/power/wait_for_fb_sleep"
#define EARLYSUSPEND_WAIT_FOR_FB_WAKE "/sys/power/wait_for_fb_wake"

static int sPowerStatefd;
static const char *pwr_state_mem = "mem";
static const char *pwr_state_on = "on";
static pthread_t earlysuspend_thread;
static pthread_mutex_t earlysuspend_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t earlysuspend_cond = PTHREAD_COND_INITIALIZER;
static bool wait_for_earlysuspend;
static enum {
    EARLYSUSPEND_ON,
    EARLYSUSPEND_MEM,
} earlysuspend_state = EARLYSUSPEND_ON;

int wait_for_fb_wake(void)
{
    int err = 0;
    char buf;
    int fd = TEMP_FAILURE_RETRY(open(EARLYSUSPEND_WAIT_FOR_FB_WAKE, O_RDONLY, 0));
    // if the file doesn't exist, the error will be caught in read() below
    err = TEMP_FAILURE_RETRY(read(fd, &buf, 1));
    ALOGE_IF(err < 0,
            "*** ANDROID_WAIT_FOR_FB_WAKE failed (%s)", strerror(errno));
    close(fd);
    return err < 0 ? err : 0;
}

static int wait_for_fb_sleep(void)
{
    int err = 0;
    char buf;
    int fd = TEMP_FAILURE_RETRY(open(EARLYSUSPEND_WAIT_FOR_FB_SLEEP, O_RDONLY, 0));
    // if the file doesn't exist, the error will be caught in read() below
    err = TEMP_FAILURE_RETRY(read(fd, &buf, 1));
    ALOGE_IF(err < 0,
            "*** ANDROID_WAIT_FOR_FB_SLEEP failed (%s)", strerror(errno));
    close(fd);
    return err < 0 ? err : 0;
}

static void *earlysuspend_thread_func(void __unused *arg)
{
    while (1) {
        if (wait_for_fb_sleep()) {
            ALOGE("Failed reading wait_for_fb_sleep, exiting earlysuspend thread");
            return NULL;
        }
        pthread_mutex_lock(&earlysuspend_mutex);
        earlysuspend_state = EARLYSUSPEND_MEM;
        pthread_cond_signal(&earlysuspend_cond);
        pthread_mutex_unlock(&earlysuspend_mutex);

        if (wait_for_fb_wake()) {
            ALOGE("Failed reading wait_for_fb_wake, exiting earlysuspend thread");
            return NULL;
        }
        pthread_mutex_lock(&earlysuspend_mutex);
        earlysuspend_state = EARLYSUSPEND_ON;
        pthread_cond_signal(&earlysuspend_cond);
        pthread_mutex_unlock(&earlysuspend_mutex);
    }
}
static int autosuspend_earlysuspend_enable(void)
{
    char buf[80];
    int ret;

    ALOGV("autosuspend_earlysuspend_enable");

    ret = write(sPowerStatefd, pwr_state_mem, strlen(pwr_state_mem));
    if (ret < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s", EARLYSUSPEND_SYS_POWER_STATE, buf);
        goto err;
    }

    if (wait_for_earlysuspend) {
        pthread_mutex_lock(&earlysuspend_mutex);
        while (earlysuspend_state != EARLYSUSPEND_MEM) {
            pthread_cond_wait(&earlysuspend_cond, &earlysuspend_mutex);
        }
        pthread_mutex_unlock(&earlysuspend_mutex);
    }

    ALOGV("autosuspend_earlysuspend_enable done");

    return 0;

err:
    return ret;
}

static int autosuspend_earlysuspend_disable(void)
{
    char buf[80];
    int ret;

    ALOGV("autosuspend_earlysuspend_disable");

    ret = TEMP_FAILURE_RETRY(write(sPowerStatefd, pwr_state_on, strlen(pwr_state_on)));
    if (ret < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s", EARLYSUSPEND_SYS_POWER_STATE, buf);
        goto err;
    }

    if (wait_for_earlysuspend) {
        pthread_mutex_lock(&earlysuspend_mutex);
        while (earlysuspend_state != EARLYSUSPEND_ON) {
            pthread_cond_wait(&earlysuspend_cond, &earlysuspend_mutex);
        }
        pthread_mutex_unlock(&earlysuspend_mutex);
    }

    ALOGV("autosuspend_earlysuspend_disable done");

    return 0;

err:
    return ret;
}

static int force_suspend(int timeout_ms) {
    ALOGV("force_suspend called with timeout: %d", timeout_ms);
    return 0;
}

static void autosuspend_set_wakeup_callback(__attribute__ ((unused)) void (*func)(bool success)) {}

struct autosuspend_ops autosuspend_earlysuspend_ops = {
        .enable = autosuspend_earlysuspend_enable,
        .disable = autosuspend_earlysuspend_disable,
        .force_suspend = force_suspend,
        .set_wakeup_callback = autosuspend_set_wakeup_callback,
};

void start_earlysuspend_thread(void)
{
    char buf[80];
    int ret;

    ret = access(EARLYSUSPEND_WAIT_FOR_FB_SLEEP, F_OK);
    if (ret < 0) {
        return;
    }

    ret = access(EARLYSUSPEND_WAIT_FOR_FB_WAKE, F_OK);
    if (ret < 0) {
        return;
    }

    wait_for_fb_wake();

    ALOGI("Starting early suspend unblocker thread");
    ret = pthread_create(&earlysuspend_thread, NULL, earlysuspend_thread_func, NULL);
    if (ret) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error creating thread: %s", buf);
        return;
    }

    wait_for_earlysuspend = true;
}

struct autosuspend_ops *autosuspend_earlysuspend_init(void)
{
    char buf[80];
    int ret;

    sPowerStatefd = TEMP_FAILURE_RETRY(open(EARLYSUSPEND_SYS_POWER_STATE, O_RDWR));

    if (sPowerStatefd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGW("Error opening %s: %s", EARLYSUSPEND_SYS_POWER_STATE, buf);
        return NULL;
    }

    ret = TEMP_FAILURE_RETRY(write(sPowerStatefd, "on", 2));
    if (ret < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGW("Error writing 'on' to %s: %s", EARLYSUSPEND_SYS_POWER_STATE, buf);
        goto err_write;
    }

    ALOGI("Selected early suspend");

    start_earlysuspend_thread();

    return &autosuspend_earlysuspend_ops;

err_write:
    close(sPowerStatefd);
    return NULL;
}

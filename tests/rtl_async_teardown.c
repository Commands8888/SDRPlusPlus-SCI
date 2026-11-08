/* Fault injection for the private driver's async cancellation paths.
 * No receiver is required. Compile the patched source with mocked USB I/O;
 * ASan/UBSan check the real allocation, callback and cleanup code. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb.h>

static int submit_mock(struct libusb_transfer *);
static int cancel_mock(struct libusb_transfer *);
static int events_mock(libusb_context *, struct timeval *, int *);
static void free_mock(struct libusb_transfer *);
static int release_mock(libusb_device_handle *, int);
static void close_mock(libusb_device_handle *);
static void exit_mock(libusb_context *);
#define libusb_submit_transfer submit_mock
#define libusb_cancel_transfer cancel_mock
#define libusb_handle_events_timeout_completed events_mock
#define libusb_free_transfer free_mock
#define libusb_release_interface release_mock
#define libusb_close close_mock
#define libusb_exit exit_mock
#include "librtlsdr.c"
#undef libusb_free_transfer

enum scenario { CALLBACK_CANCEL, USB_ERROR, DEVICE_LOST, PARTIAL_SUBMIT,
                UNREAPABLE_ERROR, UNREAPABLE_INTERRUPTED };
struct tracked_transfer {
    struct libusb_transfer *ptr;
    unsigned char *buffer;
    int active;
    int cancelled;
};
static struct tracked_transfer transfers[4];
static enum scenario scenario;
static rtlsdr_dev_t *device;
static int submissions, events, frees, samples, releases, closes, exits;
static struct libusb_transfer **saved_xfers;
static unsigned char **saved_buffers;

static int slot(struct libusb_transfer *p)
{
    int i;
    for (i = 0; i < 4; ++i)
        if (transfers[i].ptr == p)
            return i;
    return -1;
}

static int submit_mock(struct libusb_transfer *p)
{
    int i = slot(p);
    submissions++;
    if (scenario == PARTIAL_SUBMIT && submissions == 2)
        return LIBUSB_ERROR_IO;
    if (i < 0) {
        for (i = 0; i < 4 && transfers[i].ptr; ++i) {}
        assert(i < 4);
        transfers[i].ptr = p;
        transfers[i].buffer = p->buffer;
    }
    assert(!transfers[i].active);
    transfers[i].active = 1;
    return 0;
}

static int cancel_mock(struct libusb_transfer *p)
{
    int i = slot(p);
    if (i < 0 || !transfers[i].active)
        return LIBUSB_ERROR_NOT_FOUND;
    transfers[i].cancelled = 1;
    return 0;
}

static void deliver(int i, enum libusb_transfer_status status)
{
    struct libusb_transfer *p = transfers[i].ptr;
    assert(transfers[i].active);
    transfers[i].active = 0;
    transfers[i].cancelled = 0;
    p->status = status;
    p->actual_length = status == LIBUSB_TRANSFER_COMPLETED ? 512 : 0;
    p->callback(p);
}

static int events_mock(libusb_context *ctx, struct timeval *tv, int *completed)
{
    int i;
    (void)ctx; (void)tv; (void)completed;
    assert(++events < 100);
    if (events == 1) {
        saved_xfers = device->xfer;
        saved_buffers = device->xfer_buf;
        if (scenario == CALLBACK_CANCEL)
            deliver(0, LIBUSB_TRANSFER_COMPLETED);
        else if (scenario == DEVICE_LOST)
            deliver(0, LIBUSB_TRANSFER_NO_DEVICE);
        else if (scenario != PARTIAL_SUBMIT)
            return LIBUSB_ERROR_IO;
    } else if (scenario == UNREAPABLE_ERROR) {
        return LIBUSB_ERROR_IO;
    } else if (scenario == UNREAPABLE_INTERRUPTED) {
        return LIBUSB_ERROR_INTERRUPTED;
    }
    for (i = 0; i < 4; ++i)
        if (transfers[i].cancelled)
            deliver(i, LIBUSB_TRANSFER_CANCELLED);
    return 0;
}

static void free_mock(struct libusb_transfer *p)
{
    int i = slot(p);
    assert(i < 0 || !transfers[i].active);
    if (i >= 0)
        transfers[i].ptr = NULL;
    frees++;
    libusb_free_transfer(p);
}
static int release_mock(libusb_device_handle *h, int i)
{ (void)h; (void)i; releases++; return 0; }
static void close_mock(libusb_device_handle *h)
{ (void)h; closes++; }
static void exit_mock(libusb_context *c)
{ (void)c; exits++; }
static void sample(unsigned char *buf, uint32_t len, void *ctx)
{
    (void)buf; (void)ctx;
    assert(len == 512);
    samples++;
    assert(rtlsdr_cancel_async(device) == 0);
}

static void run(enum scenario which)
{
    int i, rc, stuck;
    memset(transfers, 0, sizeof(transfers));
    scenario = which;
    submissions = events = frees = samples = releases = closes = exits = 0;
    device = calloc(1, sizeof(*device));
    assert(device);
    device->devh = (libusb_device_handle *)1;
    rc = rtlsdr_read_async(device, sample, NULL, 4, 512);
    assert(device->async_status == RTLSDR_INACTIVE);
    stuck = which == UNREAPABLE_ERROR || which == UNREAPABLE_INTERRUPTED;
    assert(device->xfer_stuck == stuck);
    if (which == CALLBACK_CANCEL) {
        assert(samples == 1);
        assert(submissions == 4); /* No resubmit after sample cancels. */
    } else {
        assert(samples == 0);
    }
    if (which == DEVICE_LOST)
        assert(rc == -1);
    if (which == UNREAPABLE_INTERRUPTED)
        assert(events == 51); /* Error + exactly 50 bounded drain slices. */
    if (which == UNREAPABLE_ERROR)
        assert(events == 2); /* Stop draining on unrecoverable USB error. */
    if (stuck) {
        assert(frees == 0);
        assert(rtlsdr_read_async(device, sample, NULL, 4, 512) == -3);
    } else {
        assert(device->xfer_live == 0);
        assert(frees == 4);
    }
    device->dev_lost = 1; /* Skip hardware register writes in close. */
    assert(rtlsdr_close(device) == 0);
    assert(closes == !stuck && releases == !stuck && exits == !stuck);
    if (stuck) {
        /* The real process retains these until exit. The harness can reclaim
         * them now because the fake kernel owns no transfers. */
        for (i = 0; i < 4; ++i) {
            libusb_free_transfer(transfers[i].ptr);
            free(transfers[i].buffer);
        }
        free(saved_xfers);
        free(saved_buffers);
    }
}

int main(void)
{
    int i;
    for (i = CALLBACK_CANCEL; i <= UNREAPABLE_INTERRUPTED; ++i)
        run((enum scenario)i);
    puts("OK: six async teardown scenarios (ASan/UBSan)");
    return 0;
}

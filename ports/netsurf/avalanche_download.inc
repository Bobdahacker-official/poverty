/* NetSurf framebuffer download frontend for the Magenta desktop. */
#include <stdint.h>
#include <unistd.h>
#include "desktop/download.h"
#include "netsurf/download.h"
#include "magenta.h"

struct gui_download_window {
  download_context *context;
  unsigned id;
};

static int magenta_download_request(struct gui_download_window *window,
                                    unsigned operation) {
  struct av_download_request request = {
      .operation = operation, .id = window->id};
  return syscall(AV_LINUX_DOWNLOAD, &request);
}

static void magenta_download_check(void *opaque);

static void magenta_download_release(struct gui_download_window *window,
                                     bool abort) {
  framebuffer_schedule(-1, magenta_download_check, window);
  if (abort)
    download_context_abort(window->context);
  download_context_destroy(window->context);
  free(window);
}

static void magenta_download_check(void *opaque) {
  struct gui_download_window *window = opaque;
  if (magenta_download_request(window, AV_DOWNLOAD_STATUS) < 0) {
    magenta_download_release(window, true);
    return;
  }
  framebuffer_schedule(200, magenta_download_check, window);
}

static struct gui_download_window *
magenta_download_create(download_context *context, struct gui_window *parent) {
  (void)parent;
  struct gui_download_window *window = calloc(1, sizeof(*window));
  if (!window)
    return NULL;
  struct av_download_request request = {
      .operation = AV_DOWNLOAD_BEGIN,
      .total = download_context_get_total_length(context)};
  const char *name = download_context_get_filename(context);
  if (name)
    snprintf(request.filename, sizeof(request.filename), "%s", name);
  long id = syscall(AV_LINUX_DOWNLOAD, &request);
  if (id < 0) {
    free(window);
    return NULL;
  }
  window->context = context;
  window->id = id;
  framebuffer_schedule(200, magenta_download_check, window);
  return window;
}

static nserror magenta_download_data(struct gui_download_window *window,
                                    const char *data, unsigned size) {
  struct av_download_request request = {
      .operation = AV_DOWNLOAD_DATA, .id = window->id,
      .size = size, .data = (uintptr_t)data};
  if (syscall(AV_LINUX_DOWNLOAD, &request) < 0)
    magenta_download_release(window, true);
  return NSERROR_OK;
}

static void magenta_download_error(struct gui_download_window *window,
                                   const char *message) {
  (void)message;
  magenta_download_request(window, AV_DOWNLOAD_FAILED);
  magenta_download_release(window, false);
}

static void magenta_download_done(struct gui_download_window *window) {
  magenta_download_request(window, AV_DOWNLOAD_DONE);
  magenta_download_release(window, false);
}

static struct gui_download_table magenta_download_table = {
    .create = magenta_download_create,
    .data = magenta_download_data,
    .error = magenta_download_error,
    .done = magenta_download_done,
};

#ifndef IDR_REQUESTER_H
#define IDR_REQUESTER_H

#include <glib.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IdrRequester IdrRequester;

IdrRequester *idr_requester_new(void);
void idr_requester_free(IdrRequester *req);
void idr_requester_note_source(IdrRequester *req, const struct sockaddr *addr, socklen_t len);
void idr_requester_handle_warning(IdrRequester *req);

#ifdef __cplusplus
}
#endif

#endif // IDR_REQUESTER_H

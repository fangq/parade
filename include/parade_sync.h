/*
 * Parade real-time collaboration: a document bound to a shared CRDT replica
 *
 * Built only with `make SYNC=yrs` (the yrs library, a Rust CRDT with a C
 * API). Each editor's document gets a pd_sync. Local edits become updates
 * for the others; updates received are merged and the document changes to
 * match, so every replica that has seen the same updates shows the same
 * document whatever order they arrived in. Updates may be lost, repeated or
 * reordered on the way: re-sending is always safe, and a replica that was
 * away catches up by sending its state vector and applying the diff it is
 * answered with.
 *
 * Shared: the block tree, paragraph text with its character formatting
 * (each property merged on its own), inline objects and the pictures they
 * show, paragraph and block properties, styles, lists, tracked changes.
 * Not yet: comments, metadata, per-user undo (undo reverts remote edits
 * too).
 */

#ifndef PARADE_SYNC_H
#define PARADE_SYNC_H

#include "parade_doc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pd_sync pd_sync;

/** receives an update to send to the others (a server or peers); valid for the call */
typedef void (*pd_sync_send_fn)(void* user, const void* update, size_t len);

/**
 * Bind a document to a new replica. client identifies this replica among
 * everyone who has ever edited the document (0: a random one); two live
 * replicas with one id corrupt the shared state. The first editor then
 * calls pd_sync_publish; the others apply the shared state as an update
 * before editing, and their document is replaced by it.
 */
PD_API pd_status pd_sync_new(pd_doc* doc, uint64_t client, pd_sync** out);
/** unbind; the document stays as it is */
PD_API void      pd_sync_free(pd_sync* sync);
/** make the document's content the shared state (on a replica that has seen nothing yet) */
PD_API pd_status pd_sync_publish(pd_sync* sync);
/** where updates go: called after each local operation that changed something shared */
PD_API void      pd_sync_set_sender(pd_sync* sync, pd_sync_send_fn fn, void* user);
/** merge an update from another replica into this one, and the document with it */
PD_API pd_status pd_sync_receive(pd_sync* sync, const void* update, size_t len);
/** what this replica has seen, for another to answer with pd_sync_diff */
PD_API pd_status pd_sync_state_vector(pd_sync* sync, void** out, size_t* len);
/** what this replica has that the owner of a state vector lacks; sv NULL: everything */
PD_API pd_status pd_sync_diff(pd_sync* sync, const void* sv, size_t sv_len, void** out, size_t* len);
/** release what pd_sync_state_vector, pd_sync_diff or pd_sync_dump returned */
PD_API void      pd_sync_free_data(void* data);
/**
 * A canonical text of the document as the replica shares it (from_shared
 * 0: read from the document, 1: from the shared state), the same on every
 * replica that has seen the same updates. For tests and diagnostics.
 */
PD_API char*     pd_sync_dump(pd_sync* sync, int from_shared);

#ifdef __cplusplus
}
#endif

#endif /* PARADE_SYNC_H */

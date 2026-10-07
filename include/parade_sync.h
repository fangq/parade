/*
 * Parade real-time collaboration: a document bound to a shared CRDT replica
 *
 * Built only with `make SYNC=yrs` (the yrs library, a Rust CRDT with a C
 * API). Each editor's document gets a pd_sync. Local edits become updates
 * for the others; updates received are merged and the document changes to
 * match, so every replica that has seen the same updates shows the same
 * document. Updates may be lost or repeated on the way: re-sending is
 * always safe, and a replica that was away catches up by sending its state
 * vector and applying the diff it is answered with.
 *
 * Shared: the block tree, paragraph text with its character formatting
 * (each property merged on its own), inline objects and the pictures they
 * show, paragraph and block properties, styles, lists, tracked changes,
 * comments with replies (their ranges anchored to the text). Not yet:
 * metadata.
 *
 * Updates must reach each replica in an order that keeps every update
 * after the ones it depends on -- as a server or relay forwarding them in
 * the order received does (yrs 0.28 loses an update that comes before one
 * it depends on).
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
/**
 * Undo and redo this replica's own edits only, leaving everyone else's;
 * the others are sent the change like any edit. With a pd_sync, a host
 * calls these in place of pd_doc_undo and pd_doc_redo.
 */
PD_API pd_status pd_sync_undo(pd_sync* sync);
PD_API pd_status pd_sync_redo(pd_sync* sync);
PD_API int32_t   pd_sync_can_undo(pd_sync* sync);
PD_API int32_t   pd_sync_can_redo(pd_sync* sync);

/**
 * A position as every replica can read it -- the paragraph's shared key
 * and the byte offset -- for showing where the others' carets are; and
 * back into this document (PD_ERR_RANGE when that paragraph is not here).
 */
PD_API pd_status pd_sync_pos_share(const pd_sync* sync, pd_pos pos, char* key, size_t cap, uint32_t* offset);
PD_API pd_status pd_sync_pos_local(const pd_sync* sync, const char* key, uint32_t offset, pd_pos* out);

/**
 * Updates merged into one, as a relay compacting its log does: applied in
 * order to an empty yrs document (the first is usually the previous
 * merge), whose whole state comes out as one update. PD_ERR_FORMAT when one
 * does not apply or one is missing that another depends on (then *out is
 * NULL). Release *out with pd_sync_free_data.
 */
PD_API pd_status pd_sync_merge(const void* const* updates, const size_t* lens, size_t n, void** out, size_t* len);
/** release what pd_sync_state_vector, pd_sync_diff, pd_sync_merge or pd_sync_dump returned */
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

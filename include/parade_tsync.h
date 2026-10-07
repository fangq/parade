/*
 * Parade -- plain text shared between editors (optional, built with SYNC=yrs).
 *
 * pd_sync shares a rich document; this shares a plain text, for a code or
 * text editor: one yrs (Yjs) text, the same relay and the same updates on
 * the wire. Positions are UTF-16 code units from the start of the text,
 * a line break counting one -- what yrs indexes text by, and what keeps
 * offsets right next to characters of several bytes.
 *
 *   the host                          pd_tsync
 *   typed, deleted, pasted   --->     pd_tsync_insert / pd_tsync_delete   ---> sender: an update to send
 *   applies change(at, ...)  <---     pd_tsync_receive (an update from the relay), pd_tsync_undo/redo
 *
 * The listener is told each change the text took that the host did not
 * make, in order, each at a position in the host's text as it is by then:
 * apply them one after the other and the host's text is the shared one.
 */
#ifndef PARADE_TSYNC_H
#define PARADE_TSYNC_H

#include <stddef.h>
#include <stdint.h>
#include "parade.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pd_tsync pd_tsync;

typedef void (*pd_tsync_send_fn)(void* user, const void* update, size_t len);
/** removed code units at at go, then inserted (UTF-8, len bytes) goes in there */
typedef void (*pd_tsync_change_fn)(void* user, uint32_t at, uint32_t removed, const char* inserted, size_t len);

/** a shared text, empty; client 0 picks one at random */
PD_API pd_status pd_tsync_new(uint64_t client, pd_tsync** out);
PD_API void      pd_tsync_free(pd_tsync* t);
PD_API void      pd_tsync_set_sender(pd_tsync* t, pd_tsync_send_fn fn, void* user);
PD_API void      pd_tsync_set_listener(pd_tsync* t, pd_tsync_change_fn fn, void* user);

/** the text a sharer starts it with (sent, not undoable) */
PD_API pd_status pd_tsync_publish(pd_tsync* t, const char* utf8, size_t len);
/** an update from the relay, in the relay's order; the listener is told what it changed */
PD_API pd_status pd_tsync_receive(pd_tsync* t, const void* update, size_t len);

/** what the host did to its text: inserted UTF-8 at a position, or count code units deleted from one */
PD_API pd_status pd_tsync_insert(pd_tsync* t, uint32_t at, const char* utf8, size_t len);
PD_API pd_status pd_tsync_delete(pd_tsync* t, uint32_t at, uint32_t count);

/** this replica's own edits undone and done again (typing within a moment is one step); the listener is told */
PD_API pd_status pd_tsync_undo(pd_tsync* t);
PD_API pd_status pd_tsync_redo(pd_tsync* t);
PD_API int32_t   pd_tsync_can_undo(pd_tsync* t);
PD_API int32_t   pd_tsync_can_redo(pd_tsync* t);
/** the undo step under way ends here: the next edit starts another (a host calls this when the caret jumps) */
PD_API void      pd_tsync_seal(pd_tsync* t);
/** what came before is not undoable (after a join: the text as the others left it) */
PD_API void      pd_tsync_clear_undo(pd_tsync* t);

/** the shared text (UTF-8, NUL-terminated; release with pd_tsync_free_data), and its length in code units */
PD_API char*     pd_tsync_text(pd_tsync* t);
PD_API uint32_t  pd_tsync_length(pd_tsync* t);
PD_API void      pd_tsync_free_data(void* data);

/** the code units of a UTF-8 run: what a host counts positions in */
PD_API uint32_t  pd_tsync_units(const char* utf8, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PARADE_TSYNC_H */

#include "dispatch.h"

void dispatch_init(Dispatcher *d, SpscRing **rings, int n, DispatchKind policy, bool drop,
                   atomic_bool *stop)
{
    d->rings = rings;
    d->n = n;
    d->policy = policy;
    d->drop = drop;
    d->next = 0;
    d->stop = stop;
    atomic_init(&d->dropped_lines, 0);
    atomic_init(&d->dropped_batches, 0);
}

int dispatch_pick(Dispatcher *d)
{
    int pick = d->next;
    if (d->policy == DISPATCH_LEAST) {
        /* The sizes are snapshots (the workers keep popping), which is all a
         * load-balancing heuristic needs. */
        size_t best = spsc_size(d->rings[pick]);
        for (int k = 1; k < d->n && best > 0; k++) {
            int i = (d->next + k) % d->n;
            size_t s = spsc_size(d->rings[i]);
            if (s < best) {
                best = s;
                pick = i;
            }
        }
    }
    d->next = (pick + 1) % d->n;
    return pick;
}

DispatchResult dispatch_job(Dispatcher *d, Job *job)
{
    SpscRing *ring = d->rings[dispatch_pick(d)];
    if (d->drop) {
        if (spsc_try_push(ring, job))
            return DISPATCH_SENT;
        atomic_fetch_add_explicit(&d->dropped_lines, job->batch.count, memory_order_relaxed);
        atomic_fetch_add_explicit(&d->dropped_batches, 1, memory_order_relaxed);
        return DISPATCH_DROPPED;
    }
    return spsc_push(ring, job, d->stop) ? DISPATCH_SENT : DISPATCH_STOPPED;
}

bool dispatch_eof(Dispatcher *d, Job *eof)
{
    for (int i = 0; i < d->n; i++)
        if (!spsc_push(d->rings[i], eof, d->stop))
            return false;
    return true;
}

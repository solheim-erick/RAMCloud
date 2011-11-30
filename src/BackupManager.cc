/* Copyright (c) 2009-2011 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "BackupClient.h"
#include "BackupManager.h"
#include "CycleCounter.h"
#include "ShortMacros.h"
#include "RawMetrics.h"
#include "Segment.h"

namespace RAMCloud {

/**
 * Create a BackupManager.
 * \param coordinator
 *      Cluster coordinator. May be NULL for testing purposes.
 * \param masterId
 *      Server id of master that this will be managing replicas for.
 * \param numReplicas
 *      Number replicas to keep of each segment.
 */
BackupManager::BackupManager(CoordinatorClient* coordinator,
                             const Tub<uint64_t>& masterId,
                             uint32_t numReplicas)
    : numReplicas(numReplicas)
    , backupSelector(coordinator)
    , masterId(masterId)
    , coordinator(coordinator)
    , replicatedSegmentPool(ReplicatedSegment::sizeOf(numReplicas))
    , replicatedSegmentList()
    , taskManager()
    , outstandingRpcs(0)
    , activeTime()
{
}

/**
 * Create a BackupManager.
 * This manager is constructed the same way as a previous manager.
 * This is used, for instance, by the LogCleaner to obtain a private
 * BackupManager that is configured equivalently to the Log's own
 * manager (without having to share the two).
 *
 * TODO: This is completely broken and needs to be done away with.
 * TODO: Eliminate #coordinator when this is fixed.
 * 
 * \param prototype
 *      The BackupManager that serves as a prototype for this newly
 *      created one. The same masterId, number of replicas, and
 *      coordinator are used.
 */
BackupManager::BackupManager(BackupManager* prototype)
    : numReplicas(prototype->numReplicas)
    , backupSelector(prototype->coordinator)
    , masterId(prototype->masterId)
    , coordinator(prototype->coordinator)
    , replicatedSegmentPool(ReplicatedSegment::sizeOf(numReplicas))
    , replicatedSegmentList()
    , taskManager()
    , outstandingRpcs(0)
    , activeTime()
{
}

BackupManager::~BackupManager()
{
    sync();
    // sync() is insufficient, may have outstanding frees, etc. Done below.
    while (!taskManager.isIdle())
        proceed();
    while (!replicatedSegmentList.empty())
        destroyAndFreeReplicatedSegment(&replicatedSegmentList.front());
}

/**
 * Ask backups to discard a segment.
 *
 * TODO: Deprecated in favor of ReplicatedSegment::free().
 */
void
BackupManager::freeSegment(uint64_t segmentId)
{
    CycleCounter<RawMetric> _(&metrics->master.backupManagerTicks);

    // TODO: Don't allow free on an open segment. (Already enforced in new
    // interface, should just work once we can delete this method).

    // Note: cannot use foreach since proceed() can delete elements
    // of replicatedSegmentList.
    auto it = replicatedSegmentList.begin();
    while (it != replicatedSegmentList.end()) {
        it->free();
        ++it;
        while (!taskManager.isIdle())
            proceed();
    }
}

/**
 * Begin replicating a segment on backups.  Allocates and returns a
 * ReplicatedSegment which acts as a handle for the log module to perform
 * future operations related to this segment (like queueing more data for
 * replication, waiting for data to be replicated, or freeing replicas).
 *
 * \param segmentId
 *      A unique identifier for this segment. The caller must ensure a
 *      segment with this segmentId is not already open.
 * \param data
 *      Starting location of the raw segment data to be replicated.
 * \param len
 *      Number of bytes to send atomically to backups with open segment RPC.
 * \return
 *      Pointer to a ReplicatedSegment that is valid until
 *      ReplicatedSegment::free() is called on it.
 */
ReplicatedSegment*
BackupManager::openSegment(uint64_t segmentId, const void* data, uint32_t len)
{
    CycleCounter<RawMetric> _(&metrics->master.backupManagerTicks);
    LOG(DEBUG, "openSegment %lu, %lu, ..., %u", *masterId, segmentId, len);
    auto* p = replicatedSegmentPool.malloc();
    if (p == NULL)
        DIE("Out of memory");
    auto* replicatedSegment =
        new(p) ReplicatedSegment(taskManager, backupSelector,
                                 *this,
                                 *masterId, segmentId,
                                 data, len, numReplicas);
    replicatedSegmentList.push_back(*replicatedSegment);
    replicatedSegment->schedule();
    return replicatedSegment;
}

/**
 * Make progress on replicating the log to backups, but don't block.
 * This method checks for completion of outstanding backup operations and
 * starts new ones when possible.
 */
void
BackupManager::proceed()
{
    CycleCounter<RawMetric> _(&metrics->master.backupManagerTicks);
    taskManager.proceed();
}

/**
 * Wait until all written data has been acknowledged by the backups for all
 * segments.
 */
void
BackupManager::sync()
{
    {
        CycleCounter<RawMetric> _(&metrics->master.backupManagerTicks);
        while (!isSynced()) {
            taskManager.proceed();
        }
    } // block ensures that _ is destroyed and counter stops
    // TODO: may need to rename this (outstandingWriteRpcs?)
    assert(outstandingRpcs == 0);
}

// - private -

/**
 * Respond to a change in cluster configuration by scheduling any work that
 * is needed to restore durablity guarantees.  The work is queued into
 * #taskManager which is then executed during calls to #proceed().  One call
 * is sufficient since tasks reschedule themselves until all guarantees are
 * restored.
 */
void
BackupManager::clusterConfigurationChanged()
{
    foreach (auto& segment, replicatedSegmentList)
        segment.schedule();
}

/**
 * Internal helper for #sync().
 * Returns true when all data queued for replication by the log module is
 * durably replicated.
 */
bool
BackupManager::isSynced()
{
    foreach (auto& segment, replicatedSegmentList) {
        if (!segment.isSynced())
            return false;
    }
    return true;
}

/**
 * Invoked by ReplicatedSegment to indicate that the BackupManager no longer
 * needs to keep an information about this segment (for example, when all
 * replicas are freed on backups or during shutdown).
 * Only used by ReplicatedSegment.
 */
void
BackupManager::destroyAndFreeReplicatedSegment(ReplicatedSegment*
                                                    replicatedSegment)
{
    assert(!replicatedSegment->isScheduled());
    erase(replicatedSegmentList, *replicatedSegment);
    replicatedSegment->~ReplicatedSegment();
    replicatedSegmentPool.free(replicatedSegment);
}

} // namespace RAMCloud

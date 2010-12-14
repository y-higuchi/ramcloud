/* Copyright (c) 2009-2010 Stanford University
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

#include <boost/scoped_ptr.hpp>

#include "Buffer.h"
#include "ClientException.h"
#include "MasterServer.h"
#include "ObjectTub.h"
#include "ProtoBuf.h"
#include "Rpc.h"
#include "Segment.h"
#include "SegmentIterator.h"
#include "Transport.h"
#include "TransportManager.h"

namespace RAMCloud {

// --- SegmentLocatorChooser ---

/**
 * \param list
 *      A list of servers along with the segment id they have backed up.
 *      See Recovery for details on this format.
 */
SegmentLocatorChooser::SegmentLocatorChooser(const ProtoBuf::ServerList& list)
    : map()
    , ids()
{
    foreach (const ProtoBuf::ServerList::Entry& server, list.server()) {
        if (!server.has_segment_id()) {
            LOG(WARNING,
                "List of backups for recovery must contain segmentIds");
            continue;
        }
        if (server.server_type() != ProtoBuf::BACKUP) {
            LOG(WARNING,
                "List of backups for recovery shouldn't contain MASTERs");
            continue;
        }
        map.insert(make_pair(server.segment_id(), server.service_locator()));
    }
    std::transform(map.begin(), map.end(), back_inserter(ids),
                   &first<uint64_t, string>);
    // not the most efficient approach in the world...
    SegmentIdList::iterator newEnd = std::unique(ids.begin(), ids.end());
    ids.erase(newEnd, ids.end());
    std::random_shuffle(ids.begin(), ids.end());
}

/**
 * Provide locators for potential backup server to contact to find
 * segment data during recovery.
 *
 * \param segmentId
 *      A segment id for a segment that needs to be recovered.
 * \return
 *      A service locator string indicating a location where this segment
 *      can be recovered from.
 * \throw SegmentRecoveryFailedException
 *      If the requested segment id has no remaining potential backup
 *      locations.
 */
const string&
SegmentLocatorChooser::get(uint64_t segmentId)
{
    LocatorMap::size_type count = map.count(segmentId);

    ConstLocatorRange range = map.equal_range(segmentId);
    if (range.first == range.second)
        throw SegmentRecoveryFailedException(HERE);

    // Using rdtsc() as a fast random number generator. Perhaps a bad idea.
    LocatorMap::size_type random = rdtsc() % count;
    LocatorMap::const_iterator it = range.first;
    for (LocatorMap::size_type i = 0; i < random; ++i)
        ++it;
    return it->second;
}

/**
 * Returns a randomly ordered list of segment ids which acts as a
 * schedule for recovery.
 */
const SegmentLocatorChooser::SegmentIdList&
SegmentLocatorChooser::getSegmentIdList()
{
    return ids;
}

/**
 * Remove the locator as a potential backup location for a particular
 * segment.
 *
 * \param segmentId
 *      The id of a segment which could not be located at the specified
 *      backup locator.
 * \param locator
 *      The locator string that should not be returned from future calls
 *      to get for this particular #segmentId.
 */
void
SegmentLocatorChooser::markAsDown(uint64_t segmentId, const string& locator)
{
    LocatorRange range = map.equal_range(segmentId);
    if (range.first == map.end())
        return;

    for (LocatorMap::iterator it = range.first;
         it != range.second; ++it)
    {
        if (it->second == locator) {
            map.erase(it);
            return;
        }
    }
}

// --- MasterServer ---

void objectEvictionCallback(LogEntryType type,
                            const void* p,
                            uint64_t len,
                            void* cookie);
void tombstoneEvictionCallback(LogEntryType type,
                               const void* p,
                               uint64_t len,
                               void* cookie);

/**
 * Construct a MasterServer.
 *
 * \param config
 *      Contains various parameters that configure the operation of
 *      this server.
 * \param coordinator
 *      A client to the coordinator for the RAMCloud this Master is in.
 * \param replicas
 *      The number of backups required before writes are considered safe.
 */
MasterServer::MasterServer(const ServerConfig config,
                           CoordinatorClient* coordinator,
                           uint32_t replicas)
    : config(config)
    , coordinator(coordinator)
    // Permit a NULL coordinator for testing/benchmark purposes.
    , serverId(coordinator ? coordinator->enlistServer(MASTER,
                                                       config.localLocator)
                           : 0)
    , backup(coordinator, serverId, replicas)
    , bytesWritten(0)
    , log(serverId, config.logBytes, Segment::SEGMENT_SIZE, &backup)
    , objectMap(config.hashTableBytes / ObjectMap::bytesPerCacheLine())
    , tablets()
{
    LOG(NOTICE, "My server ID is %lu", serverId);
    log.registerType(LOG_ENTRY_TYPE_OBJ, objectEvictionCallback, this);
    log.registerType(LOG_ENTRY_TYPE_OBJTOMB, tombstoneEvictionCallback, this);
}

MasterServer::~MasterServer()
{
    std::set<Table*> tables;
    foreach (const ProtoBuf::Tablets::Tablet& tablet, tablets.tablet())
        tables.insert(reinterpret_cast<Table*>(tablet.user_data()));
    foreach (Table* table, tables)
        delete table;
}

void
MasterServer::dispatch(RpcType type, Transport::ServerRpc& rpc,
                       Responder& responder)
{
    switch (type) {
        case CreateRpc::type:
            callHandler<CreateRpc, MasterServer,
                        &MasterServer::create>(rpc);
            break;
        case PingRpc::type:
            callHandler<PingRpc, MasterServer,
                        &MasterServer::ping>(rpc);
            break;
        case ReadRpc::type:
            callHandler<ReadRpc, MasterServer,
                        &MasterServer::read>(rpc);
            break;
        case RecoverRpc::type:
            callHandler<RecoverRpc, MasterServer,
                        &MasterServer::recover>(rpc, responder);
            break;
        case RemoveRpc::type:
            callHandler<RemoveRpc, MasterServer,
                        &MasterServer::remove>(rpc);
            break;
        case SetTabletsRpc::type:
            callHandler<SetTabletsRpc, MasterServer,
                        &MasterServer::setTablets>(rpc);
            break;
        case WriteRpc::type:
            callHandler<WriteRpc, MasterServer,
                        &MasterServer::write>(rpc);
            break;
        default:
            throw UnimplementedRequestError(HERE);
    }
}

void __attribute__ ((noreturn))
MasterServer::run()
{
    while (true)
        handleRpc<MasterServer>();
}

/**
 * Top-level server method to handle the CREATE request.
 * See the documentation for the corresponding method in RamCloudClient for
 * complete information about what this request does.
 * \copydetails Server::ping
 */
void
MasterServer::create(const CreateRpc::Request& reqHdr,
                     CreateRpc::Response& respHdr,
                     Transport::ServerRpc& rpc)
{
    Table& t(getTable(reqHdr.tableId, ~0UL));
    uint64_t id = t.AllocateKey(&objectMap);

    RejectRules rejectRules;
    memset(&rejectRules, 0, sizeof(RejectRules));
    rejectRules.exists = 1;

    storeData(reqHdr.tableId, id, &rejectRules,
              &rpc.recvPayload, sizeof(reqHdr), reqHdr.length,
              &respHdr.version);
    respHdr.id = id;
}

/**
 * Top-level server method to handle the PING request.
 *
 * For debugging it print out statistics on the RPCs that it has
 * handled along with some stats on amount of data written to the
 * master.
 *
 * \copydetails Server::ping
 */
void
MasterServer::ping(const PingRpc::Request& reqHdr,
                   PingRpc::Response& respHdr,
                   Transport::ServerRpc& rpc)
{
    LOG(NOTICE, "Bytes written: %lu", bytesWritten);
    LOG(NOTICE, "Bytes logged : %lu", log.getBytesAppended());

    Server::ping(reqHdr, respHdr, rpc);
}

/**
 * Top-level server method to handle the READ request.
 * \copydetails create
 */
void
MasterServer::read(const ReadRpc::Request& reqHdr,
                   ReadRpc::Response& respHdr,
                   Transport::ServerRpc& rpc)
{
    // We must throw an exception if the table does not exist. Also, we might
    // have an entry in the hash table that's invalid because its tablet no
    // longer lives here.
    getTable(reqHdr.tableId, reqHdr.id);

    const Object* o = objectMap.lookup(reqHdr.tableId, reqHdr.id);
    if (!o) {
        throw ObjectDoesntExistException(HERE);
    }

    respHdr.version = o->version;
    rejectOperation(&reqHdr.rejectRules, o->version);
    Buffer::Chunk::appendToBuffer(&rpc.replyPayload,
                                  o->data, static_cast<uint32_t>(o->data_len));
    // TODO(ongaro): We'll need a new type of Chunk to block the cleaner
    // from scribbling over o->data.
    respHdr.length = o->data_len;
}

/**
 * Callback used to purge the recovery tombstone hash table. Invoked by
 * HashTable::forEach.
 */
static void
recoveryCleanup(const ObjectTombstone *tomb, void *cookie)
{
    free(const_cast<ObjectTombstone *>(tomb));
}

// used in recover()
struct Task {
    Task(uint64_t masterId, uint64_t segmentId,
         const char* backupLocator, const ProtoBuf::Tablets& tablets)
        : segmentId(segmentId)
        , backupLocator(backupLocator)
        , response()
        , client(transportManager.getSession(backupLocator))
        , rpc(client, masterId, segmentId, tablets, response)
    {}
    uint64_t segmentId;
    const char* backupLocator;
    Buffer response;
    BackupClient client;
    BackupClient::GetRecoveryData rpc;
    DISALLOW_COPY_AND_ASSIGN(Task);
};

/**
 * Helper for public recover() method.
 * Collect all the filtered log segments from backups for a set of tablets
 * formerly belonging to a crashed master which is being recovered and pass
 * them to the recovery master to have them replayed.
 *
 * \param masterId
 *      The id of the crashed master for which recoveryMaster will be taking
 *      over ownership of tablets.
 * \param tablets
 *      A set of tables with key ranges describing which poritions of which
 *      tables recoveryMaster should have replayed to it.
 * \param backups
 *      A list of backup locators along with a segmentId specifying for each
 *      segmentId a backup who can provide a filtered recovery data segment.
 *      A particular segment may be listed more than once if it has multiple
 *      viable backups, hence a particular backup locator can also be listed
 *      many times.
 * \param tombstoneMap
 *      (table id, object id) to ObjectTombstone map used during recovery.
 */
void
MasterServer::recover(uint64_t masterId,
                      const ProtoBuf::Tablets& tablets,
                      const ProtoBuf::ServerList& backups,
                      ObjectTombstoneMap& tombstoneMap)
{
    LOG(NOTICE, "Recovering master %lu, %u tablets, %u hosts",
        masterId, tablets.tablet_size(), backups.server_size());

#if TESTING
    if (!mockRandomValue)
        srand(rdtsc());
    else
        srand(0);
#else
    srand(rdtsc());
#endif

#ifdef PERF_DEBUG_RECOVERY_SERIAL
    ObjectTub<Task> tasks[1];
#else
    ObjectTub<Task> tasks[4];
#endif

    SegmentLocatorChooser chooser(backups);
    auto segIdsIt = chooser.getSegmentIdList().begin();
    auto segIdsEnd = chooser.getSegmentIdList().end();
    uint32_t activeSegments = 0;

    // Start RPCs
    foreach (auto& task, tasks) {
        if (segIdsIt == segIdsEnd)
            break;
        uint64_t segmentId = *segIdsIt++;
        task.construct(masterId, segmentId,
                       chooser.get(segmentId).c_str(),
                       tablets);
        ++activeSegments;
    }

    // As RPCs complete, process them and start more
    while (activeSegments > 0) {
        foreach (auto& task, tasks) {
            if (!task || !task->rpc.isReady())
                continue;
            LOG(DEBUG, "Waiting on recovery data for segment %lu from %s",
                task->segmentId, task->backupLocator);
            try {
                (task->rpc)();
            } catch (const TransportException& e) {
                LOG(DEBUG, "Couldn't contact %s, trying next backup; "
                    "failure was: %s", task->backupLocator, e.str().c_str());
                // TODO(ongaro): try to get this segment from other backups
                throw SegmentRecoveryFailedException(HERE);
            } catch (const ClientException& e) {
                LOG(DEBUG, "getRecoveryData failed on %s, trying next backup; "
                    "failure was: %s", task->backupLocator, e.str().c_str());
                // TODO(ongaro): try to get this segment from other backups
                throw SegmentRecoveryFailedException(HERE);
            }

            uint32_t responseLen = task->response.getTotalLength();
            LOG(DEBUG, "Recovering segment %lu with size %u",
                task->segmentId, responseLen);
            recoverSegment(task->segmentId,
                           task->response.getRange(0, responseLen),
                           responseLen,
                           tombstoneMap);
            task.destroy();
            if (segIdsIt == segIdsEnd) {
                --activeSegments;
                continue;
            }
            uint64_t segmentId = *segIdsIt++;
            task.construct(masterId, segmentId,
                           chooser.get(segmentId).c_str(),
                           tablets);
        }
    }

    log.sync();
}

/**
 * Top-level server method to handle the RECOVER request.
 * \copydetails Server::ping
 * \param responder
 *      Functor to respond to the RPC before returning from this method.
 */
void
MasterServer::recover(const RecoverRpc::Request& reqHdr,
                      RecoverRpc::Response& respHdr,
                      Transport::ServerRpc& rpc,
                      Responder& responder)
{
    uint64_t masterId = reqHdr.masterId;
    ProtoBuf::Tablets recoveryTablets;
    ProtoBuf::parseFromResponse(rpc.recvPayload, sizeof(reqHdr),
                                reqHdr.tabletsLength, recoveryTablets);
    ProtoBuf::ServerList backups;
    ProtoBuf::parseFromResponse(rpc.recvPayload,
                                sizeof(reqHdr) + reqHdr.tabletsLength,
                                reqHdr.serverListLength, backups);
    LOG(DEBUG, "Starting recovery of %u tablets on masterId %lu",
        recoveryTablets.tablet_size(), serverId);
    responder();

    // reqHdr, respHdr, and rpc are off-limits now

    {
        // Allocate a recovery hash table for the tombstones.
        static_assert(sizeof(ObjectTombstoneMap) < 1024,
                      "ObjectTombstoneMap is big");
        ObjectTombstoneMap tombstoneMap(64 * 1024 * 1024 /
            ObjectTombstoneMap::bytesPerCacheLine());

        // Recover Segments, firing MasterServer::recoverSegment for each one.
        recover(masterId, recoveryTablets, backups, tombstoneMap);

        // Free recovery tombstones left in the hash table and deallocate it.
        tombstoneMap.forEach(recoveryCleanup, NULL);
    }

    // Once the coordinator and the recovery master agree that the
    // master has taken over for the tablets it can update its tables
    // and begin serving requests.

    // Update the recoveryTablets to reflect the fact that this master is
    // going to try to become the owner.
    foreach (ProtoBuf::Tablets::Tablet& tablet,
             *recoveryTablets.mutable_tablet()) {
        LOG(NOTICE, "set tablet %lu %lu %lu to locator %s, id %lu",
                 tablet.table_id(), tablet.start_object_id(),
                 tablet.end_object_id(), config.localLocator.c_str(), serverId);
        tablet.set_service_locator(config.localLocator);
        tablet.set_server_id(serverId);
    }

    coordinator->tabletsRecovered(recoveryTablets);
    // Ok - we're free to start serving now.

    // Union the new tablets into an updated tablet map
    ProtoBuf::Tablets newTablets(tablets);
    newTablets.mutable_tablet()->MergeFrom(recoveryTablets.tablet());
    // and set ourself as open for business.
    setTablets(newTablets);
    // TODO(stutsman) update local copy of the will
}

/**
 * Given a SegmentIterator for the Segment we're currently recovering,
 * advance it and issue prefetches on the hash tables. This is used
 * exclusively by recoverSegment().
 *
 * \param[in] i
 *      A SegmentIterator to use for prefetching. Note that this
 *      method modifies the iterator, so the caller should not use
 *      it for its own iteration.
 * \param tombstoneMap
 *      (table id, object id) to ObjectTombstone map used during recovery.
 */
void
MasterServer::recoverSegmentPrefetcher(SegmentIterator& i,
                                       ObjectTombstoneMap& tombstoneMap)
{
    i.next();

    if (i.isDone())
        return;

    LogEntryType type = i.getType();
    uint64_t objId = ~0UL, tblId = ~0UL;

    if (type == LOG_ENTRY_TYPE_OBJ) {
        const Object *recoverObj = reinterpret_cast<const Object *>(
                     i.getPointer());
        objId = recoverObj->id;
        tblId = recoverObj->table;
    } else if (type == LOG_ENTRY_TYPE_OBJTOMB) {
        const ObjectTombstone *recoverTomb =
            reinterpret_cast<const ObjectTombstone *>(i.getPointer());
        objId = recoverTomb->objectId;
        tblId = recoverTomb->tableId;
    } else {
        return;
    }

    objectMap.prefetch(tblId, objId);
    tombstoneMap.prefetch(tblId, objId);
}

/**
 * Replay a filtered segment from a crashed Master that this Master is taking
 * over for.
 *
 * \param segmentId
 *      The segmentId of the segment as it was in the log of the crashed Master.
 * \param buffer 
 *      A pointer to a valid segment which has been pre-filtered of all
 *      objects except those that pertain to the tablet ranges this Master
 *      will be responsible for after the recovery completes.
 * \param bufferLength
 *      Length of the buffer in bytes.
 * \param tombstoneMap
 *      (table id, object id) to ObjectTombstone map used during recovery.
 */
void
MasterServer::recoverSegment(uint64_t segmentId, const void *buffer,
    uint64_t bufferLength, ObjectTombstoneMap& tombstoneMap)
{
    LOG(DEBUG, "recoverSegment %lu, ...", segmentId);

    SegmentIterator i(buffer, bufferLength, true);
#ifndef PERF_DEBUG_RECOVERY_REC_SEG_NO_PREFETCH
    SegmentIterator prefetch(buffer, bufferLength, true);
#endif

#ifdef PERF_DEBUG_RECOVERY_REC_SEG_JUST_ITER
    for (; !i.isDone(); i.next());
    return;
#endif
    while (!i.isDone()) {
        LogEntryType type = i.getType();

#ifndef PERF_DEBUG_RECOVERY_REC_SEG_NO_PREFETCH
        recoverSegmentPrefetcher(prefetch, tombstoneMap);
#endif

        if (type == LOG_ENTRY_TYPE_OBJ) {
            const Object *recoverObj = reinterpret_cast<const Object *>(
                i.getPointer());
            uint64_t objId = recoverObj->id;
            uint64_t tblId = recoverObj->table;

#ifdef PERF_DEBUG_RECOVERY_REC_SEG_NO_HT
            const Object *localObj = 0;
            const ObjectTombstone *tomb = 0;
#else
            const Object *localObj = objectMap.lookup(tblId, objId);
            const ObjectTombstone *tomb = tombstoneMap.lookup(tblId, objId);
#endif

            // can't have both a tombstone and an object in the hash tables
            assert(tomb == NULL || localObj == NULL);

            uint64_t minSuccessor = 0;
            if (localObj != NULL)
                minSuccessor = localObj->version + 1;
            else if (tomb != NULL)
                minSuccessor = tomb->objectVersion + 1;

            if (recoverObj->version >= minSuccessor) {
#ifdef PERF_DEBUG_RECOVERY_REC_SEG_NO_LOG
                const Object* newObj = localObj;
#else
                // write to log (with lazy backup flush) & update hash table
                const Object *newObj = reinterpret_cast<const Object*>(
                    log.append(LOG_ENTRY_TYPE_OBJ, recoverObj,
                                recoverObj->size(), false));
#endif

#ifndef PERF_DEBUG_RECOVERY_REC_SEG_NO_HT
                objectMap.replace(tblId, objId, newObj);
#endif

                // nuke the tombstone, if it existed
                if (tomb != NULL) {
                    tombstoneMap.remove(tblId, objId);
                    free(const_cast<ObjectTombstone *>(tomb));
                }

                // nuke the old object, if it existed
                if (localObj != NULL) {
                    log.free(localObj);
                }
            }
        } else if (type == LOG_ENTRY_TYPE_OBJTOMB) {
            const ObjectTombstone *recoverTomb =
                reinterpret_cast<const ObjectTombstone *>(i.getPointer());
            uint64_t objId = recoverTomb->objectId;
            uint64_t tblId = recoverTomb->tableId;

            const Object *localObj = objectMap.lookup(tblId, objId);
            const ObjectTombstone *tomb = tombstoneMap.lookup(tblId, objId);

            // can't have both a tombstone and an object in the hash tables
            assert(tomb == NULL || localObj == NULL);

            uint64_t minSuccessor = 0;
            if (localObj != NULL)
                minSuccessor = localObj->version;
            else if (tomb != NULL)
                minSuccessor = tomb->objectVersion + 1;

            if (recoverTomb->objectVersion >= minSuccessor) {
                // allocate memory for the tombstone & update hash table
                // TODO(ongaro): Change to new with copy constructor?
                ObjectTombstone *newTomb = reinterpret_cast<ObjectTombstone *>(
                    xmalloc(sizeof(*newTomb)));
                memcpy(newTomb, const_cast<ObjectTombstone *>(recoverTomb),
                    sizeof(*newTomb));
                tombstoneMap.replace(tblId, objId, newTomb);

                // nuke the old tombstone, if it existed
                if (tomb != NULL) {
                    free(const_cast<ObjectTombstone *>(tomb));
                }

                // nuke the object, if it existed
                if (localObj != NULL) {
                    objectMap.remove(tblId, objId);
                    log.free(localObj);
                }
            }
        }

        i.next();
    }
    LOG(NOTICE, "Segment %lu replay complete", segmentId);
}

/**
 * Top-level server method to handle the REMOVE request.
 * \copydetails create
 */
void
MasterServer::remove(const RemoveRpc::Request& reqHdr,
                     RemoveRpc::Response& respHdr,
                     Transport::ServerRpc& rpc)
{
    Table& t(getTable(reqHdr.tableId, reqHdr.id));
    const Object* o = objectMap.lookup(reqHdr.tableId, reqHdr.id);
    if (o == NULL) {
        rejectOperation(&reqHdr.rejectRules, VERSION_NONEXISTENT);
        return;
    }
    respHdr.version = o->version;

    // Abort if we're trying to delete the wrong version.
    rejectOperation(&reqHdr.rejectRules, respHdr.version);

    t.RaiseVersion(o->version + 1);

    ObjectTombstone tomb(tomb.segmentId, o);

    // Mark the deleted object as free first, since the append could
    // invalidate it
    log.free(o);
    log.append(LOG_ENTRY_TYPE_OBJTOMB, &tomb, sizeof(tomb));
    objectMap.remove(reqHdr.tableId, reqHdr.id);
}

/**
 * Set the list of tablets that this master serves.
 *
 * Notice that this method does nothing about the objects and data
 * for a particular tablet.  That is, the log and hashtable must already
 * contain a consistent view of the tablet before being set as an active
 * tablet with this method.
 *
 * \param newTablets
 *      The new set of tablets this master is serving.
 */
void
MasterServer::setTablets(const ProtoBuf::Tablets& newTablets)
{
    typedef std::map<uint32_t, Table*> Tables;
    Tables tables;

    // create map from table ID to Table of pre-existing tables
    foreach (const ProtoBuf::Tablets::Tablet& oldTablet, tablets.tablet()) {
        tables[oldTablet.table_id()] =
            reinterpret_cast<Table*>(oldTablet.user_data());
    }

    // overwrite tablets with new tablets
    tablets = newTablets;

    // delete pre-existing tables that no longer live here
#ifdef __INTEL_COMPILER
    for (Tables::iterator it(tables.begin()); it != tables.end(); ++it) {
        Tables::value_type oldTable = *it;
        for (uint32_t i = 0; i < tablets.tablet_size(); ++i) {
            const ProtoBuf::Tablets::Tablet& newTablet(tablets.tablet(i));
#else
    foreach (Tables::value_type oldTable, tables) {
        foreach (const ProtoBuf::Tablets::Tablet& newTablet,
                 tablets.tablet()) {
#endif
            if (oldTable.first == newTablet.table_id())
                goto next;
        }
        delete oldTable.second;
        oldTable.second = NULL;
      next:
        { /* pass */ }
    }

    // create new Tables and assign all new tablets tables
    LOG(NOTICE, "Now serving tablets:");
#ifdef __INTEL_COMPILER
    for (uint32_t i = 0; i < tablets.tablet_size(); ++i) {
        ProtoBuf::Tablets::Tablet& newTablet(*tablets.mutable_tablet(i));
#else
    foreach (ProtoBuf::Tablets::Tablet& newTablet, *tablets.mutable_tablet()) {
#endif
        LOG(NOTICE, "table: %20lu, start: %20lu, end  : %20lu",
            newTablet.table_id(), newTablet.start_object_id(),
            newTablet.end_object_id());
        Table* table = tables[newTablet.table_id()];
        if (table == NULL) {
            table = new Table(newTablet.table_id());
            tables[newTablet.table_id()] = table;
        }
        newTablet.set_user_data(reinterpret_cast<uint64_t>(table));
    }
}

/**
 * Top-level server method to handle the SET_TABLETS request.
 * \copydetails create
 */
void
MasterServer::setTablets(const SetTabletsRpc::Request& reqHdr,
                         SetTabletsRpc::Response& respHdr,
                         Transport::ServerRpc& rpc)
{
    ProtoBuf::Tablets newTablets;
    ProtoBuf::parseFromRequest(rpc.recvPayload, sizeof(reqHdr),
                               reqHdr.tabletsLength, newTablets);
    setTablets(newTablets);
}

/**
 * Top-level server method to handle the WRITE request.
 * \copydetails create
 */
void
MasterServer::write(const WriteRpc::Request& reqHdr,
                    WriteRpc::Response& respHdr,
                    Transport::ServerRpc& rpc)
{
    storeData(reqHdr.tableId, reqHdr.id,
              &reqHdr.rejectRules, &rpc.recvPayload, sizeof(reqHdr),
              static_cast<uint32_t>(reqHdr.length), &respHdr.version);
}

/**
 * Ensures that this master owns the tablet for the given object
 * and returns the corresponding Table.
 *
 * \param tableId
 *      Identifier for a desired table.
 * \param objectId
 *      Identifier for a desired object.
 *
 * \return
 *      The Table of which the tablet containing this object is a part.
 *
 * \exception TableDoesntExist
 *      Thrown if that tablet isn't owned by this server.
 */
// TODO(ongaro): Masters don't know whether tables exist.
// This be something like ObjectNotHereException.
Table&
MasterServer::getTable(uint32_t tableId, uint64_t objectId) {

    foreach (const ProtoBuf::Tablets::Tablet& tablet, tablets.tablet()) {
        if (tablet.table_id() == tableId &&
            tablet.start_object_id() <= objectId &&
            objectId <= tablet.end_object_id()) {
            return *reinterpret_cast<Table*>(tablet.user_data());
        }
    }
    throw TableDoesntExistException(HERE);
}

/**
 * Check a set of RejectRules against the current state of an object
 * to decide whether an operation is allowed.
 *
 * \param rejectRules
 *      Specifies conditions under which the operation should fail.
 * \param version
 *      The current version of an object, or VERSION_NONEXISTENT
 *      if the object does not currently exist (used to test rejectRules)
 *
 * \return
 *      The return value is STATUS_OK if none of the reject rules
 *      indicate that the operation should be rejected. Otherwise
 *      the return value indicates the reason for the rejection.
 */
void
MasterServer::rejectOperation(const RejectRules* rejectRules, uint64_t version)
{
    if (version == VERSION_NONEXISTENT) {
        if (rejectRules->doesntExist)
            throw ObjectDoesntExistException(HERE);
        return;
    }
    if (rejectRules->exists)
        throw ObjectExistsException(HERE);
    if (rejectRules->versionLeGiven && version <= rejectRules->givenVersion)
        throw WrongVersionException(HERE);
    if (rejectRules->versionNeGiven && version != rejectRules->givenVersion)
        throw WrongVersionException(HERE);
}

//-----------------------------------------------------------------------
// Everything below here is "old" code, meaning it probably needs to
// get refactored at some point, it doesn't follow the coding conventions,
// and there are no unit tests for it.
//-----------------------------------------------------------------------

struct obj_replay_cookie {
    MasterServer *server;
    uint64_t usedBytes;
};

/**
 * Callback used by the LogCleaner when it's cleaning a Segment and evicts
 * an Object (i.e. an entry of type LOG_ENTRY_TYPE_OBJ).
 *
 * Upon return, the object will be discarded. Objects must therefore be
 * perpetuated when the object being evicted is exactly the object referenced
 * by the hash table. Otherwise, it's an old object and a tombstone for it
 * exists.
 *
 * \param[in]  type
 *      LogEntryType of the evictee (LOG_ENTRY_TYPE_OBJ).
 * \param[in]  p
 *      Opaque pointer to the immutable entry in the log. 
 * \param[in]  len
 *      Size of the log entry being evicted in bytes.
 * \param[in]  cookie
 *      The opaque state pointer registered with the callback.
 */
void
objectEvictionCallback(LogEntryType type,
                       const void* p,
                       uint64_t len,
                       void* cookie)
{
    assert(type == LOG_ENTRY_TYPE_OBJ);

    MasterServer *svr = static_cast<MasterServer *>(cookie);
    assert(svr != NULL);

    Log& log = svr->log;

    const Object *evictObj = static_cast<const Object *>(p);
    assert(evictObj != NULL);

    try {
        svr->getTable(evictObj->table, evictObj->id);
    } catch (TableDoesntExistException& e) {
        // That tablet doesn't exist on this server anymore.
        // Just remove the hash table entry, if it exists.
        svr->objectMap.remove(evictObj->table, evictObj->id);
        return;
    }

    const Object *hashTblObj =
        svr->objectMap.lookup(evictObj->table, evictObj->id);

    // simple pointer comparison suffices
    if (hashTblObj == evictObj) {
        const Object *newObj = (const Object *)log.append(
            LOG_ENTRY_TYPE_OBJ, evictObj, evictObj->size());
        svr->objectMap.replace(evictObj->table, evictObj->id, newObj);
    }
}

void
objectReplayCallback(LogEntryType type,
                     const void *p,
                     uint64_t len,
                     void *cookiep)
{
    obj_replay_cookie *cookie = static_cast<obj_replay_cookie *>(cookiep);
    MasterServer *server = cookie->server;

    //printf("ObjectReplayCallback: type %u\n", type);

    // Used to determine free_bytes after passing over the segment
    cookie->usedBytes += len;

    switch (type) {
    case LOG_ENTRY_TYPE_OBJ: {
        const Object *obj = static_cast<const Object *>(p);
        assert(obj);

        server->objectMap.remove(obj->table, obj->id);
        server->objectMap.replace(obj->table, obj->id, obj);
    }
        break;
    case LOG_ENTRY_TYPE_OBJTOMB:
        assert(false);  //XXX- fixme
        break;
    case LOG_ENTRY_TYPE_SEGHEADER:
    case LOG_ENTRY_TYPE_SEGFOOTER:
        break;
    default:
        printf("!!! Unknown object type on log replay: 0x%x", type);
    }
}

/**
 * Callback used by the LogCleaner when it's cleaning a Segment and evicts
 * an ObjectTombstone (i.e. an entry of type LOG_ENTRY_TYPE_OBJTOMB).
 *
 * Tombstones are perpetuated when the Segment they reference is still
 * valid in the system.
 *
 * \param[in]  type
 *      LogEntryType of the evictee (LOG_ENTRY_TYPE_OBJTOMB).
 * \param[in]  p
 *      Opaque pointer to the immutable entry in the log.
 * \param[in]  len
 *      Size of the log entry being evicted in bytes.
 * \param[in]  cookie
 *      The opaque state pointer registered with the callback.
 */
void
tombstoneEvictionCallback(LogEntryType type,
                          const void* p,
                          uint64_t len,
                          void* cookie)
{
    assert(type == LOG_ENTRY_TYPE_OBJTOMB);

    MasterServer *svr = static_cast<MasterServer *>(cookie);
    assert(svr != NULL);

    Log& log = svr->log;

    const ObjectTombstone *tomb =
        static_cast<const ObjectTombstone *>(p);
    assert(tomb != NULL);

    // see if the referant is still there
    if (log.isSegmentLive(tomb->segmentId))
        log.append(LOG_ENTRY_TYPE_OBJTOMB, tomb, sizeof(*tomb));
}

void
MasterServer::storeData(uint64_t tableId, uint64_t id,
                        const RejectRules* rejectRules, Buffer* data,
                        uint32_t dataOffset, uint32_t dataLength,
                        uint64_t* newVersion)
{
    Table& t(getTable(tableId, id));
    const Object *o = objectMap.lookup(tableId, id);
    uint64_t version = (o != NULL) ? o->version : VERSION_NONEXISTENT;
    try {
        rejectOperation(rejectRules, version);
    } catch (...) {
        *newVersion = version;
        throw;
    }

    DECLARE_OBJECT(newObject, dataLength);

    newObject->id = id;
    newObject->table = tableId;
    if (o != NULL)
        newObject->version = o->version + 1;
    else
        newObject->version = t.AllocateVersion();
    assert(o == NULL || newObject->version > o->version);
    // TODO(stutsman): dm's super-fast checksum here
    newObject->checksum = 0x0BE70BE70BE70BE7ULL;
    newObject->data_len = dataLength;
    data->copy(dataOffset, dataLength, newObject->data);

    // If the Object is being overwritten, we need to mark the previous space
    // used as free and add a tombstone that references it.
    if (o != NULL) {
        // Mark the old object as freed _before_ writing the new object to the
        // log. If we do it afterwards, the LogCleaner could be triggered and
        // `o' could be reclaimed before log->append() returns. The subsequent
        // free then breaks, as that Segment may have been cleaned.
        log.free(o);

        uint64_t segmentId = log.getSegmentId(o);
        ObjectTombstone tomb(segmentId, o);
        log.append(LOG_ENTRY_TYPE_OBJTOMB, &tomb, sizeof(tomb));
    }

    const Object *objPtr = (const Object *)log.append(
        LOG_ENTRY_TYPE_OBJ, newObject, newObject->size());
    objectMap.replace(tableId, id, objPtr);

    *newVersion = objPtr->version;
    bytesWritten += dataLength;
}

} // namespace RAMCloud

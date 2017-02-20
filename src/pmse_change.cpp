/*
 * Copyright 2014-2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"

#include "pmse_change.h"

#include <libpmemobj++/transaction.hpp>

namespace mongo {

    InsertChange::InsertChange(persistent_ptr<PmseMap<InitData>> mapper, RecordId loc) : _mapper(mapper), _loc(loc) {}
    void InsertChange::commit() {}
    void InsertChange::rollback() {
        _mapper->remove((uint64_t) _loc.repr());
    }

    RemoveChange::RemoveChange(pool_base pop, InitData* data) : _pop(pop)   {
        _cachedData = (InitData*)malloc(sizeof(InitData)+data->size);
        memcpy(_cachedData->data, data->data, data->size);
        _cachedData->size = data->size;
    }
    RemoveChange::~RemoveChange()
    {
        free(_cachedData);
    }

    void RemoveChange::commit() {}
    void RemoveChange::rollback() {
        persistent_ptr<InitData> obj;
        uint64_t id = 0;
        _mapper = pool<root>(_pop).get_root()->kvmap_root_ptr;
        try {
            transaction::exec_tx(_pop, [&] {
                obj = pmemobj_tx_alloc(sizeof(InitData::size) + _cachedData->size, 1);
                obj->size = _cachedData->size;
                memcpy(obj->data, _cachedData->data, _cachedData->size);
            });
        } catch (std::exception &e) {
            log() << e.what();
        }
        id = _mapper->insert(obj);
    }

    UpdateChange::UpdateChange(pool_base pop, uint64_t key, InitData* data) : _pop(pop), _key(key)   {
        _cachedData = (InitData*)malloc(sizeof(InitData)+data->size);
        memcpy(_cachedData->data, data->data, data->size);
        _cachedData->size = data->size;
    }
    UpdateChange::~UpdateChange()
    {
        free(_cachedData);
    }
    void UpdateChange::commit() {}
    void UpdateChange::rollback() {
        persistent_ptr<InitData> obj;
        uint64_t id = 0;
        try {
            _mapper = pool<root>(_pop).get_root()->kvmap_root_ptr;
            transaction::exec_tx(_pop, [&] {
                obj = pmemobj_tx_alloc(sizeof(InitData::size) + _cachedData->size, 1);
                obj->size = _cachedData->size;
                memcpy(obj->data, _cachedData->data, _cachedData->size);
            });
        } catch (std::exception &e) {
            log() << e.what();
        }
        id = _mapper->updateKV(_key, obj);
    }

    InsertIndexChange::InsertIndexChange(persistent_ptr<PmseTree> tree,
                                         pool_base pop, BSONObj key,
                                         RecordId loc, bool dupsAllowed,
                                         const IndexDescriptor* desc) :
                                    _tree(tree),
                                    _pop(pop),
                                    _key(key),
                                    _loc(loc),
                                    _dupsAllowed(dupsAllowed),
                                    _desc(desc) {
    }
    void InsertIndexChange::commit() {
    }
    void InsertIndexChange::rollback() {
        try {
            transaction::exec_tx(_pop,

                            [&] {
                                _tree->remove(_pop, _key, _loc, _dupsAllowed, _desc->keyPattern(), nullptr);
                                --(_tree->_records);
                            });
        } catch (std::exception &e) {
            log() << e.what();
        }
    }
    RemoveIndexChange::RemoveIndexChange(pool_base pop, BSONObj key,RecordId loc, bool dupsAllowed, BSONObj ordering) :
                                    _pop(pop),
                                    _key(key),
                                    _loc(loc),
                                    _dupsAllowed(dupsAllowed),
                                    _ordering(ordering) {
    }
    void RemoveIndexChange::commit() {}
    void RemoveIndexChange::rollback() {
        persistent_ptr<char> obj;
        Status status = Status::OK();
        BSONObj_PM bsonPM;

        try {
            _tree = pool<PmseTree>(_pop).get_root();
            transaction::exec_tx(_pop,
                            [&] {
                                obj = pmemobj_tx_alloc(_key.objsize(), 1);
                                memcpy( (void*)obj.get(), _key.objdata(), _key.objsize());
                                bsonPM.data = obj;
                                status = _tree->insert(_pop, bsonPM, _loc, _ordering, _dupsAllowed);
                                if(status == Status::OK())
                                {
                                    ++_tree->_records;
                                }
                            });
        } catch (std::exception &e) {
            log() << e.what();
        }
    }

}

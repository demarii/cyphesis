// Cyphesis Online RPG Server and AI Engine
// Copyright (C) 2008 Alistair Riddoch
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

// $Id$

#include "StorageManager.h"

#include "WorldRouter.h"

#include "rulesets/Entity.h"

#include "common/Database.h"
#include "common/TypeNode.h"
#include "common/Property.h"
#include "common/debug.h"
#include "common/Monitors.h"
#include "common/const.h"

#include <wfmath/atlasconv.h>

#include <sigc++/adaptors/bind.h>
#include <sigc++/functors/mem_fun.h>

#include <iostream>

typedef Database::KeyValues KeyValues;

static const bool debug_flag = false;

StorageManager:: StorageManager(WorldRouter & world) :
      m_insertEntityCount(0), m_updateEntityCount(0),
      m_insertPropertyCount(0), m_updatePropertyCount(0),
      m_insertQps(0), m_updateQps(0),
      m_insertQpsNow(0), m_updateQpsNow(0),
      m_insertQpsAvg(0), m_updateQpsAvg(0),
      m_insertQpsIndex(0), m_updateQpsIndex(0)

{
    if (database_flag) {
        world.inserted.connect(sigc::mem_fun(this,
              &StorageManager::entityInserted));

        Monitors::instance()->watch("storage_entity_inserts",
                                    new Monitor<int>(m_insertEntityCount));
        Monitors::instance()->watch("storage_entity_updates",
                                    new Monitor<int>(m_updateEntityCount));
        Monitors::instance()->watch("storage_property_inserts",
                                    new Monitor<int>(m_insertPropertyCount));
        Monitors::instance()->watch("storage_property_updates",
                                    new Monitor<int>(m_updatePropertyCount));

        Monitors::instance()->watch("storage_qps{qtype=inserts,t=1}",
                                    new Monitor<int>(m_insertQpsNow));
        Monitors::instance()->watch("storage_qps{qtype=updates,t=1}",
                                    new Monitor<int>(m_updateQpsNow));

        Monitors::instance()->watch("storage_qps{qtype=inserts,t=32}",
                                    new Monitor<int>(m_insertQpsAvg));
        Monitors::instance()->watch("storage_qps{qtype=updates,t=32}",
                                    new Monitor<int>(m_updateQpsAvg));

        for (int i = 0; i < 32; ++i) {
            m_insertQpsRing[i] = 0;
            m_updateQpsRing[i] = 0;
        }
    }
}

/// \brief Called when a new Entity is inserted in the world
void StorageManager::entityInserted(Entity * ent)
{
    if (ent->getFlags() & entity_ephem) {
        return;
    }
    m_unstoredEntities.push_back(EntityRef(ent));
    ent->setFlags(entity_queued);
}

/// \brief Called when an Entity is modified
void StorageManager::entityUpdated(Entity * ent)
{
    if (ent->isDestroyed()) {
        m_destroyedEntities.push_back(ent->getIntId());
        return;
    }
    // Is it already in the dirty Entities queue?
    // Perhaps we need to modify the semantics of the updated signal
    // so it is only emitted if the entity was not marked as dirty.
    if (ent->getFlags() & entity_queued) {
        // std::cout << "Already queued " << ent->getId() << std::endl << std::flush;
        return;
    }
    m_dirtyEntities.push_back(EntityRef(ent));
    // std::cout << "Updated fired " << ent->getId() << std::endl << std::flush;
    ent->setFlags(entity_queued);
}

void StorageManager::encodeProperty(PropertyBase * prop, std::string & store)
{
    Atlas::Message::MapType map;
    prop->get(map["val"]);
    Database::instance()->encodeObject(map, store);
}

void StorageManager::insertEntity(Entity * ent)
{
    std::string location;
    Atlas::Message::MapType map;
    map["pos"] = ent->m_location.pos().toAtlas();
    if (ent->m_location.orientation().isValid()) {
        map["orientation"] = ent->m_location.orientation().toAtlas();
    }
    Database::instance()->encodeObject(map, location);

    Database::instance()->insertEntity(ent->getId(),
                                       ent->m_location.m_loc->getId(),
                                       ent->getType()->name(),
                                       ent->getSeq(),
                                       location);
    ++m_insertEntityCount;
    KeyValues property_tuples;
    const PropertyDict & properties = ent->getProperties();
    PropertyDict::const_iterator I = properties.begin();
    PropertyDict::const_iterator Iend = properties.end();
    for (; I != Iend; ++I) {
        PropertyBase * prop = I->second;
        if (prop->flags() & per_ephem) {
            continue;
        }
        encodeProperty(prop, property_tuples[I->first]);
        prop->setFlags(per_clean | per_seen);
    }
    if (!property_tuples.empty()) {
        Database::instance()->insertProperties(ent->getId(), property_tuples);
        ++m_insertPropertyCount;
    }
    ent->resetFlags(entity_queued);
    ent->setFlags(entity_clean | entity_pos_clean | entity_orient_clean);
    ent->updated.connect(sigc::bind(sigc::mem_fun(this, &StorageManager::entityUpdated), ent));
}

void StorageManager::updateEntity(Entity * ent)
{
    std::string location;
    Atlas::Message::MapType map;
    map["pos"] = ent->m_location.pos().toAtlas();
    if (ent->m_location.orientation().isValid()) {
        map["orientation"] = ent->m_location.orientation().toAtlas();
    }
    Database::instance()->encodeObject(map, location);

    Database::instance()->updateEntity(ent->getId(),
                                       ent->getSeq(),
                                       location);
    ++m_updateEntityCount;
    KeyValues new_property_tuples;
    KeyValues upd_property_tuples;
    const PropertyDict & properties = ent->getProperties();
    PropertyDict::const_iterator I = properties.begin();
    PropertyDict::const_iterator Iend = properties.end();
    for (; I != Iend; ++I) {
        PropertyBase * prop = I->second;
        if (prop->flags() & per_mask) {
            continue;
        }
        // FIXME check if this is new or just modded.
        if (prop->flags() & per_seen) {
            encodeProperty(prop, upd_property_tuples[I->first]);
            ++m_updatePropertyCount;
        } else {
            encodeProperty(prop, new_property_tuples[I->first]);
            ++m_insertPropertyCount;
        }
        prop->setFlags(per_clean | per_seen);
    }
    if (!new_property_tuples.empty()) {
        Database::instance()->insertProperties(ent->getId(),
                                               new_property_tuples);
    }
    if (!upd_property_tuples.empty()) {
        Database::instance()->updateProperties(ent->getId(),
                                               upd_property_tuples);
    }
    ent->resetFlags(entity_queued);
    ent->setFlags(entity_clean);
}

void StorageManager::tick()
{
    int inserts = 0, updates = 0;
    int old_insert_queries = m_insertEntityCount + m_insertPropertyCount;
    int old_update_queries = m_updateEntityCount + m_updatePropertyCount;

    while (!m_destroyedEntities.empty()) {
        long id = m_destroyedEntities.front();
        Database::instance()->dropEntity(id);
        m_destroyedEntities.pop_front();
    }

    while (!m_unstoredEntities.empty()) {
        const EntityRef & ent = m_unstoredEntities.front();
        if (ent.get() != 0) {
            debug( std::cout << "storing " << ent->getId() << std::endl << std::flush; );
            insertEntity(ent.get());
            ++inserts;
        } else {
            debug( std::cout << "deleted" << std::endl << std::flush; );
        }
        m_unstoredEntities.pop_front();
    }

    while (!m_dirtyEntities.empty()) {
        if (Database::instance()->queryQueueSize() > 200) {
            debug(std::cout << "Too many" << std::endl << std::flush;);
            break;
        }
        const EntityRef & ent = m_dirtyEntities.front();
        if (ent.get() != 0) {
            debug( std::cout << "updating " << ent->getId() << std::endl << std::flush; );
            updateEntity(ent.get());
            ++updates;
        } else {
            debug( std::cout << "deleted" << std::endl << std::flush; );
        }
        m_dirtyEntities.pop_front();
    }
    if (inserts > 0 || updates > 0) {
        debug(std::cout << "I: " << inserts << " U: " << updates
                        << std::endl << std::flush;);
    }
    int insert_queries = m_insertEntityCount + m_insertPropertyCount 
                         - old_insert_queries;

    if (++m_insertQpsIndex >= 32) {
        m_insertQpsIndex = 0;
    }
    m_insertQps -= m_insertQpsRing[m_insertQpsIndex];
    m_insertQps += insert_queries;
    m_insertQpsRing[m_insertQpsIndex] = insert_queries;
    m_insertQpsAvg = m_insertQps / 32;
    m_insertQpsNow = insert_queries;

    debug(std::cout << "Ins: " << insert_queries << ", " << m_insertQps / 32
                    << std::endl << std::flush;);

    int update_queries = m_updateEntityCount + m_updatePropertyCount 
                         - old_update_queries;

    if (++m_updateQpsIndex >= 32) {
        m_updateQpsIndex = 0;
    }
    m_updateQps -= m_updateQpsRing[m_updateQpsIndex];
    m_updateQps += update_queries;
    m_updateQpsRing[m_updateQpsIndex] = update_queries;
    m_updateQpsAvg = m_updateQps / 32;
    m_updateQpsNow = update_queries;

    debug(std::cout << "Ups: " << update_queries << ", " << m_updateQps / 32
                    << std::endl << std::flush;);
}

int StorageManager::initWorld()
{
    Entity * ent = &BaseWorld::instance().m_gameWorld;

    ent->updated.connect(sigc::bind(sigc::mem_fun(this, &StorageManager::entityUpdated), ent));
    ent->setFlags(entity_clean);
    // FIXME queue it so the initial state gets persisted.
    return 0;
}

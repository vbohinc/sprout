/**
 * @file regstore.cpp Registration data store.
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2013  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version, along with the "Special Exception" for use of
 * the program along with SSL, set forth below. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details. You should have received a copy of the GNU General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by
 * post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 *
 * Special Exception
 * Metaswitch Networks Ltd  grants you permission to copy, modify,
 * propagate, and distribute a work formed by combining OpenSSL with The
 * Software, or a work derivative of such a combination, even if such
 * copying, modification, propagation, or distribution would otherwise
 * violate the terms of the GPL. You must comply with the GPL in all
 * respects for all of the code used other than OpenSSL.
 * "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
 * Project and licensed under the OpenSSL Licenses, or a work based on such
 * software and licensed under the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */


// Common STL includes.
#include <cassert>
#include <vector>
#include <map>
#include <set>
#include <list>
#include <queue>
#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <time.h>

#include "log.h"
#include "utils.h"
#include "regstore.h"


RegStore::RegStore(Store* data_store) :
  _data_store(data_store)
{
}


RegStore::~RegStore()
{
}


/// Retrieve the registration data for a given SIP Address of Record, creating
/// an empty record if no data exists for the AoR.
///
/// @param aor_id       The SIP Address of Record for the registration
RegStore::AoR* RegStore::get_aor_data(const std::string& aor_id)
{
  LOG_DEBUG("Get AoR data for %s", aor_id.c_str());
  AoR* aor_data = NULL;

  std::string data;
  uint64_t cas;
  Store::Status status = _data_store->get_data("reg", aor_id, data, cas);

  if (status == Store::Status::OK)
  {
    // Retrieved the data, so deserialize it.
    aor_data = deserialize_aor(data);
    aor_data->_cas = cas;
    LOG_DEBUG("Data store returned a record, CAS = %ld", aor_data->_cas);
  }
  else if (status == Store::Status::NOT_FOUND)
  {
    // Data store didn't find the record, so create a new blank record.
    aor_data = new AoR();
    LOG_DEBUG("Data store returned not found, so create new record, CAS = %ld", aor_data->_cas);
  }

  return aor_data;
}


/// Update the data for a particular address of record.  Writes the data
/// atomically.  If the underlying data has changed since it was last
/// read, the update is rejected and this returns false; if the update
/// succeeds, this returns true.
///
/// @param aor_id     The SIP Address of Record for the registration
/// @param aor_data   The registration data record.
bool RegStore::set_aor_data(const std::string& aor_id,
                            AoR* aor_data)
{
  // Expire any old bindings before writing to the server.  In theory,
  // if there are no bindings left we could delete the entry, but this
  // may cause concurrency problems because memcached does not support
  // cas on delete operations.  In this case we do a memcached_cas with
  // an effectively immediate expiry time.
  int now = time(NULL);
  int max_expires = expire_bindings(aor_data, now);

  LOG_DEBUG("Set AoR data for %s, CAS=%ld, expiry = %d",
            aor_id.c_str(), aor_data->_cas, max_expires);

  std::string data = serialize_aor(aor_data);

  Store::Status status = _data_store->set_data("reg",
                                               aor_id,
                                               data,
                                               aor_data->_cas,
                                               max_expires - now);
  LOG_DEBUG("Data store set_data returned %d", status);

  return (status == Store::Status::OK);
}


/// Expire any old bindings, and calculates the latest outstanding expiry time,
/// or now if none.
///
/// @returns             The latest expiry time from all unexpired bindings.
/// @param aor_data      The registration data record.
/// @param now           The current time in seconds since the epoch.
int RegStore::expire_bindings(AoR* aor_data,
                              int now)
{
  int max_expires = now;
  for (AoR::Bindings::iterator i = aor_data->_bindings.begin();
       i != aor_data->_bindings.end();
      )
  {
    AoR::Binding* b = i->second;
    if (b->_expires <= now)
    {
      // The binding has expired, so remove it.
      delete i->second;
      aor_data->_bindings.erase(i++);
    }
    else
    {
      if (b->_expires > max_expires)
      {
        max_expires = b->_expires;
      }
      ++i;
    }
  }
  return max_expires;
}


/// Serialize the contents of an AoR.
std::string RegStore::serialize_aor(AoR* aor_data)
{
  std::ostringstream oss(std::ostringstream::out|std::ostringstream::binary);

  int num_bindings = aor_data->bindings().size();
  oss.write((const char *)&num_bindings, sizeof(int));

  for (AoR::Bindings::const_iterator i = aor_data->bindings().begin();
       i != aor_data->bindings().end();
       ++i)
  {
    oss << i->first << '\0';

    AoR::Binding* b = i->second;
    oss << b->_uri << '\0';
    oss << b->_cid << '\0';
    oss.write((const char *)&b->_cseq, sizeof(int));
    oss.write((const char *)&b->_expires, sizeof(int));
    oss.write((const char *)&b->_priority, sizeof(int));
    int num_params = b->_params.size();
    oss.write((const char *)&num_params, sizeof(int));
    for (std::list<std::pair<std::string, std::string> >::const_iterator i = b->_params.begin();
         i != b->_params.end();
         ++i)
    {
      oss << i->first << '\0' << i->second << '\0';
    }
    int num_path_hdrs = b->_path_headers.size();
    oss.write((const char *)&num_path_hdrs, sizeof(int));
    for (std::list<std::string>::const_iterator i = b->_path_headers.begin();
         i != b->_path_headers.end();
         ++i)
    {
      oss << *i << '\0';
    }
  }

  return oss.str();
}


/// Deserialize the contents of an AoR
RegStore::AoR* RegStore::deserialize_aor(const std::string& s)
{
  std::istringstream iss(s, std::istringstream::in|std::istringstream::binary);

  AoR* aor_data = new AoR();
  int num_bindings;
  iss.read((char *)&num_bindings, sizeof(int));

  for (int ii = 0; ii < num_bindings; ++ii)
  {
    // Extract the binding identifier into a string.
    std::string binding_id;
    getline(iss, binding_id, '\0');

    AoR::Binding* b = aor_data->get_binding(binding_id);

    // Now extract the various fixed binding parameters.
    getline(iss, b->_uri, '\0');
    getline(iss, b->_cid, '\0');
    iss.read((char *)&b->_cseq, sizeof(int));
    iss.read((char *)&b->_expires, sizeof(int));
    iss.read((char *)&b->_priority, sizeof(int));

    int num_params;
    iss.read((char *)&num_params, sizeof(int));
    b->_params.resize(num_params);
    for (std::list<std::pair<std::string, std::string> >::iterator i = b->_params.begin();
         i != b->_params.end();
         ++i)
    {
      getline(iss, i->first, '\0');
      getline(iss, i->second, '\0');
    }

    int num_paths = 0;
    iss.read((char *)&num_paths, sizeof(int));
    b->_path_headers.resize(num_paths);
    for (std::list<std::string>::iterator i = b->_path_headers.begin();
         i != b->_path_headers.end();
         ++i)
    {
      getline(iss, *i, '\0');
    }
  }

  return aor_data;
}

/// Default constructor.
RegStore::AoR::AoR() :
  _bindings(),
  _cas(0)
{
}


/// Destructor.
RegStore::AoR::~AoR()
{
  clear();
}


/// Copy constructor.
RegStore::AoR::AoR(const AoR& other)
{
  for (Bindings::const_iterator i = other._bindings.begin();
       i != other._bindings.end();
       ++i)
  {
    Binding* bb = new Binding(*i->second);
    _bindings.insert(std::make_pair(i->first, bb));
  }
  _cas = other._cas;
}


// Make sure assignment is deep!
RegStore::AoR& RegStore::AoR::operator= (AoR const& other)
{
  if (this != &other)
  {
    clear();

    for (Bindings::const_iterator i = other._bindings.begin();
         i != other._bindings.end();
         ++i)
    {
      Binding* bb = new Binding(*i->second);
      _bindings.insert(std::make_pair(i->first, bb));
    }
    _cas = other._cas;
  }

  return *this;
}


/// Clear all the bindings from this object.
void RegStore::AoR::clear()
{
  for (Bindings::iterator i = _bindings.begin();
       i != _bindings.end();
       ++i)
  {
    delete i->second;
  }
  _bindings.clear();
}


/// Retrieve a binding by binding identifier, creating an empty one if
/// necessary.  The created binding is completely empty, even the Contact URI
/// field.
RegStore::AoR::Binding* RegStore::AoR::get_binding(const std::string& binding_id)
{
  AoR::Binding* b;
  AoR::Bindings::const_iterator i = _bindings.find(binding_id);
  if (i != _bindings.end())
  {
    b = i->second;
  }
  else
  {
    // No existing binding with this id, so create a new one.
    b = new Binding;
    _bindings.insert(std::make_pair(binding_id, b));
  }
  return b;
}


/// Removes any binding that had the given ID.  If there is no such binding,
/// does nothing.
void RegStore::AoR::remove_binding(const std::string& binding_id)
{
  AoR::Bindings::iterator i = _bindings.find(binding_id);
  if (i != _bindings.end())
  {
    delete i->second;
    _bindings.erase(i);
  }
}


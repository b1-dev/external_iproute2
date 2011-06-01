/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "IDBCursorBackendImpl.h"

#if ENABLE(INDEXED_DATABASE)

#include "CrossThreadTask.h"
#include "IDBCallbacks.h"
#include "IDBDatabaseBackendImpl.h"
#include "IDBDatabaseError.h"
#include "IDBDatabaseException.h"
#include "IDBIndexBackendImpl.h"
#include "IDBKeyRange.h"
#include "IDBObjectStoreBackendImpl.h"
#include "IDBRequest.h"
#include "IDBSQLiteDatabase.h"
#include "IDBTransactionBackendInterface.h"
#include "SQLiteDatabase.h"
#include "SQLiteStatement.h"
#include "SerializedScriptValue.h"

namespace WebCore {

IDBCursorBackendImpl::IDBCursorBackendImpl(IDBSQLiteDatabase* database, PassRefPtr<IDBKeyRange> keyRange, IDBCursor::Direction direction, PassOwnPtr<SQLiteStatement> query, bool isSerializedScriptValueCursor, IDBTransactionBackendInterface* transaction, IDBObjectStoreBackendInterface* objectStore)
    : m_database(database)
    , m_keyRange(keyRange)
    , m_direction(direction)
    , m_query(query)
    , m_isSerializedScriptValueCursor(isSerializedScriptValueCursor)
    , m_transaction(transaction)
    , m_objectStore(objectStore)
{
    loadCurrentRow();
}

IDBCursorBackendImpl::~IDBCursorBackendImpl()
{
}

unsigned short IDBCursorBackendImpl::direction() const
{
    return m_direction;
}

PassRefPtr<IDBKey> IDBCursorBackendImpl::key() const
{
    return m_currentKey;
}

PassRefPtr<IDBAny> IDBCursorBackendImpl::value() const
{
    if (m_isSerializedScriptValueCursor)
        return IDBAny::create(m_currentSerializedScriptValue.get());
    return IDBAny::create(m_currentIDBKeyValue.get());
}

void IDBCursorBackendImpl::update(PassRefPtr<SerializedScriptValue> value, PassRefPtr<IDBCallbacks> callbacks, ExceptionCode& ec)
{
    if (!m_query || m_currentId == InvalidId || !m_isSerializedScriptValueCursor) {
        ec = IDBDatabaseException::NOT_ALLOWED_ERR;
        return;
    }

    RefPtr<IDBKey> key = m_currentIDBKeyValue ? m_currentIDBKeyValue : m_currentKey;
    m_objectStore->put(value, key.release(), IDBObjectStoreBackendInterface::CursorUpdate, callbacks, m_transaction.get(), ec);
}

void IDBCursorBackendImpl::continueFunction(PassRefPtr<IDBKey> prpKey, PassRefPtr<IDBCallbacks> prpCallbacks, ExceptionCode& ec)
{
    RefPtr<IDBCursorBackendImpl> cursor = this;
    RefPtr<IDBKey> key = prpKey;
    RefPtr<IDBCallbacks> callbacks = prpCallbacks;
    if (!m_transaction->scheduleTask(createCallbackTask(&IDBCursorBackendImpl::continueFunctionInternal, cursor, key, callbacks)))
        ec = IDBDatabaseException::NOT_ALLOWED_ERR;
}

bool IDBCursorBackendImpl::currentRowExists()
{
    String sql = m_currentIDBKeyValue ? "SELECT id FROM IndexData WHERE id = ?" : "SELECT id FROM ObjectStoreData WHERE id = ?";
    SQLiteStatement statement(m_database->db(), sql);

    bool ok = statement.prepare() == SQLResultOk;
    ASSERT_UNUSED(ok, ok);

    statement.bindInt64(1, m_currentId);
    return statement.step() == SQLResultRow;
}

// IMPORTANT: If this ever 1) fires an 'error' event and 2) it's possible to fire another event afterwards,
//            IDBRequest::hasPendingActivity() will need to be modified to handle this!!!
void IDBCursorBackendImpl::continueFunctionInternal(ScriptExecutionContext*, PassRefPtr<IDBCursorBackendImpl> prpCursor, PassRefPtr<IDBKey> prpKey, PassRefPtr<IDBCallbacks> callbacks)
{
    RefPtr<IDBCursorBackendImpl> cursor = prpCursor;
    RefPtr<IDBKey> key = prpKey;
    while (true) {
        if (!cursor->m_query || cursor->m_query->step() != SQLResultRow) {
            cursor->m_query = 0;
            cursor->m_currentId = InvalidId;
            cursor->m_currentKey = 0;
            cursor->m_currentSerializedScriptValue = 0;
            cursor->m_currentIDBKeyValue = 0;
            callbacks->onSuccess(SerializedScriptValue::nullValue());
            return;
        }

        RefPtr<IDBKey> oldKey = cursor->m_currentKey;
        cursor->loadCurrentRow();

        // Skip if this entry has been deleted from the object store.
        if (!cursor->currentRowExists())
            continue;

        // If a key was supplied, we must loop until we find that key (or hit the end).
        if (key && !key->isEqual(cursor->m_currentKey.get()))
            continue;

        // If we don't have a uniqueness constraint, we can stop now.
        if (cursor->m_direction == IDBCursor::NEXT || cursor->m_direction == IDBCursor::PREV)
            break;
        if (!cursor->m_currentKey->isEqual(oldKey.get()))
            break;
    }

    callbacks->onSuccess(cursor.get());
}

void IDBCursorBackendImpl::deleteFunction(PassRefPtr<IDBCallbacks> prpCallbacks, ExceptionCode& ec)
{
    if (!m_query || m_currentId == InvalidId || !m_isSerializedScriptValueCursor) {
        ec = IDBDatabaseException::NOT_ALLOWED_ERR;
        return;
    }

    RefPtr<IDBKey> key = m_currentIDBKeyValue ? m_currentIDBKeyValue : m_currentKey;
    m_objectStore->deleteFunction(key.release(), prpCallbacks, m_transaction.get(), ec);
}


void IDBCursorBackendImpl::loadCurrentRow()
{
    // The column numbers depend on the query in IDBObjectStoreBackendImpl::openCursorInternal or
    // IDBIndexBackendImpl::openCursorInternal.
    m_currentId = m_query->getColumnInt64(0);
    m_currentKey = IDBKey::fromQuery(*m_query, 1);
    if (m_isSerializedScriptValueCursor)
        m_currentSerializedScriptValue = SerializedScriptValue::createFromWire(m_query->getColumnBlobAsString(4));

    m_currentIDBKeyValue = IDBKey::fromQuery(*m_query, 5);
}

SQLiteDatabase& IDBCursorBackendImpl::database() const
{
    return m_database->db();
}

} // namespace WebCore

#endif // ENABLE(INDEXED_DATABASE)
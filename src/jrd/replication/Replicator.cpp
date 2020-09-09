/*
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Dmitry Yemanov
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2013 Dmitry Yemanov <dimitr@firebirdsql.org>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 */

#include "firebird.h"
#include "../jrd/jrd.h"
#include "../../common/classes/BlobWrapper.h"

#include "Config.h"
#include "Replicator.h"
#include "Utils.h"

using namespace Firebird;
using namespace Jrd;
using namespace Replication;


Replicator::Replicator(MemoryPool& pool,
					   Manager* manager,
					   const Guid& guid,
					   const MetaString& user)
	: m_manager(manager),
	  m_config(manager->getConfig()),
	  m_guid(guid),
	  m_user(user),
	  m_transactions(pool),
	  m_generators(pool),
	  m_status(pool)
{
}

void Replicator::flush(BatchBlock& block, FlushReason reason, ULONG flags)
{
	const auto traNumber = block.header.traNumber;

	const auto orgLength = (ULONG) block.buffer->getCount();
	fb_assert(orgLength > sizeof(Block));
	block.header.protocol = PROTOCOL_CURRENT_VERSION;
	block.header.dataLength = orgLength - sizeof(Block);
	block.header.metaLength = (ULONG) (block.metadata.getCount() * sizeof(MetaString));
	block.header.timestamp = TimeZoneUtil::getCurrentGmtTimeStamp().utc_timestamp;
	block.header.flags |= flags;

	// Add metadata (if any) to the buffer

	if (block.header.metaLength)
	{
		block.buffer->resize(orgLength + block.header.metaLength);
		memcpy(block.buffer->begin() + orgLength, block.metadata.begin(), block.header.metaLength);
	}

	// Re-write the updated header

	memcpy(block.buffer->begin(), &block.header, sizeof(Block));

	// Pass the buffer to the replication manager and setup the new one

	const auto sync = (reason == FLUSH_SYNC);
	m_manager->flush(block.buffer, sync);

	memset(&block.header, 0, sizeof(Block));
	block.header.traNumber = traNumber;

	block.metadata.clear();
	block.lastMetaId = MAX_ULONG;
	block.buffer = m_manager->getBuffer();
	block.flushes++;
}

void Replicator::logError(const IStatus* status)
{
	string message;

	auto statusPtr = status->getErrors();

	char temp[BUFFER_LARGE];
	while (fb_interpret(temp, sizeof(temp), &statusPtr))
	{
		if (!message.isEmpty())
			message += "\n\t";

		message += temp;
	}

	logOriginMessage(m_config->dbName, message, ERROR_MSG);
}

void Replicator::postError(const Exception& ex)
{
	FbLocalStatus tempStatus;
	ex.stuffException(&tempStatus);

	logError(&tempStatus);

	Arg::StatusVector newErrors;
	newErrors << Arg::Gds(isc_random) << Arg::Str("Replication error");
	newErrors << Arg::StatusVector(tempStatus->getErrors());
	newErrors.copyTo(&m_status);
}

// IReplicatedSession implementation

IReplicatedTransaction* Replicator::startTransaction(ITransaction* trans, SINT64 number)
{
	AutoPtr<Transaction> transaction;

	try
	{
		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		MemoryPool& pool = getPool();
		transaction = FB_NEW_POOL(pool) Transaction(this, trans);
		m_transactions.add(transaction);

		auto& txnData = transaction->getData();

		fb_assert(!txnData.header.traNumber);
		txnData.header.traNumber = number;
		txnData.header.flags = BLOCK_BEGIN_TRANS;

		txnData.buffer = m_manager->getBuffer();

		txnData.putTag(opStartTransaction);
	}
	catch (const Exception& ex)
	{
		postError(ex);
	}

	return transaction.release();
}

bool Replicator::prepareTransaction(Transaction* transaction)
{
	try
	{
		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		auto& txnData = transaction->getData();

		txnData.putTag(opPrepareTransaction);

		flush(txnData, FLUSH_PREPARE);
	}
	catch (const Exception& ex)
	{
		postError(ex);
		return false;
	}

	return true;
}

bool Replicator::commitTransaction(Transaction* transaction)
{
	try
	{
		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		auto& txnData = transaction->getData();

		// Do not replicate this transaction if it's de-facto read-only.
		// If there were no flushes yet and the buffer contains just one tag
		// (this should be opStartTransaction), it means nothing was changed.

		const auto dataLength = txnData.buffer->getCount() - sizeof(Block);

		if (txnData.flushes || dataLength > sizeof(UCHAR))
		{
			for (const auto& generator : m_generators)
			{
				fb_assert(generator.name.hasData());

				txnData.putTag(opSetSequence);
				txnData.putMetaName(generator.name.c_str());
				txnData.putBigInt(generator.value);
			}

			m_generators.clear();

			txnData.putTag(opCommitTransaction);
			flush(txnData, FLUSH_SYNC, BLOCK_END_TRANS);
		}
		else
		{
			fb_assert((*txnData.buffer)[sizeof(Block)] == opStartTransaction);
		}
	}
	catch (const Exception& ex)
	{
		postError(ex);
		return false;
	}

	return true;
}

bool Replicator::rollbackTransaction(Transaction* transaction)
{
	try
	{
		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		auto& txnData = transaction->getData();

		if (txnData.flushes)
		{
			txnData.putTag(opRollbackTransaction);
			flush(txnData, FLUSH_SYNC, BLOCK_END_TRANS);
		}
		else
		{
			fb_assert((*txnData.buffer)[sizeof(Block)] == opStartTransaction);
		}
	}
	catch (const Exception& ex)
	{
		postError(ex);
		return false;
	}

	return true;
}

void Replicator::releaseTransaction(Transaction* transaction)
{
	try
	{
		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		auto& txnData = transaction->getData();
		m_manager->releaseBuffer(txnData.buffer);

		FB_SIZE_T pos;
		if (m_transactions.find(transaction, pos))
			m_transactions.remove(pos);
	}
	catch (const Exception&)
	{} // no-op
}

bool Replicator::startSavepoint(Transaction* transaction)
{
	try
	{
		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		auto& txnData = transaction->getData();

		txnData.putTag(opStartSavepoint);

		if (txnData.getSize() > m_config->bufferSize)
			flush(txnData, FLUSH_OVERFLOW);
	}
	catch (const Exception& ex)
	{
		postError(ex);
		return false;
	}

	return true;
}

bool Replicator::releaseSavepoint(Transaction* transaction)
{
	try
	{
		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		auto& txnData = transaction->getData();

		txnData.putTag(opReleaseSavepoint);

		if (txnData.getSize() > m_config->bufferSize)
			flush(txnData, FLUSH_OVERFLOW);
	}
	catch (const Exception& ex)
	{
		postError(ex);
		return false;
	}

	return true;
}

bool Replicator::rollbackSavepoint(Transaction* transaction)
{
	try
	{
		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		auto& txnData = transaction->getData();

		txnData.putTag(opRollbackSavepoint);

		flush(txnData, FLUSH_SYNC);
	}
	catch (const Exception& ex)
	{
		postError(ex);
		return false;
	}

	return true;
}

bool Replicator::insertRecord(Transaction* transaction,
							  const char* relName,
							  IReplicatedRecord* record)
{
	try
	{
		for (unsigned id = 0; id < record->getCount(); id++)
		{
			IReplicatedField* field = record->getField(id);
			if (field != nullptr)
			{
				auto type = field->getType();
				if (type == SQL_ARRAY || type == SQL_BLOB)
				{
					const auto blobId = (ISC_QUAD*) field->getData();

					if (blobId != nullptr)
					{
						if (!storeBlob(transaction, *blobId))
						{
							return false;
						}
					}
				}
			}
		}

		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		const auto length = record->getRawLength();
		const auto data = record->getRawData();

		auto& txnData = transaction->getData();

		txnData.putTag(opInsertRecord);
		txnData.putMetaName(relName);
		txnData.putBinary(length, data);

		if (txnData.getSize() > m_config->bufferSize)
			flush(txnData, FLUSH_OVERFLOW);
	}
	catch (const Exception& ex)
	{
		postError(ex);
		return false;
	}

	return true;
}

bool Replicator::updateRecord(Transaction* transaction,
							  const char* relName,
							  IReplicatedRecord* orgRecord,
							  IReplicatedRecord* newRecord)
{
	try
	{
		for (unsigned id = 0; id < newRecord->getCount(); id++)
		{
			IReplicatedField* field = newRecord->getField(id);
			if (field != nullptr)
			{
				auto type = field->getType();
				if (type == SQL_ARRAY || type == SQL_BLOB)
				{
					const auto blobId = (ISC_QUAD*) field->getData();

					if (blobId != nullptr)
					{
						if (!storeBlob(transaction, *blobId))
						{
							return false;
						}
					}
				}
			}
		}

		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		const auto orgLength = orgRecord->getRawLength();
		const auto orgData = orgRecord->getRawData();

		const auto newLength = newRecord->getRawLength();
		const auto newData = newRecord->getRawData();

		auto& txnData = transaction->getData();

		txnData.putTag(opUpdateRecord);
		txnData.putMetaName(relName);
		txnData.putBinary(orgLength, orgData);
		txnData.putBinary(newLength, newData);

		if (txnData.getSize() > m_config->bufferSize)
			flush(txnData, FLUSH_OVERFLOW);
	}
	catch (const Exception& ex)
	{
		postError(ex);
		return false;
	}

	return true;
}

bool Replicator::deleteRecord(Transaction* transaction,
							  const char* relName,
							  IReplicatedRecord* record)
{
	try
	{
		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		const auto length = record->getRawLength();
		const auto data = record->getRawData();

		auto& txnData = transaction->getData();

		txnData.putTag(opDeleteRecord);
		txnData.putMetaName(relName);
		txnData.putBinary(length, data);

		if (txnData.getSize() > m_config->bufferSize)
			flush(txnData, FLUSH_OVERFLOW);
	}
	catch (const Exception& ex)
	{
		postError(ex);
		return false;
	}

	return true;
}

bool Replicator::storeBlob(Transaction* transaction,
						   ISC_QUAD blobId)
{
	try
	{
		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		BlobWrapper blob(&m_status);
		if (!blob.open(m_attachment, transaction->getInterface(), blobId))
		{
			return false;
		}

		UCharBuffer buffer;
		const auto bufferLength = MAX_USHORT;
		auto data = buffer.getBuffer(bufferLength);

		auto& txnData = transaction->getData();
		bool newOp = true;

		FB_SIZE_T segmentLength;
		while (blob.getSegment(bufferLength, data, segmentLength))
		{
			if (!segmentLength)
				continue; // Zero-length segments are unusual but ok.

			if (newOp)
			{
				txnData.putTag(opStoreBlob);
				txnData.putInt(blobId.gds_quad_high);
				txnData.putInt(blobId.gds_quad_low);
				newOp = false;
			}

			txnData.putBinary(segmentLength, data);

			if (txnData.getSize() > m_config->bufferSize)
			{
				flush(txnData, FLUSH_OVERFLOW);
				newOp = true;
			}
		}

		if (m_status->getState() & IStatus::STATE_ERRORS)
			return false;

		blob.close();

		if (newOp)
		{
			txnData.putTag(opStoreBlob);
			txnData.putInt(blobId.gds_quad_high);
			txnData.putInt(blobId.gds_quad_low);
		}

		txnData.putBinary(0, NULL);

		if (txnData.getSize() > m_config->bufferSize)
			flush(txnData, FLUSH_OVERFLOW);
	}
	catch (const Exception& ex)
	{
		postError(ex);
		return false;
	}

	return true;
}

bool Replicator::executeSqlIntl(Transaction* transaction,
								unsigned charset,
								const char* sql)
{
	try
	{
		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		auto& txnData = transaction->getData();

		if (charset == CS_UTF8)
		{
			txnData.putTag(opExecuteSql);
		}
		else
		{
			txnData.putTag(opExecuteSqlIntl);
			txnData.putInt(charset);
		}

		txnData.putString(sql);
		txnData.putMetaName(m_user);

		if (txnData.getSize() > m_config->bufferSize)
			flush(txnData, FLUSH_OVERFLOW);
	}
	catch (const Exception& ex)
	{
		postError(ex);
		return false;
	}

	return true;
}

FB_BOOLEAN Replicator::cleanupTransaction(SINT64 number)
{
	try
	{
		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		BatchBlock block(getPool());
		block.header.traNumber = number;
		block.buffer = m_manager->getBuffer();
		block.putTag(opCleanupTransaction);

		flush(block, FLUSH_SYNC, BLOCK_END_TRANS);
	}
	catch (const Exception& ex)
	{
		postError(ex);
		return FB_FALSE;
	}

	return FB_TRUE;
}

FB_BOOLEAN Replicator::setSequence(const char* genName,
								   SINT64 value)
{
	try
	{
		MutexLockGuard guard(m_mutex, FB_FUNCTION);

		for (auto& generator : m_generators)
		{
			if (generator.name == genName)
			{
				generator.value = value;
				return true;
			}
		}

		GeneratorValue generator;
		generator.name = genName;
		generator.value = value;

		m_generators.add(generator);
	}
	catch (const Exception& ex)
	{
		postError(ex);
		return FB_FALSE;
	}

	return FB_TRUE;
}

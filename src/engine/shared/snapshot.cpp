/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "snapshot.h"
#include "compression.h"
#include "uuid_manager.h"

#include <game/generated/protocol.h>
#include <game/generated/protocolglue.h>

// CSnapshot

CSnapshotItem *CSnapshot::GetItem(int Index) const
{
	return (CSnapshotItem *)(DataStart() + Offsets()[Index]);
}

int CSnapshot::GetItemSize(int Index) const
{
	if(Index == m_NumItems - 1)
		return (m_DataSize - Offsets()[Index]) - sizeof(CSnapshotItem);
	return (Offsets()[Index + 1] - Offsets()[Index]) - sizeof(CSnapshotItem);
}

int CSnapshot::GetItemType(int Index) const
{
	int InternalType = GetItem(Index)->Type();
	if(InternalType < OFFSET_UUID_TYPE)
	{
		return InternalType;
	}

	int TypeItemIndex = GetItemIndex((0 << 16) | InternalType); // NETOBJTYPE_EX
	if(TypeItemIndex == -1 || GetItemSize(TypeItemIndex) < (int)sizeof(CUuid))
	{
		return InternalType;
	}

	CSnapshotItem *pTypeItem = GetItem(TypeItemIndex);
	CUuid Uuid;
	for(int i = 0; i < (int)sizeof(CUuid) / 4; i++)
	{
		Uuid.m_aData[i * 4 + 0] = pTypeItem->Data()[i] >> 24;
		Uuid.m_aData[i * 4 + 1] = pTypeItem->Data()[i] >> 16;
		Uuid.m_aData[i * 4 + 2] = pTypeItem->Data()[i] >> 8;
		Uuid.m_aData[i * 4 + 3] = pTypeItem->Data()[i];
	}

	return g_UuidManager.LookupUuid(Uuid);
}

int CSnapshot::GetItemIndex(int Key) const
{
	// TODO: OPT: this should not be a linear search. very bad
	for(int i = 0; i < m_NumItems; i++)
	{
		if(GetItem(i)->Key() == Key)
			return i;
	}
	return -1;
}

unsigned CSnapshot::Crc()
{
	unsigned int Crc = 0;

	for(int i = 0; i < m_NumItems; i++)
	{
		CSnapshotItem *pItem = GetItem(i);
		int Size = GetItemSize(i);

		for(int b = 0; b < Size / 4; b++)
			Crc += pItem->Data()[b];
	}
	return Crc;
}

void CSnapshot::DebugDump()
{
	dbg_msg("snapshot", "data_size=%d num_items=%d", m_DataSize, m_NumItems);
	for(int i = 0; i < m_NumItems; i++)
	{
		CSnapshotItem *pItem = GetItem(i);
		int Size = GetItemSize(i);
		dbg_msg("snapshot", "\ttype=%d id=%d", pItem->Type(), pItem->ID());
		for(int b = 0; b < Size / 4; b++)
			dbg_msg("snapshot", "\t\t%3d %12d\t%08x", b, pItem->Data()[b], pItem->Data()[b]);
	}
}

// CSnapshotDelta

struct CItemList
{
	int m_Num;
	int m_aKeys[64];
	int m_aIndex[64];
};

enum
{
	HASHLIST_SIZE = 256,
};

static void GenerateHash(CItemList *pHashlist, CSnapshot *pSnapshot)
{
	for(int i = 0; i < HASHLIST_SIZE; i++)
		pHashlist[i].m_Num = 0;

	for(int i = 0; i < pSnapshot->NumItems(); i++)
	{
		int Key = pSnapshot->GetItem(i)->Key();
		int HashID = ((Key >> 12) & 0xf0) | (Key & 0xf);
		if(pHashlist[HashID].m_Num != 64)
		{
			pHashlist[HashID].m_aIndex[pHashlist[HashID].m_Num] = i;
			pHashlist[HashID].m_aKeys[pHashlist[HashID].m_Num] = Key;
			pHashlist[HashID].m_Num++;
		}
	}
}

static int GetItemIndexHashed(int Key, const CItemList *pHashlist)
{
	int HashID = ((Key >> 12) & 0xf0) | (Key & 0xf);
	for(int i = 0; i < pHashlist[HashID].m_Num; i++)
	{
		if(pHashlist[HashID].m_aKeys[i] == Key)
			return pHashlist[HashID].m_aIndex[i];
	}

	return -1;
}

int CSnapshotDelta::DiffItem(int *pPast, int *pCurrent, int *pOut, int Size)
{
	int Needed = 0;
	while(Size)
	{
		*pOut = *pCurrent - *pPast;
		Needed |= *pOut;
		pOut++;
		pPast++;
		pCurrent++;
		Size--;
	}

	return Needed;
}

void CSnapshotDelta::UndiffItem(int *pPast, int *pDiff, int *pOut, int Size)
{
	while(Size)
	{
		*pOut = *pPast + *pDiff;

		if(*pDiff == 0)
			m_aSnapshotDataRate[m_SnapshotCurrent] += 1;
		else
		{
			unsigned char aBuf[16];
			unsigned char *pEnd = CVariableInt::Pack(aBuf, *pDiff);
			m_aSnapshotDataRate[m_SnapshotCurrent] += (int)(pEnd - (unsigned char *)aBuf) * 8;
		}

		pOut++;
		pPast++;
		pDiff++;
		Size--;
	}
}

CSnapshotDelta::CSnapshotDelta()
{
	mem_zero(m_aItemSizes, sizeof(m_aItemSizes));
	mem_zero(m_aSnapshotDataRate, sizeof(m_aSnapshotDataRate));
	mem_zero(m_aSnapshotDataUpdates, sizeof(m_aSnapshotDataUpdates));
	m_SnapshotCurrent = 0;
	mem_zero(&m_Empty, sizeof(m_Empty));
}

CSnapshotDelta::CSnapshotDelta(const CSnapshotDelta &Old)
{
	mem_copy(m_aItemSizes, Old.m_aItemSizes, sizeof(m_aItemSizes));
	mem_copy(m_aSnapshotDataRate, Old.m_aSnapshotDataRate, sizeof(m_aSnapshotDataRate));
	mem_copy(m_aSnapshotDataUpdates, Old.m_aSnapshotDataUpdates, sizeof(m_aSnapshotDataUpdates));
	mem_copy(&m_SnapshotCurrent, &Old.m_SnapshotCurrent, sizeof(m_SnapshotCurrent));
	mem_copy(&m_Empty, &Old.m_Empty, sizeof(m_Empty));
}

void CSnapshotDelta::SetStaticsize(int ItemType, int Size)
{
	if(ItemType < 0 || ItemType >= MAX_NETOBJSIZES)
		return;
	m_aItemSizes[ItemType] = Size;
}

CSnapshotDelta::CData *CSnapshotDelta::EmptyDelta()
{
	return &m_Empty;
}

// TODO: OPT: this should be made much faster
int CSnapshotDelta::CreateDelta(CSnapshot *pFrom, CSnapshot *pTo, void *pDstData)
{
	CData *pDelta = (CData *)pDstData;
	int *pData = (int *)pDelta->m_aData;
	int i, ItemSize, PastIndex;
	CSnapshotItem *pFromItem;
	CSnapshotItem *pCurItem;
	CSnapshotItem *pPastItem;
	int Count = 0;

	pDelta->m_NumDeletedItems = 0;
	pDelta->m_NumUpdateItems = 0;
	pDelta->m_NumTempItems = 0;

	CItemList aHashlist[HASHLIST_SIZE];
	GenerateHash(aHashlist, pTo);

	// pack deleted stuff
	for(i = 0; i < pFrom->NumItems(); i++)
	{
		pFromItem = pFrom->GetItem(i);
		if(GetItemIndexHashed(pFromItem->Key(), aHashlist) == -1)
		{
			// deleted
			pDelta->m_NumDeletedItems++;
			*pData = pFromItem->Key();
			pData++;
		}
	}

	GenerateHash(aHashlist, pFrom);
	int aPastIndices[1024];

	// fetch previous indices
	// we do this as a separate pass because it helps the cache
	const int NumItems = pTo->NumItems();
	for(i = 0; i < NumItems; i++)
	{
		pCurItem = pTo->GetItem(i); // O(1) .. O(n)
		aPastIndices[i] = GetItemIndexHashed(pCurItem->Key(), aHashlist); // O(n) .. O(n^n)
	}

	for(i = 0; i < NumItems; i++)
	{
		// do delta
		ItemSize = pTo->GetItemSize(i); // O(1) .. O(n)
		pCurItem = pTo->GetItem(i); // O(1) .. O(n)
		PastIndex = aPastIndices[i];

		bool IncludeSize = pCurItem->Type() >= MAX_NETOBJSIZES || !m_aItemSizes[pCurItem->Type()];

		if(PastIndex != -1)
		{
			int *pItemDataDst = pData + 3;

			pPastItem = pFrom->GetItem(PastIndex);

			if(!IncludeSize)
				pItemDataDst = pData + 2;

			if(DiffItem(pPastItem->Data(), pCurItem->Data(), pItemDataDst, ItemSize / 4))
			{
				*pData++ = pCurItem->Type();
				*pData++ = pCurItem->ID();
				if(IncludeSize)
					*pData++ = ItemSize / 4;
				pData += ItemSize / 4;
				pDelta->m_NumUpdateItems++;
			}
		}
		else
		{
			*pData++ = pCurItem->Type();
			*pData++ = pCurItem->ID();
			if(IncludeSize)
				*pData++ = ItemSize / 4;

			mem_copy(pData, pCurItem->Data(), ItemSize);
			pData += ItemSize / 4;
			pDelta->m_NumUpdateItems++;
			Count++;
		}
	}

	if(0)
	{
		dbg_msg("snapshot", "%d %d %d",
			pDelta->m_NumDeletedItems,
			pDelta->m_NumUpdateItems,
			pDelta->m_NumTempItems);
	}

	/*
	// TODO: pack temp stuff

	// finish
	//mem_copy(pDelta->offsets, deleted, pDelta->num_deleted_items*sizeof(int));
	//mem_copy(&(pDelta->offsets[pDelta->num_deleted_items]), update, pDelta->num_update_items*sizeof(int));
	//mem_copy(&(pDelta->offsets[pDelta->num_deleted_items+pDelta->num_update_items]), temp, pDelta->num_temp_items*sizeof(int));
	//mem_copy(pDelta->data_start(), data, data_size);
	//pDelta->data_size = data_size;
	* */

	if(!pDelta->m_NumDeletedItems && !pDelta->m_NumUpdateItems && !pDelta->m_NumTempItems)
		return 0;

	return (int)((char *)pData - (char *)pDstData);
}

static int RangeCheck(void *pEnd, void *pPtr, int Size)
{
	if((const char *)pPtr + Size > (const char *)pEnd)
		return -1;
	return 0;
}

int CSnapshotDelta::UnpackDelta(CSnapshot *pFrom, CSnapshot *pTo, void *pSrcData, int DataSize)
{
	CSnapshotBuilder Builder;
	CData *pDelta = (CData *)pSrcData;
	int *pData = (int *)pDelta->m_aData;
	int *pEnd = (int *)(((char *)pSrcData + DataSize));

	CSnapshotItem *pFromItem;
	int Keep, ItemSize;
	int *pDeleted;
	int ID, Type, Key;
	int FromIndex;
	int *pNewData;

	Builder.Init();

	// unpack deleted stuff
	pDeleted = pData;
	pData += pDelta->m_NumDeletedItems;
	if(pData > pEnd)
		return -1;

	// copy all non deleted stuff
	for(int i = 0; i < pFrom->NumItems(); i++)
	{
		// dbg_assert(0, "fail!");
		pFromItem = pFrom->GetItem(i);
		ItemSize = pFrom->GetItemSize(i);
		Keep = 1;
		for(int d = 0; d < pDelta->m_NumDeletedItems; d++)
		{
			if(pDeleted[d] == pFromItem->Key())
			{
				Keep = 0;
				break;
			}
		}

		if(Keep)
		{
			void *pObj = Builder.NewItem(pFromItem->Type(), pFromItem->ID(), ItemSize);
			if(!pObj)
				return -4;

			// keep it
			mem_copy(pObj, pFromItem->Data(), ItemSize);
		}
	}

	// unpack updated stuff
	for(int i = 0; i < pDelta->m_NumUpdateItems; i++)
	{
		if(pData + 2 > pEnd)
			return -1;

		Type = *pData++;
		if(Type < 0)
			return -1;
		ID = *pData++;
		if(Type < MAX_NETOBJSIZES && m_aItemSizes[Type])
			ItemSize = m_aItemSizes[Type];
		else
		{
			if(pData + 1 > pEnd)
				return -2;
			ItemSize = (*pData++) * 4;
		}
		m_SnapshotCurrent = Type;

		if(m_SnapshotCurrent < 0 || m_SnapshotCurrent > 0xFFFF)
			return -3;
		if(RangeCheck(pEnd, pData, ItemSize) || ItemSize < 0)
			return -3;

		Key = (Type << 16) | ID;

		// create the item if needed
		pNewData = Builder.GetItemData(Key);
		if(!pNewData)
			pNewData = (int *)Builder.NewItem(Key >> 16, Key & 0xffff, ItemSize);

		if(!pNewData)
			return -4;

		FromIndex = pFrom->GetItemIndex(Key);
		if(FromIndex != -1)
		{
			// we got an update so we need pTo apply the diff
			UndiffItem(pFrom->GetItem(FromIndex)->Data(), pData, pNewData, ItemSize / 4);
			m_aSnapshotDataUpdates[m_SnapshotCurrent]++;
		}
		else // no previous, just copy the pData
		{
			mem_copy(pNewData, pData, ItemSize);
			m_aSnapshotDataRate[m_SnapshotCurrent] += ItemSize * 8;
			m_aSnapshotDataUpdates[m_SnapshotCurrent]++;
		}

		pData += ItemSize / 4;
	}

	// finish up
	return Builder.Finish(pTo);
}

// CSnapshotStorage

void CSnapshotStorage::Init()
{
	m_pFirst = 0;
	m_pLast = 0;
}

void CSnapshotStorage::PurgeAll()
{
	CHolder *pHolder = m_pFirst;
	CHolder *pNext;

	while(pHolder)
	{
		pNext = pHolder->m_pNext;
		free(pHolder);
		pHolder = pNext;
	}

	// no more snapshots in storage
	m_pFirst = 0;
	m_pLast = 0;
}

void CSnapshotStorage::PurgeUntil(int Tick)
{
	CHolder *pHolder = m_pFirst;
	CHolder *pNext;

	while(pHolder)
	{
		pNext = pHolder->m_pNext;
		if(pHolder->m_Tick >= Tick)
			return; // no more to remove
		free(pHolder);

		// did we come to the end of the list?
		if(!pNext)
			break;

		m_pFirst = pNext;
		pNext->m_pPrev = 0x0;

		pHolder = pNext;
	}

	// no more snapshots in storage
	m_pFirst = 0;
	m_pLast = 0;
}

void CSnapshotStorage::Add(int Tick, int64 Tagtime, int DataSize, void *pData, int CreateAlt)
{
	// allocate memory for holder + snapshot_data
	int TotalSize = sizeof(CHolder) + DataSize;

	if(CreateAlt)
		TotalSize += DataSize;

	CHolder *pHolder = (CHolder *)malloc(TotalSize);

	// set data
	pHolder->m_Tick = Tick;
	pHolder->m_Tagtime = Tagtime;
	pHolder->m_SnapSize = DataSize;
	pHolder->m_pSnap = (CSnapshot *)(pHolder + 1);
	mem_copy(pHolder->m_pSnap, pData, DataSize);

	if(CreateAlt) // create alternative if wanted
	{
		pHolder->m_pAltSnap = (CSnapshot *)(((char *)pHolder->m_pSnap) + DataSize);
		mem_copy(pHolder->m_pAltSnap, pData, DataSize);
	}
	else
		pHolder->m_pAltSnap = 0;

	// link
	pHolder->m_pNext = 0;
	pHolder->m_pPrev = m_pLast;
	if(m_pLast)
		m_pLast->m_pNext = pHolder;
	else
		m_pFirst = pHolder;
	m_pLast = pHolder;
}

int CSnapshotStorage::Get(int Tick, int64 *pTagtime, CSnapshot **ppData, CSnapshot **ppAltData)
{
	CHolder *pHolder = m_pFirst;

	while(pHolder)
	{
		if(pHolder->m_Tick == Tick)
		{
			if(pTagtime)
				*pTagtime = pHolder->m_Tagtime;
			if(ppData)
				*ppData = pHolder->m_pSnap;
			if(ppAltData)
				*ppAltData = pHolder->m_pAltSnap;
			return pHolder->m_SnapSize;
		}

		pHolder = pHolder->m_pNext;
	}

	return -1;
}

// CSnapshotBuilder
CSnapshotBuilder::CSnapshotBuilder()
{
	m_NumExtendedItemTypes = 0;
}

void CSnapshotBuilder::Init(bool Sixup)
{
	m_DataSize = 0;
	m_NumItems = 0;
	m_Sixup = Sixup;

	for(int i = 0; i < m_NumExtendedItemTypes; i++)
	{
		AddExtendedItemType(i);
	}
}

CSnapshotItem *CSnapshotBuilder::GetItem(int Index)
{
	return (CSnapshotItem *)&(m_aData[m_aOffsets[Index]]);
}

int *CSnapshotBuilder::GetItemData(int Key)
{
	int i;
	for(i = 0; i < m_NumItems; i++)
	{
		if(GetItem(i)->Key() == Key)
			return GetItem(i)->Data();
	}
	return 0;
}

int CSnapshotBuilder::Finish(void *pSnapData)
{
	// dbg_msg("snap", "---------------------------");
	//  flattern and make the snapshot
	CSnapshot *pSnap = (CSnapshot *)pSnapData;
	int OffsetSize = sizeof(int) * m_NumItems;
	pSnap->m_DataSize = m_DataSize;
	pSnap->m_NumItems = m_NumItems;
	mem_copy(pSnap->Offsets(), m_aOffsets, OffsetSize);
	mem_copy(pSnap->DataStart(), m_aData, m_DataSize);
	return sizeof(CSnapshot) + OffsetSize + m_DataSize;
}

static int GetTypeFromIndex(int Index)
{
	return CSnapshot::MAX_TYPE - Index;
}

void CSnapshotBuilder::AddExtendedItemType(int Index)
{
	dbg_assert(0 <= Index && Index < m_NumExtendedItemTypes, "index out of range");
	int TypeID = m_aExtendedItemTypes[Index];
	CUuid Uuid = g_UuidManager.GetUuid(TypeID);
	int *pUuidItem = (int *)NewItem(0, GetTypeFromIndex(Index), sizeof(Uuid)); // NETOBJTYPE_EX
	if(pUuidItem)
	{
		for(int i = 0; i < (int)sizeof(CUuid) / 4; i++)
		{
			pUuidItem[i] =
				(Uuid.m_aData[i * 4 + 0] << 24) |
				(Uuid.m_aData[i * 4 + 1] << 16) |
				(Uuid.m_aData[i * 4 + 2] << 8) |
				(Uuid.m_aData[i * 4 + 3]);
		}
	}
}

int CSnapshotBuilder::GetExtendedItemTypeIndex(int TypeID)
{
	for(int i = 0; i < m_NumExtendedItemTypes; i++)
	{
		if(m_aExtendedItemTypes[i] == TypeID)
		{
			return i;
		}
	}
	dbg_assert(m_NumExtendedItemTypes < MAX_EXTENDED_ITEM_TYPES, "too many extended item types");
	int Index = m_NumExtendedItemTypes;
	m_aExtendedItemTypes[Index] = TypeID;
	m_NumExtendedItemTypes++;
	return Index;
}

void *CSnapshotBuilder::NewItem(int Type, int ID, int Size)
{
	if(m_DataSize + sizeof(CSnapshotItem) + Size >= CSnapshot::MAX_SIZE ||
		m_NumItems + 1 >= MAX_ITEMS)
	{
		dbg_assert(m_DataSize < CSnapshot::MAX_SIZE, "too much data");
		dbg_assert(m_NumItems < MAX_ITEMS, "too many items");
		return 0;
	}

	bool Extended = false;
	if(Type >= OFFSET_UUID)
	{
		Extended = true;
		Type = GetTypeFromIndex(GetExtendedItemTypeIndex(Type));
	}

	CSnapshotItem *pObj = (CSnapshotItem *)(m_aData + m_DataSize);

	if(m_Sixup && !Extended)
	{
		if(Type >= 0)
			Type = Obj_SixToSeven(Type);
		else
			Type *= -1;

		if(Type < 0)
			return pObj;
	}

	mem_zero(pObj, sizeof(CSnapshotItem) + Size);
	pObj->m_TypeAndID = (Type << 16) | ID;
	m_aOffsets[m_NumItems] = m_DataSize;
	m_DataSize += sizeof(CSnapshotItem) + Size;
	m_NumItems++;

	return pObj->Data();
}

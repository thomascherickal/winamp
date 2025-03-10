/* ---------------------------------------------------------------------------
Nullsoft Database Engine
--------------------
codename: Near Death Experience
--------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------

Table Class

--------------------------------------------------------------------------- */
#include "../nde.h"
//#include <direct.h>
#include <stdio.h>
#include <string.h>
#include "../CRC.H"
#include <strsafe.h>

const char *tSign="NDETABLE";

//---------------------------------------------------------------------------
Table::Table( const wchar_t *TableName, const wchar_t *Idx, BOOL Create, Database *_db, BOOL _Cached ) : Scanner( this )
{
	memset( column_ids, FIELD_UNKNOWN, 255 );

	Cached     = _Cached;
	db         = _db;
	AutoCreate = Create;
	Name       = ndestring_wcsdup( TableName );
	IdxName    = ndestring_wcsdup( Idx );

	Init();
}

//---------------------------------------------------------------------------
void Table::Init()
{
	numErrors       = 0;
	Scanners        = new LinkedList();
	// benski> cut: Handle=NULL;
	IdxHandle       = NULL;
	FieldsRecord    = NULL;
	IndexList       = NULL;
	GLocateUpToDate = FALSE;
}

//---------------------------------------------------------------------------
Table::~Table()
{
	Reset();
	if (Handle) // Reset doesn'l_data_type completely destroy Handle
		Vfdestroy(Handle);
	Handle = 0;
	
	ndestring_release(Name);
	ndestring_release(IdxName);
}

//---------------------------------------------------------------------------
void Table::Reset()
{
	if ( IndexList )
		IndexList->Release();

	IndexList = 0;

	if ( FieldsRecord )
		FieldsRecord->Release();

	FieldsRecord = 0;

	delete Scanners;
	Scanners = NULL;

	if ( Handle )
		Vfclose( Handle ); // close (but don'l_data_type destroy) to keep mutex open.
	if ( IdxHandle )
		Vfdestroy( IdxHandle );

	IdxHandle = NULL;

	for ( RowCache::iterator itr = rowCache.begin(); itr != rowCache.end(); ++itr )
	{
		if ( itr->second )
			itr->second->Release();
	}

	rowCache.clear();

	memset( column_ids, FIELD_UNKNOWN, 255 );
	columns_cached = false;
}

struct IndexNewWalkerContext
{
	IndexNewWalkerContext(Table *_table)
	{
		N = -1;
		table = _table;
	}
	int N;
	Table *table;
};

bool Table::IndexNewWalker(IndexRecord *record, Field *entry, void *context_in)
{
	IndexNewWalkerContext *context = (IndexNewWalkerContext *)context_in;
	IndexField *p = (IndexField *)entry;
	p->index = new Index(context->table->IdxHandle, p->ID, context->N++, p->Type, FALSE, 0, context->table);
	return true;
}

//---------------------------------------------------------------------------
BOOL Table::Open(void)
{
	int justcreated = 0;

	if (!Handle)
		Handle = Vfnew(Name, "r+b", Cached);
	if (!Handle) return FALSE;
	if (!Vflock(Handle))
	{
		Vfdestroy(Handle);
		Handle = 0;
		return FALSE;
	}

	Handle = Vfopen(Handle, Name, "r+b", Cached);
	IdxHandle = Vfopen(0, IdxName, "r+b", TRUE);
	BOOL Valid = (Handle && IdxHandle);

	// unlock
	if (Valid || !AutoCreate)
	{
		//if (Handle)
			//Vfunlock(Handle);
	}
	else
	{
		if (Handle)
		{
			Vfclose(Handle);
			if (IdxHandle)
				Vfdestroy(IdxHandle);
			IdxHandle = 0;
		}
		else
		{
			if (IdxHandle)
				Vfdestroy(IdxHandle);
			IdxHandle = 0;
			Handle = Vfnew(Name, "w+b", Cached);
			if (!Vflock(Handle))
			{
				Vfdestroy(Handle);
				return FALSE;
			}
		}

		Handle = Vfopen(Handle, Name, "w+b", Cached);
		IdxHandle = Vfopen(0, IdxName, "w+b", TRUE);
		Valid = (Handle && IdxHandle);

		if (Valid)
		{
			Vfwrite(__TABLE_SIGNATURE__, strlen(__TABLE_SIGNATURE__), Handle);
			Vfwrite(__INDEX_SIGNATURE__, strlen(__TABLE_SIGNATURE__), IdxHandle);
			// TODO bensk> change if NUM_SPECIAL_RECORDS ever increases
			int v=NUM_SPECIAL_RECORDS;//strlen(__TABLE_SIGNATURE__);
			Vfwrite(&v, sizeof(v), IdxHandle);
			//    v = 0; fwrite(&v, sizeof(v), 1, IdxHandle);
			v = -1; Vfwrite(&v, sizeof(v), IdxHandle); // write ID
			v = 0; 
			for (int i=0;i<NUM_SPECIAL_RECORDS;i++)
			{
				Vfwrite(&v, sizeof(v), IdxHandle);
				Vfwrite(&v, sizeof(v), IdxHandle);
			}
			Sync();
			justcreated = 1;
		}
	}

	if (!Valid)
	{
		if (Handle) Vfdestroy(Handle);
		if (IdxHandle) Vfdestroy(IdxHandle);
		Handle = NULL;
		IdxHandle = NULL;
	}
	else
	{
		char test1[9]={0};
		char test2[9]={0};

		Vfseek(Handle, 0, SEEK_SET);
		Vfread(test1, strlen(__TABLE_SIGNATURE__), Handle);
		Vfseek(IdxHandle, 0, SEEK_SET);
		Vfread(test2, strlen(__INDEX_SIGNATURE__), IdxHandle);
		test1[8]=0;
		test2[8]=0;
		if (strcmp(test1, __TABLE_SIGNATURE__) || strcmp(test2, __INDEX_SIGNATURE__))
		{
			if (Handle) Vfdestroy(Handle);
			Handle = 0;
			if (IdxHandle) Vfdestroy(IdxHandle);
			IdxHandle = 0;
			return FALSE;
		}

		// Load default index
		IndexField *l_index_field = new IndexField(PRIMARY_INDEX, -1, -1, L"None");
		l_index_field->index = new Index(IdxHandle, PRIMARY_INDEX, -1, -1, FALSE, 0, this);

		// Get indexes
		int Ptr = l_index_field->index->Get(INDEX_RECORD_NUM);
		IndexList = new IndexRecord(Ptr, INDEX_RECORD_NUM, Handle, this);
		if (!IndexList)
		{
			delete l_index_field;
			if (Handle) Vfdestroy(Handle);
			Handle = 0;
			if (IdxHandle) Vfdestroy(IdxHandle);
			IdxHandle = 0;
			return FALSE;
		}

		// Init them
		IndexNewWalkerContext newContext(this);
		IndexList->WalkFields(IndexNewWalker, &newContext);

		// Add default in case its not there (if it is it won'l_data_type be added by addfield)
		IndexList->AddField(l_index_field);

		// Get the default index (whether loaded or preloaded)
		Scanner::index = ((IndexField*)IndexList->GetField(PRIMARY_INDEX))->index;

		// If it's different from preloaded, delete preloaded
		if (l_index_field->index != Scanner::index)
		{
			delete l_index_field;
			l_index_field = NULL;
		}

		// Set up colaboration
		IndexList->BuildCollaboration();

		// Get columns
		Ptr = Scanner::index->Get(FIELDS_RECORD_NUM);
		FieldsRecord = new Record(Ptr, FIELDS_RECORD_NUM, Handle, this);
		if (!FieldsRecord)
		{
			IndexList->Release();
			IndexList=0;
			if (Handle) Vfdestroy(Handle);
			Handle = 0;
			if (IdxHandle) Vfdestroy(IdxHandle);
			IdxHandle = 0;
			return FALSE;
		}

		// update the column cache
		FieldsRecord->WalkFields(BuildColumnCache, this);
		columns_cached=true;
	}

	if (Valid && !justcreated)
	{
		if (IndexList->NeedFix())
			Compact();
	}

	if (Valid) First();
	if (Handle)
		Vfunlock(Handle);
	return Valid;
}

//---------------------------------------------------------------------------
void Table::Close(void)
{
	int v=0;

	if (Handle && IndexList && Vflock(Handle, 0))
	{
		IndexList->WalkFields(IndexWriteWalker, 0);
	}

	delete Scanners;
	Scanners = NULL;

	Vsync(Handle);
	if (IdxHandle)
	{
		Vfdestroy(IdxHandle);
		IdxHandle = NULL;
		v |= 2;
	}
	if (Handle)
	{
		Vfdestroy(Handle);
		Handle = NULL;
		v |= 1;
	}

	if (v != 3)
		return;
}

bool Table::IndexWriteWalker(IndexRecord *record, Field *entry, void *context)
{
	IndexField *field = (IndexField *)entry;
	field->index->WriteIndex();
	return true;
}

//---------------------------------------------------------------------------
void Table::Sync(void)
{
	if (!Vflock(Handle))
		return;

	if (IndexList)
		IndexList->WalkFields(IndexWriteWalker, 0);

	int err=0;
	if (!err && Handle) err|=Vsync(Handle);
	if (!err && IdxHandle) Vsync(IdxHandle);

	Vfunlock(Handle);
}

//---------------------------------------------------------------------------
ColumnField *Table::NewColumn( unsigned char FieldID, const wchar_t *FieldName, unsigned char FieldType, BOOL indexUnique )
{
	columns_cached = false; // if they start writing new columns, kill the columns cache until they PostColumns()
	ColumnField *l_column_field = GetColumnById( FieldID );
	if ( l_column_field )
	{
		int l_data_type = l_column_field->GetDataType();
		if ( l_data_type != FieldType )
		{
			OutputDebugStringW( L"column " );
			OutputDebugStringW( FieldName );
			OutputDebugStringW( L" already exists but is of the wrong type\n" );

			if ( CompatibleFields( l_data_type, FieldType ) )
			{
				OutputDebugStringW( L"going from one equivalent type to another, converting column\n" );
				l_column_field->SetDataType( FieldType );
				goto aok;
			}
		}

		l_column_field->SetFieldName( (wchar_t *)FieldName );
		return NULL;
	}
aok:
	if ( GetColumnByName( FieldName ) )
		return NULL;

	ColumnField *l_new_column_field = new ColumnField( FieldID, FieldName, FieldType, this );
	column_ids[ FieldID ] = FieldType;
	FieldsRecord->AddField( l_new_column_field );

	return l_new_column_field;
}

void Table::SetFieldSearchableById(unsigned char field_id, bool searchable)
{
	ColumnField *column = GetColumnById(field_id);
	if (column)
		column->SetSearchable(searchable);
	if (searchable)
	{
		search_fields.insert(field_id);
	}
	else
	{
		search_fields.erase(field_id);
	}
}

//---------------------------------------------------------------------------
bool Table::BuildColumnCache(Record *record, Field *entry, void *context)
{
	Table *table = (Table *)context;
	ColumnField *column = (ColumnField *)entry;
	unsigned char field_id=column->GetFieldId();
	table->column_ids[field_id] = column->GetDataType();

	if (column->IsSearchableField())
	{
		table->search_fields.insert(field_id);
	}
	else
	{
		table->search_fields.erase(field_id);
	}
	return true;
}

//---------------------------------------------------------------------------
void Table::PostColumns(void)
{
	FieldsRecord->WriteFields(this, FIELDS_RECORD_NUM);
	memset(column_ids, FIELD_UNKNOWN, 255);
	FieldsRecord->WalkFields(BuildColumnCache, this);
	columns_cached=true;
}

unsigned char Table::GetColumnType(unsigned char Id)
{
	if (columns_cached)
	{
		return column_ids[Id];
	}
	else
	{
		return GetColumnById(Id)->GetDataType();
	}
}

//---------------------------------------------------------------------------
IndexField *Table::GetIndexByName(const wchar_t *name)
{
	if (!IndexList)
		return NULL;
	return IndexList->GetIndexByName(name);
}

//---------------------------------------------------------------------------
IndexField *Table::GetIndexById(unsigned char Id)
{
	if (!IndexList)
		return NULL;
	return (IndexField *)IndexList->GetField(Id);
}

//---------------------------------------------------------------------------
void Table::AddIndexByName(const wchar_t *name, const wchar_t *desc)
{
	ColumnField *header = GetColumnByName(name);
	if (header)
	{
		unsigned char Idx = header->ID;
		AddIndexById(Idx, desc);
	}
}

//---------------------------------------------------------------------------
void Table::AddIndexById( unsigned char Id, const wchar_t *desc )
{
	if ( GetIndexById( Id ) )
		return;

	ColumnField *l_column_field = GetColumnById( Id );
	if ( !l_column_field )
		return;

	IndexField *l_new_index_field = new IndexField( Id, IndexList->GetColumnCount(), l_column_field->GetDataType(), desc );
	l_new_index_field->index      = new Index( IdxHandle, Id, IndexList->GetColumnCount(), l_column_field->GetDataType(), TRUE, Scanner::index->NEntries, this );
	
	IndexField* l_previous_index_field = dynamic_cast<IndexField*>(IndexList->GetLastField());

	IndexList->AddField( l_new_index_field );

	//IndexField* l_previous_index_field = (IndexField*)l_new_index_field->prev;
	if (l_previous_index_field)
	{
		l_previous_index_field->index->Colaborate(l_new_index_field);
	}

	IndexField *l_primary_index_field = (IndexField *)IndexList->GetField( PRIMARY_INDEX );
	if (l_primary_index_field)
	{
		l_new_index_field->index->Colaborate(l_primary_index_field);
	}

	if (l_previous_index_field)
	{
		l_previous_index_field->index->Propagate();
	}

	IndexList->WriteFields( this );
}

//---------------------------------------------------------------------------
BOOL Table::CheckIndexing(void)
{
	if (IndexList->GetColumnCount() == 0) return TRUE;

	for (int i=0;i<Scanner::index->NEntries;i++)
	{		
		if (!IndexList->CheckIndexing(i))
			return FALSE;
	}
	return TRUE;
}

struct IndexWalkerThunkContext
{
	void *context;
	Table *_this;
	Table::IndexWalker callback;
};

bool Table::IndexWalkerThunk(IndexRecord *record, Field *entry, void *context_in)
{
	IndexWalkerThunkContext *context = (IndexWalkerThunkContext *)context_in;
	return context->callback(context->_this, (IndexField *)entry, context->context);
}

//---------------------------------------------------------------------------
void Table::WalkIndices(IndexWalker callback, void *context)
{
	if (IndexList && callback)
	{
		IndexWalkerThunkContext walkerContext = { context, this, callback };
		IndexList->WalkFields(IndexWalkerThunk, &walkerContext);
	}
}

//---------------------------------------------------------------------------
void Table::DropIndex(IndexField *Ptr)
{
	if (!Ptr || Ptr->Type != FIELD_INDEX) return;
	if (Scanner::index == Ptr->index)
	{
		Scanner::index = ((IndexField*)IndexList->GetField(PRIMARY_INDEX))->index;
		IndexList->BuildCollaboration();
	}
	IndexList->RemoveField(Ptr);
	if (Scanner::index->SecIndex == Ptr)
		Scanner::index->SecIndex = 0;
	IndexList->WriteFields(this);
}

//---------------------------------------------------------------------------
void Table::DropIndexByName(const wchar_t *desc)
{
	IndexField *indx = GetIndexByName(desc);
	if (!_wcsicmp(desc, L"None")) return;

	if (indx)
		DropIndex(indx);
}

//---------------------------------------------------------------------------
void Table::DropIndexById(unsigned char Id)
{
	if (!IndexList)
		return;
	if (Id == (unsigned char)PRIMARY_INDEX) return;
	IndexField *indx=(IndexField *)IndexList->GetField(Id);
	if (indx)
		DropIndex(indx);
}

//---------------------------------------------------------------------------
BOOL Table::LocateByIdEx(int Id, int From, Field *field, int comp_mode)
{
	return Scanner::LocateByIdEx(Id, From, field, NULL, comp_mode);
}

//---------------------------------------------------------------------------
Record *Table::GetColumns(void)
{
	if (!FieldsRecord)
		return NULL;
	return FieldsRecord;
}

//---------------------------------------------------------------------------
Scanner *Table::NewScanner()
{
	Scanner *s = new Scanner(this);
	/*if (Scanners->GetNElements() > 0)*/
	s->index = Scanner::index;
	Scanners->AddEntry(s, TRUE);
	return s;
}

//---------------------------------------------------------------------------
Scanner *Table::GetDefaultScanner()
{
	return this;
}

//---------------------------------------------------------------------------
void Table::DeleteScanner(Scanner *scan)
{
	if (!scan) return;
		Scanners->RemoveEntry(scan);
}

//---------------------------------------------------------------------------
void Table::IndexModified(void)
{
	Scanner *s = (Scanner *)Scanners->GetHead();
	while (s)
	{
		s->IndexModified();
		s = (Scanner *)s->GetNext();
	}
}

//---------------------------------------------------------------------------
void Table::SetGlobalLocateUpToDate(BOOL is) {
	GLocateUpToDate = is;
}

struct ColumnWalkContext
{
	Table *ctable;
};

bool Table::Compact_ColumnWalk(Record *record, Field *entry, void *context_in)
{
	ColumnField *field = static_cast<ColumnField *>(entry);
	ColumnWalkContext *context = (ColumnWalkContext *)context_in;
	Table *ctable = context->ctable;

	ctable->NewColumn(field->GetFieldId(), field->GetFieldName(), field->GetDataType(), FALSE);
	return true;
}

struct ColumnWalk2Context
{
	Table *ctable;
	Table *thisTable;
	uint8_t *data;
	size_t data_size;
	int gotstuff;
};

bool Table::Compact_ColumnWalk2(Record *record, Field *entry, void *context_in)
{
	ColumnField *colfield = (ColumnField *)entry;
	ColumnWalk2Context *context = (ColumnWalk2Context *)context_in;

	unsigned char fieldid = colfield->GetFieldId();
	//char *fieldname = colfield->GetFieldName();
	Field *mfield = context->thisTable->GetFieldById(fieldid);
	//Field *mfield = GetFieldByName(fieldname);
	if (mfield != NULL) {
		if (!context->gotstuff) {
			context->ctable->New();
			context->gotstuff = 1;
		}
		Field *cfield = context->ctable->NewFieldById(fieldid, 0);
		//Field *cfield = ctable->NewFieldByName(fieldname, mfield->GetPerm());
		size_t len = mfield->GetDataSize();
		if (len > context->data_size)
		{
			size_t old_data_size = context->data_size;
			context->data_size = len;
			uint8_t *new_data = (uint8_t *)realloc(context->data, context->data_size);
			if (new_data)
			{
				context->data = new_data;
			}
			else
			{
				new_data = (uint8_t *)malloc(context->data_size);
				if (new_data)
				{
					memcpy(new_data, context->data, old_data_size);
					free(context->data);
					context->data = new_data;
				}
				else
				{
					context->data_size = old_data_size;
					return false;
				}
			}
		}
		mfield->WriteTypedData(context->data, len);
		cfield->ReadTypedData(context->data, len);
	}

	return true;
}

bool Table::Compact_IndexWalk(Table *table, IndexField *field, void *context)
{
	Table *ctable = (Table *)context;

	if (_wcsicmp(field->GetIndexName(), L"None"))
		ctable->AddIndexById(field->GetFieldId(), field->GetIndexName());
	return true;
}
//---------------------------------------------------------------------------
void Table::Compact(int *progress) {
	// ok so we're gonna be cheating a bit, instead of figuring out how to safely modify all those
	// nifty indexes that cross reference themselves and blablabla, we're just gonna duplicate the
	// whole table from scratch, overwrite ourselves, and reopen the table. duh.

	if (!Vflock(Handle))
	{
		if (progress != NULL) *progress = 100;
		return;
	}
	// create a temporary table in windows temp dir
	wchar_t temp_table[MAX_PATH+12] = {0};
	wchar_t temp_index[MAX_PATH+12] = {0};
	wchar_t old_table[MAX_PATH+12] = {0};
	wchar_t old_index[MAX_PATH+12] = {0};
	DWORD pid=GetCurrentProcessId();

	StringCbPrintfW(temp_table, sizeof(temp_table), L"%s.new%08X", Name,pid);
	StringCbPrintfW(temp_index, sizeof(temp_index),L"%s.new%08X", IdxName,pid);
	StringCbPrintfW(old_table, sizeof(old_table),L"%s.old%08X", Name,pid);
	StringCbPrintfW(old_index, sizeof(old_index),L"%s.old%08X", IdxName,pid);

	// delete them, in case we crashed while packing

	DeleteFileW(temp_table);
	DeleteFileW(temp_index);
	DeleteFileW(old_table);
	DeleteFileW(old_index);

	// create a brand new db and a brand new table
	Table *ctable = db->OpenTable(temp_table, temp_index, NDE_OPEN_ALWAYS, Cached);

	// duplicate the columns
	Record *record = GetColumns();
	if (record != NULL) 
	{
		ColumnWalkContext context;
		context.ctable = ctable;
		record->WalkFields(Compact_ColumnWalk, &context);
	}
	ctable->PostColumns();

	// duplicate the indexes
	WalkIndices(Compact_IndexWalk, (void *)ctable);

	// duplicate the data
	int reccount = GetRecordsCount();

	int count = 0;
	First();
	ColumnWalk2Context context;
	context.data_size = 65536;
	context.data = (uint8_t *)calloc(65536, sizeof(uint8_t));
	context.ctable = ctable;
	context.thisTable = this;

	while (1) {
		int lasterr = NumErrors();
		GetDefaultScanner()->GetRecordById(count, FALSE);
		count++;

		if (Eof() || count > reccount) break;

		if (NumErrors() > lasterr) 
			continue;

		Index *idx = GetDefaultScanner()->GetIndex();
		int pos = idx->Get(GetDefaultScanner()->GetRecordId());

		if (pos == 0) continue;

		int pr = (int)((float)GetRecordId()/(float)reccount*100.0f);
		if (progress != NULL) *progress = pr;
		context.gotstuff = 0;

		if (record != NULL)
			record->WalkFields(Compact_ColumnWalk2, &context);

		if (context.gotstuff) 
			ctable->Post(); 
	}
	free(context.data);

	// done creating temp table
	db->CloseTable(ctable);

	// reset the data structures and close underlying file handles
	Reset();

	if (MoveFileW(Name,old_table))
	{
		if (MoveFileW(IdxName,old_index))
		{
			if (!MoveFileW(temp_table,Name) || !MoveFileW(temp_index,IdxName))
			{
				// failed, try to copy back
				DeleteFileW(Name);
				DeleteFileW(IdxName);
				MoveFileW(old_table,Name); // restore old file
				MoveFileW(old_index,IdxName); // restore old file
			}
		}
		else
		{
			MoveFileW(old_table,Name); // restore old file
		}
	}

	// clean up our temp files
	DeleteFileW(temp_table);
	DeleteFileW(temp_index);
	DeleteFileW(old_table);
	DeleteFileW(old_index);

	if (progress != NULL) *progress = 100;

	// reopen our table
	Init();
	Open();
}

ColumnField *Table::GetColumnById(unsigned char Idx)
{
	if (!FieldsRecord)
		return NULL;
	return (ColumnField *)FieldsRecord->GetField(Idx);
}

ColumnField *Table::GetColumnByName(const wchar_t *FieldName)
{
	return FieldsRecord->GetColumnByName(FieldName);
}

void Table::RowCache_Delete(int position)
{
	if (use_row_cache)
	{
		RowCache::iterator found = rowCache.find(position);
		if (found != rowCache.end())
		{
			if (found->second)
				found->second->Release();
			rowCache.erase(found);
		}
	}
}

void Table::RowCache_Remove(int position)
{
	if (use_row_cache)
	{
		Record *&row = rowCache[position];
		if (row)
		{
			row->Release();
		}

		rowCache[position] = 0;
	}
}

void Table::RowCache_Add(Record *record, int position)
{
	if (use_row_cache)
	{
		record->Retain();

		Record *&row = rowCache[position];
		if (row)
		{
			row->Release();
		}

		rowCache[position] = record;
	}
}

Record *Table::RowCache_Get(int position)
{
	if (!use_row_cache || 0 == rowCache.count(position))
		return 0;

	Record *row = rowCache[position];
	if (row)
		row->Retain();
	return row;
}

void Table::EnableRowCache()
{
	use_row_cache=true;
}
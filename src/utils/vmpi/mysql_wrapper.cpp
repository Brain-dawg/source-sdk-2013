//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

// mysql_wrapper.cpp : Defines the entry point for the DLL application.
//
#include "stdafx.h"


#include "imysqlwrapper.h"
#include "mysql_wrapper.h"
#include "mysql.h"
#include <stdio.h>

#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&) = delete; \
  void operator=(const TypeName&) = delete;

char* CopyString( const char *pStr )
{
	if ( !pStr )
	{
		pStr = "";
	}

	int len = V_strlen( pStr ) + 1;
	char *pRet = (char*)MemAlloc_Alloc( len );
	V_strncpy( pRet, pStr, len );
	return pRet;
}


void FreeCopiedString( char* pStr )
{
	if ( pStr )
	{
		MemAlloc_Free( pStr );
	}
}


BOOL APIENTRY DllMain( HANDLE hModule, 
                       DWORD  ul_reason_for_call, 
                       LPVOID lpReserved
					 )
{
    return TRUE;
}


class CCopiedRow
{
public:
	~CCopiedRow()
	{
		m_Columns.PurgeAndDeleteElements();
	}

	CUtlVector<char*> m_Columns;
};


class CMySQLCopiedRowSet : public IMySQLRowSet
{
public:
	~CMySQLCopiedRowSet()
	{
		m_Rows.PurgeAndDeleteElements();
		m_ColumnNames.PurgeAndDeleteElements();
	}

	virtual void			Release()
	{
		delete this;
	}

	virtual int				NumFields()
	{
		return m_ColumnNames.Count();
	}

	virtual const char*		GetFieldName( int iColumn )
	{
		return m_ColumnNames[iColumn];
	}

	virtual bool			NextRow()
	{
		++m_iCurRow;
		return m_iCurRow < m_Rows.Count();
	}

	virtual bool			SeekToFirstRow()
	{
		m_iCurRow = 0;
		return m_iCurRow < m_Rows.Count();
	}

	virtual CColumnValue	GetColumnValue( int iColumn )
	{
		return CColumnValue( this, iColumn );
	}

	virtual CColumnValue	GetColumnValue( const char *pColumnName )
	{
		return CColumnValue( this, GetColumnIndex( pColumnName ) );
	}

	virtual const char*		GetColumnValue_String( int iColumn )
	{
		if ( iColumn < 0 || iColumn >= m_ColumnNames.Count() )
			return "<invalid column specified>";
		else if ( m_iCurRow < 0 || m_iCurRow >= m_Rows.Count() )
			return "<invalid row specified>";
		else
			return m_Rows[m_iCurRow]->m_Columns[iColumn];
	}

	virtual long			GetColumnValue_Int( int iColumn )
	{
		return atoi( GetColumnValue_String( iColumn ) );
	}

	virtual int				GetColumnIndex( const char *pColumnName )
	{
		for ( int i=0; i < m_ColumnNames.Count(); i++ )
		{
			if ( stricmp( m_ColumnNames[i], pColumnName ) == 0 )
				return i;
		}
		return -1;
	}


public:
	int m_iCurRow;
	CUtlVector<CCopiedRow*> m_Rows;
	CUtlVector<char*> m_ColumnNames;
};


// -------------------------------------------------------------------------------------------------------- //
// CMySQL class.
// -------------------------------------------------------------------------------------------------------- //

class CMySQLStatement;

class CMySQL : public IMySQL
{
private:
	CThreadMutex m_Mutex;
	char m_szHostName[128];
    char m_szUserName[128];
    char m_szDBName[128];
public:
							CMySQL();
	virtual					~CMySQL();

	virtual bool			InitMySQL( const char *pDBName, const char *pHostName, const char *pUserName, const char *pPassword );
	virtual void			Release();
	virtual int				Execute( const char *pString );
	virtual IMySQLRowSet*	DuplicateRowSet();
	virtual unsigned long	InsertID();
	virtual int				NumFields();
	virtual const char*		GetFieldName( int iColumn );
	virtual bool			NextRow();
	virtual bool			SeekToFirstRow();
	virtual CColumnValue	GetColumnValue( int iColumn );
	virtual CColumnValue	GetColumnValue( const char *pColumnName );
	virtual const char*		GetColumnValue_String( int iColumn );
	virtual long			GetColumnValue_Int( int iColumn );
	virtual int				GetColumnIndex( const char *pColumnName );

	// Cancels the storage of the rows from the latest query.
	void					CancelIteration();

	virtual const char *	GetLastError();

	void					EscapeString(const char* input, char* output, size_t outputSize);

	void					BuildSafeQuery(char* buffer, int bufferSize, const char* format, ...);

	CMySQLStatement*		PrepareStatement(const char *query);

	bool					BeginTransaction();
	bool					Commit();
	bool					Rollback();
	bool					TableExists(const char* tableName);
	int						GetRowCount(const char* tableName);

	bool					Ping();
	bool					Reconnect();

public:

	MYSQL		*m_pSQL;
	MYSQL_RES	*m_pResult;
	MYSQL_ROW	m_Row;
	CUtlVector<MYSQL_FIELD>	m_Fields;

	char m_szLastError[128];
};


EXPOSE_INTERFACE( CMySQL, IMySQL, MYSQL_WRAPPER_VERSION_NAME );


// -------------------------------------------------------------------------------------------------------- //
// CMySQL implementation.
// -------------------------------------------------------------------------------------------------------- //

class MySQLResultGuard
{
private:
	MYSQL_RES *m_pResult;
public:
	explicit MySQLResultGuard(MYSQL_RES *result) : m_pResult(result) {}
	~MySQLResultGuard() { if (m_pResult) mysql_free_result(m_pResult); }
	
	MYSQL_RES* Get() { return m_pResult; }
	
	// Prevent copying using Source Engine macros
	DISALLOW_COPY_AND_ASSIGN(MySQLResultGuard);
};

CMySQL::CMySQL()
{
	m_pSQL = NULL;
	m_pResult = NULL;
	m_Row = NULL;
}


CMySQL::~CMySQL()
{
	CancelIteration();

	if ( m_pSQL )
	{
		mysql_close( m_pSQL );
		m_pSQL = NULL;
	}
}


bool CMySQL::InitMySQL( const char *pDBName, const char *pHostName, const char *pUserName, const char *pPassword )
{
	// Store connection parameters for later use
    V_strncpy(m_szHostName, pHostName, sizeof(m_szHostName));
    V_strncpy(m_szUserName, pUserName, sizeof(m_szUserName));
    V_strncpy(m_szDBName, pDBName, sizeof(m_szDBName));
	MYSQL *pSQL = mysql_init(NULL);
	if (!pSQL)
		return false;
		
	// Set connection timeout to avoid hanging
	int timeout = 5;  // 5 seconds
	mysql_options(pSQL, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
	
	if (mysql_real_connect(pSQL, pHostName, pUserName, pPassword, pDBName, 0, NULL, 0))
	{
		m_pSQL = pSQL;
		
		// Set UTF-8 character set
		if (mysql_set_character_set(m_pSQL, "utf8") != 0)
		{
			Warning("Failed to set character set to UTF-8: %s\n", mysql_error(m_pSQL));
		}
		
		// Get server version
		unsigned long version = mysql_get_server_version(m_pSQL);
		DevMsg("Connected to MySQL server version %d.%d.%d\n", 
			   version / 10000, (version % 10000) / 100, version % 100);
			   
		// Adjust behavior based on version if needed
		//m_bSupportsJSON = (version >= 50700);  // JSON support added in 5.7.0
		
		return true;
	}
	
	// Store error before closing connection
	V_strncpy(m_szLastError, mysql_error(pSQL), sizeof(m_szLastError));
	mysql_close(pSQL);
	
	return false;
}


void CMySQL::Release()
{
	delete this;
}


int CMySQL::Execute( const char *pString )
{
	CFastTimer timer;
	timer.Start();
	
	//if (mysql_developer.GetBool())
	//{
	//	DevMsg("MySQL Query: %s\n", pString);
	//}
	
	CancelIteration();
	int result = mysql_query( m_pSQL, pString );
	
	if (result != 0)
	{
		Warning("MySQL Error: %s\n", mysql_error(m_pSQL));
	}

	if ( result == 0 )
	{
		// Is this a query with a result set?
		m_pResult = mysql_store_result( m_pSQL );
		if ( m_pResult )
		{
			// Store the field information.
			int count = mysql_field_count( m_pSQL );
			MYSQL_FIELD *pFields = mysql_fetch_fields( m_pResult );
			m_Fields.CopyArray( pFields, count );
			return 0;
		}
		else
		{
			// No result set. Was a set expected?
			if ( mysql_field_count( m_pSQL ) != 0 )
				return 1;	// error! The query expected data but didn't get it.
		}
	}

	timer.End();
	
	//if (mysql_developer.GetBool())
	//{
	//	DevMsg("MySQL Query took %.3f ms\n", timer.GetDuration().GetMillisecondsF());
	//}
	
	return result;
}


IMySQLRowSet* CMySQL::DuplicateRowSet()
{
	if (!m_pResult)
		return NULL;
		
	CMySQLCopiedRowSet *pSet = new CMySQLCopiedRowSet;
	pSet->m_iCurRow = -1;

	// Get row count for pre-allocation
	my_ulonglong numRows = mysql_num_rows(m_pResult);
	if (numRows > 0)
	{
		pSet->m_Rows.EnsureCapacity((int)numRows);
	}
	
	// Pre-allocate memory
	int fieldCount = m_Fields.Count();
	pSet->m_ColumnNames.EnsureCapacity(fieldCount);
	
	while ( NextRow() )
	{
		CCopiedRow *pRow = new CCopiedRow;
		pSet->m_Rows.AddToTail( pRow );
		pRow->m_Columns.SetSize( m_Fields.Count() );

		for ( int i=0; i < m_Fields.Count(); i++ )
		{
			pRow->m_Columns[i] = CopyString( m_Row[i] );
		}
	}

	return pSet;
}


unsigned long CMySQL::InsertID()
{
	return mysql_insert_id( m_pSQL );
}


int CMySQL::NumFields()
{
	return m_Fields.Count();
}


const char* CMySQL::GetFieldName( int iColumn )
{
	return m_Fields[iColumn].name;
}


bool CMySQL::NextRow()
{
	if ( !m_pResult )
		return false;

	m_Row = mysql_fetch_row( m_pResult );
	if ( m_Row == 0 )
	{
		return false;
	}
	else
	{
		return true;
	}
}


bool CMySQL::SeekToFirstRow()
{
	if ( !m_pResult )
		return false;

	mysql_data_seek( m_pResult, 0 );
	return true;
}


CColumnValue CMySQL::GetColumnValue( int iColumn )
{
	return CColumnValue( this, iColumn );
}


CColumnValue CMySQL::GetColumnValue( const char *pColumnName )
{
	return CColumnValue( this, GetColumnIndex( pColumnName ) );
}


const char* CMySQL::GetColumnValue_String( int iColumn )
{
	if ( m_Row && iColumn >= 0 && iColumn < m_Fields.Count() && m_Row[iColumn] )
		return m_Row[iColumn];
	else
		return "";
}


long CMySQL::GetColumnValue_Int( int iColumn )
{
	return atoi( GetColumnValue_String( iColumn ) );
}


int CMySQL::GetColumnIndex( const char *pColumnName )
{
	for ( int i=0; i < m_Fields.Count(); i++ )
	{
		if ( stricmp( pColumnName, m_Fields[i].name ) == 0 )
		{
			return i;
		}
	}
	
	return -1;
}


void CMySQL::CancelIteration()
{
	m_Fields.Purge();
	
	if ( m_pResult )
	{
		mysql_free_result( m_pResult );
		m_pResult = NULL;
	}

	m_Row = NULL;
}

const char* CMySQL::GetLastError()
{
	static char szErrorBuffer[1024];
	
	if (m_pSQL)
	{
		V_snprintf(szErrorBuffer, sizeof(szErrorBuffer), "MySQL Error (%d): %s", 
				  mysql_errno(m_pSQL), mysql_error(m_pSQL));
	}
	else
	{
		V_strncpy(szErrorBuffer, m_szLastError, sizeof(szErrorBuffer));
	}
	
	return szErrorBuffer;
}

void CMySQL::EscapeString(const char* input, char* output, size_t outputSize)
{
	if (!m_pSQL || !input || !output || outputSize == 0)
		return;
		
	// Leave room for null terminator
	mysql_real_escape_string(m_pSQL, output, input, V_strlen(input));
}

void CMySQL::BuildSafeQuery(char* buffer, int bufferSize, const char* format, ...)
{
	char tempBuffer[8192];
	va_list args;
	va_start(args, format);
	V_vsnprintf(tempBuffer, sizeof(tempBuffer), format, args);
	va_end(args);
	
	EscapeString(tempBuffer, buffer, bufferSize);
}

// Add basic prepared statement support
class CMySQLStatement
{
private:
	MYSQL_STMT *m_pStmt;
	CMySQL *m_pSQL;
	
public:
	CMySQLStatement(MYSQL_STMT *stmt, CMySQL *pSQL) : m_pStmt(stmt), m_pSQL(pSQL) {}
	~CMySQLStatement() { if (m_pStmt) mysql_stmt_close(m_pStmt); }
	
	// Basic bind and execute methods
	bool Execute();
	bool BindInt(int paramIndex, int value);
	bool BindString(int paramIndex, const char* value);
	// More bind methods as needed...
	
	DISALLOW_COPY_AND_ASSIGN(CMySQLStatement);
};

CMySQLStatement* CMySQL::PrepareStatement(const char *query)
{
	if (!m_pSQL) return NULL;
	
	MYSQL_STMT *stmt = mysql_stmt_init(m_pSQL);
	if (!stmt) return NULL;
	
	if (mysql_stmt_prepare(stmt, query, V_strlen(query)))
	{
		Warning("Failed to prepare statement: %s\n", mysql_stmt_error(stmt));
		mysql_stmt_close(stmt);
		return NULL;
	}
	
	return new CMySQLStatement(stmt, this);
}

bool CMySQL::BeginTransaction()
{
	return Execute("START TRANSACTION") == 0;
}

bool CMySQL::Commit()
{
	return Execute("COMMIT") == 0;
}

bool CMySQL::Rollback()
{
	return Execute("ROLLBACK") == 0;
}

// Example usage:
// pSQL->BeginTransaction();
// for (int i = 0; i < numQueries; i++) {
//     if (pSQL->Execute(queries[i]) != 0) {
//         pSQL->Rollback();
//         return false;
//     }
// }
// pSQL->Commit();

// Add helper methods for common operations
bool CMySQL::TableExists(const char* tableName)
{
	char query[256];
	V_snprintf(query, sizeof(query), "SHOW TABLES LIKE '%s'", tableName);
	
	if (Execute(query) != 0)
		return false;
		
	return mysql_num_rows(m_pResult) > 0;
}

int CMySQL::GetRowCount(const char* tableName)
{
	char query[256];
	V_snprintf(query, sizeof(query), "SELECT COUNT(*) FROM %s", tableName);
	
	if (Execute(query) != 0)
		return -1;
		
	if (!NextRow())
		return -1;
		
	return GetColumnValue_Int(0);
}

bool CMySQL::Ping()
{
	if (!m_pSQL)
		return false;
		
	return mysql_ping(m_pSQL) == 0;
}

bool CMySQL::Reconnect()
{
    if (!m_pSQL)
        return false;
        
    // Close existing connection
    mysql_close(m_pSQL);
    m_pSQL = NULL;
    
    // Reconnect using stored parameters
    return InitMySQL(m_szDBName, m_szHostName, m_szUserName, "");  // Note: password can't be stored securely
}
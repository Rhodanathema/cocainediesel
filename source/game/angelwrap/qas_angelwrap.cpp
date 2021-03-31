/*
Copyright (C) 2008 German Garcia

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "qas_local.h"
#include "addon/addon_math.h"
#include "addon/addon_scriptarray.h"
#include "addon/addon_string.h"
#include "addon/addon_vec3.h"
#include "addon/addon_cvar.h"

#include <list>
#include <map>

static void *qasAlloc( size_t size ) {
	return G_Malloc( size );
}

static void qasFree( void *mem ) {
	G_Free( mem );
}

// ============================================================================

// list of contexts in the same engine
typedef std::list<asIScriptContext *> qasContextList;

// engine -> contexts key/value pairs
typedef std::map<asIScriptEngine *, qasContextList> qasEngineContextMap;

qasEngineContextMap contexts;

// ============================================================================

static void qasMessageCallback( const asSMessageInfo *msg ) {
	const char *msg_type;

	switch( msg->type ) {
		case asMSGTYPE_ERROR:
			msg_type = S_COLOR_RED "ERROR: ";
			break;
		case asMSGTYPE_WARNING:
			msg_type = S_COLOR_YELLOW "WARNING: ";
			break;
		case asMSGTYPE_INFORMATION:
		default:
			msg_type = S_COLOR_CYAN "ANGELSCRIPT: ";
			break;
	}

	Com_Printf( "%s%s %d:%d: %s\n", msg_type, msg->section, msg->row, msg->col, msg->message );
}

static void qasExceptionCallback( asIScriptContext *ctx ) {
	int line, col;
	asIScriptFunction *func;
	const char *sectionName, *exceptionString, *funcDecl;

	line = ctx->GetExceptionLineNumber( &col, &sectionName );
	func = ctx->GetExceptionFunction();
	exceptionString = ctx->GetExceptionString();
	funcDecl = ( func ? func->GetDeclaration( true ) : "" );

	Com_Printf( S_COLOR_RED "ASModule::ExceptionCallback:\n%s %d:%d %s: %s\n", sectionName, line, col, funcDecl, exceptionString );
}

asIScriptEngine *qasCreateEngine( bool *asMaxPortability ) {
	asIScriptEngine *engine;

	// register the global memory allocation and deallocation functions
	asSetGlobalMemoryFunctions( qasAlloc, qasFree );

	// always new

	// ask for angelscript initialization and script engine creation
	engine = asCreateScriptEngine( ANGELSCRIPT_VERSION );
	if( !engine ) {
		return NULL;
	}

	if( strstr( asGetLibraryOptions(), "AS_MAX_PORTABILITY" ) ) {
		Com_Printf( "* angelscript library with AS_MAX_PORTABILITY detected\n" );
		engine->Release();
		return NULL;
	}

	*asMaxPortability = false;

	// The script compiler will write any compiler messages to the callback.
	engine->SetMessageCallback( asFUNCTION( qasMessageCallback ), 0, asCALL_CDECL );
	engine->SetEngineProperty( asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT, 1 );
	engine->SetDefaultAccessMask( 0xFFFFFFFF );

	PreRegisterMathAddon( engine );
	PreRegisterScriptArray( engine, true );
	PreRegisterStringAddon( engine );
	PreRegisterVec3Addon( engine );
	PreRegisterCvarAddon( engine );

	RegisterMathAddon( engine );
	RegisterScriptArray( engine, true );
	RegisterStringAddon( engine );
	RegisterVec3Addon( engine );
	RegisterCvarAddon( engine );

	engine->SetDefaultAccessMask( 0x1 );

	return engine;
}

void qasReleaseEngine( asIScriptEngine *engine ) {
	if( !engine ) {
		return;
	}

	// release all contexts linked to this engine
	qasContextList &ctxList = contexts[engine];
	for( qasContextList::iterator it = ctxList.begin(); it != ctxList.end(); it++ ) {
		asIScriptContext *ctx = *it;
		ctx->Release();
	}
	ctxList.clear();

	qasEngineContextMap::iterator it = contexts.find( engine );
	if( it != contexts.end() ) {
		contexts.erase( it );
	}

	engine->Release();
}

static asIScriptContext *qasCreateContext( asIScriptEngine *engine ) {
	asIScriptContext *ctx;
	int error;

	if( engine == NULL ) {
		return NULL;
	}

	// always new
	ctx = engine->CreateContext();
	if( !ctx ) {
		return NULL;
	}

	// We don't want to allow the script to hang the application, e.g. with an
	// infinite loop, so we'll use the line callback function to set a timeout
	// that will abort the script after a certain time. Before executing the
	// script the timeOut variable will be set to the time when the script must
	// stop executing.

	error = ctx->SetExceptionCallback( asFUNCTION( qasExceptionCallback ), NULL, asCALL_CDECL );
	if( error < 0 ) {
		ctx->Release();
		return NULL;
	}

	qasContextList &ctxList = contexts[engine];
	ctxList.push_back( ctx );

	return ctx;
}

void qasReleaseContext( asIScriptContext *ctx ) {
	if( !ctx ) {
		return;
	}

	asIScriptEngine *engine = ctx->GetEngine();
	qasContextList &ctxList = contexts[engine];
	ctxList.remove( ctx );

	ctx->Release();
}

asIScriptContext *qasAcquireContext( asIScriptEngine *engine ) {
	if( !engine ) {
		return NULL;
	}

	// try to reuse any context linked to this engine
	qasContextList &ctxList = contexts[engine];
	for( qasContextList::iterator it = ctxList.begin(); it != ctxList.end(); it++ ) {
		asIScriptContext *ctx = *it;
		if( ctx->GetState() == asEXECUTION_FINISHED ) {
			return ctx;
		}
	}

	// if no context was available, create a new one
	return qasCreateContext( engine );
}

asIScriptContext *qasGetActiveContext() {
	return asGetActiveContext();
}

/*************************************
* Scripts
**************************************/

/*
* qasLoadScriptSection
*/
static char *qasLoadScriptSection( const char *rootDir, const char *dir, const char *script, int sectionNum ) {
	char filename[MAX_QPATH];
	uint8_t *data;
	int length, filenum;
	char *sectionName;

	sectionName = COM_ListNameForPosition( script, sectionNum, QAS_SECTIONS_SEPARATOR );
	if( !sectionName ) {
		return NULL;
	}

	COM_StripExtension( sectionName );

	while( *sectionName == '\n' || *sectionName == ' ' || *sectionName == '\r' )
		sectionName++;

	if( sectionName[0] == '/' ) {
		snprintf( filename, sizeof( filename ), "%s%s%s", rootDir, sectionName, QAS_FILE_EXTENSION );
	} else {
		snprintf( filename, sizeof( filename ), "%s/%s/%s%s", rootDir, dir, sectionName, QAS_FILE_EXTENSION );
	}
	Q_strlwr( filename );

	length = FS_FOpenFile( filename, &filenum, FS_READ );

	if( length == -1 ) {
		Com_Printf( "Couldn't find script section: '%s'\n", filename );
		return NULL;
	}

	//load the script data into memory
	data = ( uint8_t * )qasAlloc( length + 1 );
	FS_Read( data, length, filenum );
	FS_FCloseFile( filenum );

	return (char *)data;
}

/*
* qasBuildScriptProject
*/
static asIScriptModule *qasBuildScriptProject( asIScriptEngine *asEngine, const char *moduleName, const char *rootDir, const char *dir, const char *scriptName, const char *script ) {
	int error;
	int numSections, sectionNum;
	char *section;
	asIScriptModule *asModule;

	if( asEngine == NULL ) {
		Com_Printf( S_COLOR_RED "qasBuildGameScript: Angelscript API unavailable\n" );
		return NULL;
	}

	// count referenced script sections
	for( numSections = 0; ( section = COM_ListNameForPosition( script, numSections, QAS_SECTIONS_SEPARATOR ) ) != NULL; numSections++ ) ;

	if( !numSections ) {
		Com_Printf( S_COLOR_RED "* Error: script '%s' has no sections\n", scriptName );
		return NULL;
	}

	// load up the script sections

	asModule = asEngine->GetModule( moduleName, asGM_CREATE_IF_NOT_EXISTS );
	if( asModule == NULL ) {
		Com_Printf( S_COLOR_RED "qasBuildGameScript: GetModule '%s' failed\n", moduleName );
		return NULL;
	}

	for( sectionNum = 0; ( section = qasLoadScriptSection( rootDir, dir, script, sectionNum ) ) != NULL; sectionNum++ ) {
		const char *sectionName = COM_ListNameForPosition( script, sectionNum, QAS_SECTIONS_SEPARATOR );
		error = asModule->AddScriptSection( sectionName, section, strlen( section ) );

		qasFree( section );

		if( error ) {
			Com_Printf( S_COLOR_RED "* Failed to add the script section %s with error %i\n", sectionName, error );
			asEngine->DiscardModule( moduleName );
			return NULL;
		}
	}

	if( sectionNum != numSections ) {
		Com_Printf( S_COLOR_RED "* Error: couldn't load all script sections.\n" );
		asEngine->DiscardModule( moduleName );
		return NULL;
	}

	error = asModule->Build();
	if( error ) {
		Com_Printf( S_COLOR_RED "* Failed to build script '%s'\n", scriptName );
		asEngine->DiscardModule( moduleName );
		return NULL;
	}

	return asModule;
}

/*
* qasLoadScriptProject
*/
asIScriptModule *qasLoadScriptProject( asIScriptEngine *engine, const char *moduleName, const char *rootDir, const char *dir, const char *filename, const char *ext ) {
	int length, filenum;
	char *data;
	char filepath[MAX_QPATH];
	asIScriptModule *asModule;

	snprintf( filepath, sizeof( filepath ), "%s/%s/%s", rootDir, dir, filename );
	COM_DefaultExtension( filepath, ext, sizeof( filepath ) );

	length = FS_FOpenFile( filepath, &filenum, FS_READ );

	if( length == -1 ) {
		Com_Printf( "qasLoadScriptProject: Couldn't find '%s'.\n", filepath );
		return NULL;
	}

	if( !length ) {
		Com_Printf( "qasLoadScriptProject: '%s' is empty.\n", filepath );
		FS_FCloseFile( filenum );
		return NULL;
	}

	//load the script data into memory
	data = ( char * )qasAlloc( length + 1 );
	FS_Read( data, length, filenum );
	FS_FCloseFile( filenum );

	// Initialize the script
	asModule = qasBuildScriptProject( engine, moduleName, rootDir, dir, filepath, data );
	if( !asModule ) {
		qasFree( data );
		return NULL;
	}

	qasFree( data );
	return asModule;
}

/*************************************
* Array tools
**************************************/

CScriptArrayInterface *qasCreateArrayCpp( unsigned int length, void *ot ) {
	return QAS_NEW( CScriptArray )( length, static_cast<asIObjectType *>( ot ) );
}

void qasReleaseArrayCpp( CScriptArrayInterface *arr ) {
	arr->Release();
}

/*************************************
* Strings
**************************************/

asstring_t *qasStringFactoryBuffer( const char *buffer, unsigned int length ) {
	return objectString_FactoryBuffer( buffer, length );
}

void qasStringRelease( asstring_t *str ) {
	objectString_Release( str );
}

asstring_t *qasStringAssignString( asstring_t *self, const char *string, unsigned int strlen ) {
	return objectString_AssignString( self, string, strlen );
}

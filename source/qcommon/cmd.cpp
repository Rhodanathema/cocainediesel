#include <algorithm> // std::sort

#include "qcommon/qcommon.h"
#include "qcommon/array.h"
#include "qcommon/fs.h"
#include "qcommon/hash.h"
#include "qcommon/hashtable.h"
#include "qcommon/string.h"

struct ConsoleCommand {
	char * name;
	ConsoleCommandCallback callback;
	TabCompletionCallback tab_completion_callback;
	bool disabled;
};

constexpr size_t MAX_COMMANDS = 1024;

static ConsoleCommand commands[ MAX_COMMANDS ];
static Hashtable< MAX_COMMANDS * 2 > commands_hashtable;

static Span< const char > GrabLine( Span< const char > str, bool * eof ) {
	const char * newline = StrChr( str, '\n' );
	*eof = newline == NULL;
	return newline == NULL ? str : str.slice( 0, newline - str.ptr );
}

static ConsoleCommand * FindCommand( Span< const char > name ) {
	u64 hash = CaseHash64( name );
	u64 idx;
	if( !commands_hashtable.get( hash, &idx ) )
		return NULL;
	return &commands[ idx ];
}

static ConsoleCommand * FindCommand( const char * name ) {
	return FindCommand( MakeSpan( name ) );
}

static Span< Span< const char > > TokenizeString( Allocator * a, Span< const char > str ) {
	NonRAIIDynamicArray< Span< const char > > tokens;
	tokens.init( a );

	while( true ) {
		Span< const char > token = ParseToken( &str, Parse_StopOnNewLine );
		if( token.ptr == NULL )
			return;
		tokens.add( token );
	}

	return tokens.span();
}

Span< Span< const char > > TokenizeString( Allocator * a, const char * str ) {
	return TokenizeString( a, MakeSpan( str ) );
}

bool Cmd_ExecuteLine( Allocator * a, Span< const char > line, bool warn_on_invalid ) {
	Span< Span< const char > > tokens = TokenizeString( str );
	if( tokens.n == 0 ) {
		return true;
	}

	Span< const char > args;
	if( tokens.n >= 2 ) {
		args = Span< const char >( tokens[ 1 ].ptr, tokens[ tokens.n - 1 ].end() - tokens[ 1 ].ptr );

		if( args.ptr[ -1 ] == '"' ) {
			args.ptr--;
			args.n++;
		}

		if( args.end() < line.end() && *args.end() == '"' ) {
			args.n++;
		}
	}

	const char * args_cstr = ( *a )( "{}", args );
	defer { FREE( a, args_cstr ); };

	const ConsoleCommand * command = FindCommand( tokens[ 0 ] );
	if( command != NULL && !command->disabled ) {
		command->callback( args, tokens );
		return true;
	}

	if( Cvar_Command( tokens ) ) {
		return true;
	}

	if( warn_on_invalid ) {
		Com_GGPrint( "Unknown command \"{}\"", tokens[ 0 ] );
	}

	return false;
}

void Cmd_ExecuteLine( const char * line ) {
	Cmd_ExecuteLine( MakeSpan( line ), true );
}

static void Cmd_Execute( const char * str, bool skip_comments ) {
	Span< const char > cursor = MakeSpan( str );
	while( cursor.n > 0 ) {
		bool eof;
		Span< const char > line = GrabLine( cursor, &eof );
		if( !skip_comments || !StartsWith( line, "//" ) ) {
			Cmd_ExecuteLine( line, true );
		}

		cursor += line.n;
		if( !eof ) {
			cursor++;
		}
	}
}

static const char * Argv( int argc, char ** argv, int i ) {
	return i < argc ? argv[ i ] : "";
}

static void ClearArg( int argc, char ** argv, int i ) {
	if( i < argc ) {
		argv[ i ] = NULL;
	}
}

void Cmd_ExecuteEarlyCommands( int argc, char ** argv ) {
	for( int i = 1; i < argc; i++ ) {
		if( StrCaseEqual( argv[ i ], "-set" ) || StrCaseEqual( argv[ i ], "+set" ) ) {
			Cmd_Execute( "set \"{}\" \"{}\"", Argv( argc, argv, i + 1 ), Argv( argc, argv, i + 2 ) );
			ClearArg( argc, argv, i );
			ClearArg( argc, argv, i + 1 );
			ClearArg( argc, argv, i + 2 );
			i += 2;
		}
		else if( StrCaseEqual( argv[ i ], "-exec" ) || StrCaseEqual( argv[ i ], "+exec" ) ) {
			Cmd_Execute( "exec \"{}\"", Argv( argc, argv, i + 1 ) );
			ClearArg( argc, argv, i );
			ClearArg( argc, argv, i + 1 );
			i += 1;
		}
		else if( StrCaseEqual( argv[ i ], "-config" ) || StrCaseEqual( argv[ i ], "+config" ) ) {
			Cmd_Execute( "config \"{}\"", Argv( argc, argv, i + 1 ) );
			ClearArg( argc, argv, i );
			ClearArg( argc, argv, i + 1 );
			i += 1;
		}
	}
}

void Cmd_ExecuteLateCommands( int argc, char ** argv ) {
	String< 1024 > buf;

	// TODO: should probably not roundtrip to string and retokenize
	for( int i = 1; i < argc; i++ ) {
		if( argv[ i ] == NULL )
			continue;

		if( StartsWith( argv[ i ], "-" ) || StartsWith( argv[ i ], "+" ) ) {
			Cmd_ExecuteLine( buf.c_str() );
			buf.format( "{}", argv[ i ] + 1 );
		}
		else {
			buf.append( " \"{}\"", argv[ i ] );
		}
	}

	Cmd_ExecuteLine( buf.c_str() );
}

static void ExecConfig( const char * path ) {
	TracyZoneScoped;

	char * config = ReadFileString( sys_allocator, path );
	defer { FREE( sys_allocator, config ); };
	if( config == NULL ) {
		Com_Printf( "Couldn't execute: %s\n", path );
		return;
	}

	Cmd_Execute( config, true );
}

static void Cmd_Exec_f() {
	if( Cmd_Argc() < 2 ) {
		Com_Printf( "Usage: exec <filename>\n" );
		return;
	}

	const char * fmt = is_public_build ? "{}/{}" : "{}/base/{}";
	DynamicString path( sys_allocator, fmt, HomeDirPath(), Cmd_Argv( 1 ) );
	if( FileExtension( path.c_str() ) == "" ) {
		path += ".cfg";
	}

	ExecConfig( path.c_str() );
}

static void Cmd_Config_f() {
	if( Cmd_Argc() < 2 ) {
		Com_Printf( "Usage: config <filename>\n" );
		return;
	}

	DynamicString path( sys_allocator, "{}", Cmd_Argv( 1 ) );
	if( FileExtension( path.c_str() ) == "" ) {
		path += ".cfg";
	}

	ExecConfig( path.c_str() );
}

static void Cmd_Find_f() {
	const char * needle = Cmd_Argc() < 2 ? "" : Cmd_Argv( 1 );

	Span< const char * > cmds = SearchCommands( sys_allocator, needle );
	Span< const char * > cvars = SearchCvars( sys_allocator, needle );
	defer {
		FREE( sys_allocator, cmds.ptr );
		FREE( sys_allocator, cvars.ptr );
	};

	if( cmds.n == 0 && cvars.n == 0 ) {
		Com_Printf( "No results.\n" );
		return;
	}

	if( cmds.n > 0 ) {
		Com_Printf( "Found %zu command%s:\n", cmds.n, cmds.n == 1 ? "" : "s" );

		for( const char * cvar : cmds ) {
			Com_Printf( "    %s\n", cvar );
		}
	}

	if( cvars.n > 0 ) {
		Com_Printf( "Found %zu cvar%s:\n", cvars.n, cvars.n == 1 ? "" : "s" );

		for( const char * cvar : cvars ) {
			Com_Printf( "    %s\n", cvar );
		}
	}
}

void ExecDefaultCfg() {
	DynamicString path( sys_allocator, "{}/base/default.cfg", RootDirPath() );
	ExecConfig( path.c_str() );
}

void AddCommand( const char * name, ConsoleCommandCallback callback ) {
	assert( callback != NULL );

	ConsoleCommand * old_command = FindCommand( name );
	if( old_command != NULL ) {
		assert( old_command->callback == callback && old_command->disabled );
		old_command->disabled = false;
		return;
	}

	assert( commands_hashtable.size() < ARRAY_COUNT( commands ) );

	ConsoleCommand * command = &commands[ commands_hashtable.size() ];
	*command = { };
	command->name = CopyString( sys_allocator, name );
	command->callback = callback;

	u64 hash = CaseHash64( name );
	bool ok = commands_hashtable.add( hash, commands_hashtable.size() );
	assert( ok );
}

void RemoveCommand( const char * name ) {
	FindCommand( name )->disabled = true;
}

void SetTabCompletionCallback( const char * name, TabCompletionCallback callback ) {
	ConsoleCommand * command = FindCommand( name );
	assert( command->tab_completion_callback == NULL || command->tab_completion_callback == callback );
	command->tab_completion_callback = callback;
}

Span< const char * > TabCompleteCommand( TempAllocator * a, const char * partial ) {
	NonRAIIDynamicArray< const char * > results( a );

	for( size_t i = 0; i < commands_hashtable.size(); i++ ) {
		const ConsoleCommand * command = &commands[ i ];
		if( !command->disabled && CaseStartsWith( command->name, partial ) ) {
			results.add( command->name );
		}
	}

	std::sort( results.begin(), results.end(), SortCStringsComparator );

	return results.span();
}

Span< const char * > SearchCommands( Allocator * a, const char * partial ) {
	NonRAIIDynamicArray< const char * > results( a );

	for( size_t i = 0; i < commands_hashtable.size(); i++ ) {
		const ConsoleCommand * command = &commands[ i ];
		if( !command->disabled && CaseContains( command->name, partial ) ) {
			results.add( command->name );
		}
	}

	std::sort( results.begin(), results.end(), SortCStringsComparator );

	return results.span();
}

Span< const char * > TabCompleteArgument( TempAllocator * a, const char * partial ) {
	Span< const char > partial_span = MakeSpan( partial );
	Span< const char > command_name = ParseToken( &partial_span, Parse_StopOnNewLine );

	const ConsoleCommand * command = FindCommand( command_name );
	if( command == NULL || command->tab_completion_callback == NULL ) {
		return Span< const char * >();
	}

	const char * first_arg = command_name.end();
	while( *first_arg == ' ' )
		first_arg++;
	return command->tab_completion_callback( a, first_arg );
}

static void FindMatchingFilesRecursive( TempAllocator * a, NonRAIIDynamicArray< const char * > * files, DynamicString * path, const char * prefix, size_t skip, const char * extension ) {
	ListDirHandle scan = BeginListDir( sys_allocator, path->c_str() );

	const char * name;
	bool dir;
	while( ListDirNext( &scan, &name, &dir ) ) {
		// skip ., .., .git, etc
		if( name[ 0 ] == '.' )
			continue;

		size_t old_len = path->length();
		path->append( "/{}", name );
		if( dir ) {
			FindMatchingFilesRecursive( a, files, path, prefix, skip, extension );
		}
		else {
			bool prefix_matches = CaseStartsWith( path->c_str() + skip, prefix );
			bool ext_matches = StrCaseEqual( FileExtension( path->c_str() ), extension );
			if( prefix_matches && ext_matches ) {
				files->add( ( *a )( "{}", path->span().slice( skip, path->length() ) ) );
			}
		}
		path->truncate( old_len );
	}
}

Span< const char * > TabCompleteFilename( TempAllocator * a, const char * partial, const char * search_dir, const char * extension ) {
	DynamicString base_path( sys_allocator, "{}", search_dir );

	NonRAIIDynamicArray< const char * > results( a );
	FindMatchingFilesRecursive( a, &results, &base_path, partial, base_path.length() + 1, extension );

	std::sort( results.begin(), results.end(), SortCStringsComparator );

	return results.span();
}

Span< const char * > TabCompleteFilenameHomeDir( TempAllocator * a, const char * partial, const char * search_dir, const char * extension ) {
	const char * home_search_dir = ( *a )( "{}/{}", HomeDirPath(), search_dir );
	return TabCompleteFilename( a, partial, home_search_dir, extension );
}

static Span< const char * > TabCompleteExec( TempAllocator * a, const char * partial ) {
	return TabCompleteFilenameHomeDir( a, partial, "base", ".cfg" );
}

static Span< const char * > TabCompleteConfig( TempAllocator * a, const char * partial ) {
	return TabCompleteFilename( a, partial, "base", ".cfg" );
}

void Cmd_Init() {
	AddCommand( "exec", Cmd_Exec_f );
	AddCommand( "config", Cmd_Config_f );
	AddCommand( "find", Cmd_Find_f );

	SetTabCompletionCallback( "exec", TabCompleteExec );
	SetTabCompletionCallback( "config", TabCompleteConfig );
}

void Cmd_Shutdown() {
	RemoveCommand( "exec" );
	RemoveCommand( "config" );
	RemoveCommand( "find" );

	for( size_t i = 0; i < commands_hashtable.size(); i++ ) {
		ConsoleCommand * command = &commands[ i ];
		assert( command->disabled );
		FREE( sys_allocator, command->name );
	}
}
